#!/usr/bin/env python3
"""Forced-align supplied lyrics to audio with torchaudio CTC alignment."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def normalize_word(word: str) -> str:
    cleaned = re.sub(r"[^A-Za-z']", "", word).strip("'").upper()
    return cleaned


def display_word(word: str) -> str:
    return re.sub(r"[^A-Za-z']", "", word).strip("'")


def load_torchaudio():
    try:
        import torch
        import torchaudio
    except Exception as exc:  # pragma: no cover - surfaced in app status
        raise RuntimeError(f"Could not import torch/torchaudio: {exc}") from exc

    return torch, torchaudio


def build_targets(
    words: list[str],
    dictionary: dict[str, int],
    word_indices: list[int] | None = None,
) -> tuple[list[int], list[int | None], str]:
    target_ids: list[int] = []
    target_word_indices: list[int | None] = []
    transcript_parts: list[str] = []

    if word_indices is None:
        word_indices = list(range(len(words)))

    for local_index, word in enumerate(words):
        if transcript_parts:
            separator = "|"
            if separator in dictionary:
                target_ids.append(dictionary[separator])
                target_word_indices.append(None)
                transcript_parts.append(separator)

        for character in word:
            if character not in dictionary:
                continue

            target_ids.append(dictionary[character])
            target_word_indices.append(word_indices[local_index])
            transcript_parts.append(character)

    return target_ids, target_word_indices, "".join(transcript_parts)


def lyric_pairs_from_text(text: str) -> list[tuple[str, str]]:
    pairs = [(display_word(token), normalize_word(token)) for token in re.split(r"\s+", text)]
    return [(display, normalized) for display, normalized in pairs if display and normalized]


def lyric_line_chunks(text: str, normalized_words: list[str]) -> list[list[int]]:
    chunks: list[list[int]] = []
    word_cursor = 0
    for line in text.splitlines():
        pairs = lyric_pairs_from_text(line)
        if not pairs:
            continue

        indices = list(range(word_cursor, min(word_cursor + len(pairs), len(normalized_words))))
        if indices:
            chunks.append(indices)
        word_cursor += len(pairs)

    if not chunks:
        chunks = [list(range(len(normalized_words)))]

    return chunks


def repeated_token_count(target_ids: list[int]) -> int:
    return sum(1 for previous, current in zip(target_ids, target_ids[1:]) if previous == current)


def align_target_to_emissions(torch, torchaudio, log_probs, target_ids, target_word_indices, frame_offset, time_per_frame):
    targets = torch.tensor([target_ids], dtype=torch.int32)
    if log_probs.shape[1] < targets.shape[1] + repeated_token_count(target_ids):
        raise ValueError("emission window is too short for lyric phrase")

    aligned_tokens, scores = torchaudio.functional.forced_align(log_probs, targets, blank=0)
    token_spans = torchaudio.functional.merge_tokens(aligned_tokens[0], scores[0], blank=0)

    word_data: dict[int, dict[str, float | int | None]] = {}
    target_cursor = 0
    for span in token_spans:
        while target_cursor < len(target_ids) and target_ids[target_cursor] != span.token:
            target_cursor += 1

        if target_cursor >= len(target_ids):
            break

        word_index = target_word_indices[target_cursor]
        if word_index is not None:
            item = word_data.setdefault(word_index, {"start": None, "end": None, "score_sum": 0.0, "score_count": 0})
            start = (frame_offset + span.start) * time_per_frame
            end = (frame_offset + span.end) * time_per_frame
            item["start"] = start if item["start"] is None else min(float(item["start"]), start)
            item["end"] = end if item["end"] is None else max(float(item["end"]), end)
            item["score_sum"] = float(item["score_sum"]) + float(span.score)
            item["score_count"] = int(item["score_count"]) + 1

        target_cursor += 1

    return word_data


def energy_cdf_for_waveform(torch, waveform, total_frames: int):
    mono = waveform.abs().mean(dim=0)
    if mono.numel() == 0:
        return torch.linspace(0.0, 1.0, total_frames), 0, max(0, total_frames - 1)

    frame_size = max(1, mono.numel() // total_frames)
    padded = torch.nn.functional.pad(mono, (0, max(0, frame_size * total_frames - mono.numel())))
    framed = padded[: frame_size * total_frames].reshape(total_frames, frame_size)
    energy = framed.mean(dim=1)
    smoothed = torch.nn.functional.avg_pool1d(energy[None, None, :], kernel_size=13, stride=1, padding=6)[0, 0]

    peak = smoothed.max().clamp_min(1e-6)
    threshold = max(float(peak) * 0.035, float(torch.quantile(smoothed, 0.65)) * 0.75)
    active = smoothed > threshold
    active_indices = torch.nonzero(active, as_tuple=False).flatten()
    if active_indices.numel() > 0:
        active_start = max(0, int(active_indices[0].item()) - 5)
        active_end = min(total_frames - 1, int(active_indices[-1].item()) + 5)
    else:
        active_start = 0
        active_end = max(0, total_frames - 1)

    active_energy = smoothed[active_start : active_end + 1]
    active_energy = active_energy + peak * 0.012
    cdf = torch.cumsum(active_energy, dim=0)
    return cdf / cdf[-1].clamp_min(1e-6), active_start, active_end


def frame_for_fraction(torch, cdf, fraction: float, frame_offset: int) -> int:
    fraction = min(1.0, max(0.0, fraction))
    index = torch.searchsorted(cdf, torch.tensor(fraction, device=cdf.device)).item()
    return frame_offset + int(min(max(index, 0), cdf.numel() - 1))


def cdf_at_frame(cdf, frame: int, frame_offset: int) -> float:
    index = min(max(frame - frame_offset, 0), cdf.numel() - 1)
    return float(cdf[index])


def frame_for_energy_fraction(torch, cdf, start_frame: int, end_frame: int, fraction: float, frame_offset: int) -> int:
    if end_frame <= start_frame + 1:
        return start_frame

    start_index = min(max(start_frame - frame_offset, 0), cdf.numel() - 1)
    end_index = min(max(end_frame - frame_offset, 0), cdf.numel() - 1)
    if end_index <= start_index:
        return int(round(start_frame + (end_frame - start_frame) * min(1.0, max(0.0, fraction))))

    start_value = float(cdf[start_index - 1]) if start_index > 0 else 0.0
    end_value = float(cdf[end_index])
    if end_value <= start_value + 1e-6:
        return int(round(start_frame + (end_frame - start_frame) * min(1.0, max(0.0, fraction))))

    target = start_value + (end_value - start_value) * min(1.0, max(0.0, fraction))
    index = torch.searchsorted(cdf, torch.tensor(target, device=cdf.device)).item()
    index = int(min(max(index, start_index), end_index))
    return frame_offset + index


def word_duration_weight(word: str) -> int:
    vowel_groups = re.findall(r"[AEIOUY]+", word)
    return max(1, len(word) + 2 * len(vowel_groups))


def distribute_chunk_words(chunk: list[int], normalized_words: list[str], start_time: float, end_time: float):
    duration = max(0.05, end_time - start_time)
    weights = [word_duration_weight(normalized_words[index]) for index in chunk]
    total = max(1, sum(weights))
    cursor = start_time
    word_data: dict[int, dict[str, float | int | None]] = {}
    for index, weight in zip(chunk, weights):
        word_duration = duration * weight / total
        word_data[index] = {
            "start": cursor,
            "end": min(end_time, cursor + max(0.05, word_duration)),
            "score_sum": -8.0,
            "score_count": 1,
        }
        cursor += word_duration

    return word_data


def distribute_chunk_words_by_energy(
    torch,
    chunk: list[int],
    normalized_words: list[str],
    start_time: float,
    end_time: float,
    time_per_frame: float,
    energy_cdf,
    energy_frame_offset: int,
):
    if not chunk:
        return {}

    if end_time <= start_time + 0.04:
        return distribute_chunk_words(chunk, normalized_words, start_time, max(start_time + 0.05, end_time))

    start_frame = max(0, int(round(start_time / time_per_frame)))
    end_frame = max(start_frame + 1, int(round(end_time / time_per_frame)))
    weights = [word_duration_weight(normalized_words[index]) for index in chunk]
    total = max(1, sum(weights))
    cumulative = 0
    word_data: dict[int, dict[str, float | int | None]] = {}

    for index, weight in zip(chunk, weights):
        word_start_frame = frame_for_energy_fraction(torch, energy_cdf, start_frame, end_frame, cumulative / total, energy_frame_offset)
        cumulative += weight
        word_end_frame = frame_for_energy_fraction(torch, energy_cdf, start_frame, end_frame, cumulative / total, energy_frame_offset)
        word_start = max(start_time, word_start_frame * time_per_frame)
        word_end = min(end_time, max(word_start + 0.05, word_end_frame * time_per_frame))
        word_data[index] = {
            "start": word_start,
            "end": word_end,
            "score_sum": -6.0,
            "score_count": 1,
        }

    return word_data


def confidence_for_item(item) -> float:
    return float(item["score_sum"]) / max(1, int(item["score_count"]))


def usable_anchor(item, window_start: float, window_end: float) -> bool:
    if item is None or item["start"] is None or item["end"] is None:
        return False

    start = float(item["start"])
    end = float(item["end"])
    duration = end - start
    if start < window_start - 0.25 or end > window_end + 0.25:
        return False
    if duration < 0.035 or duration > max(1.0, (window_end - window_start) * 0.75):
        return False

    return confidence_for_item(item) >= -2.35


def fill_chunk_from_anchors(
    torch,
    chunk: list[int],
    normalized_words: list[str],
    base_word_data,
    raw_word_data,
    window_start: float,
    window_end: float,
    time_per_frame: float,
    energy_cdf,
    energy_frame_offset: int,
):
    anchors: list[int] = []
    previous_end = window_start - 0.01
    for index in chunk:
        item = raw_word_data.get(index)
        if not usable_anchor(item, window_start, window_end):
            continue

        start = float(item["start"])
        end = float(item["end"])
        if start < previous_end - 0.03:
            continue

        anchors.append(index)
        previous_end = end

    if not anchors:
        return {index: base_word_data[index] for index in chunk if index in base_word_data}

    result: dict[int, dict[str, float | int | None]] = {}

    def fill_run(indices: list[int], start_time: float, end_time: float):
        if not indices:
            return
        if end_time <= start_time + 0.04:
            end_time = start_time + 0.05 * len(indices)
        result.update(
            distribute_chunk_words_by_energy(
                torch,
                indices,
                normalized_words,
                max(window_start, start_time),
                min(window_end, end_time),
                time_per_frame,
                energy_cdf,
                energy_frame_offset,
            )
        )

    cursor = 0
    previous_boundary = window_start
    for anchor_index in anchors:
        anchor_position = chunk.index(anchor_index)
        anchor = dict(raw_word_data[anchor_index])
        anchor["start"] = max(window_start, float(anchor["start"]))
        anchor["end"] = min(window_end, max(float(anchor["start"]) + 0.05, float(anchor["end"])))
        fill_run(chunk[cursor:anchor_position], previous_boundary, float(anchor["start"]) - 0.025)
        result[anchor_index] = anchor
        previous_boundary = float(anchor["end"]) + 0.025
        cursor = anchor_position + 1

    fill_run(chunk[cursor:], previous_boundary, window_end)

    return result


def enforce_monotonic_word_data(word_data, word_count: int):
    ordered_indices = [index for index in range(word_count) if index in word_data]
    for left_index, right_index in zip(ordered_indices, ordered_indices[1:]):
        left = word_data[left_index]
        right = word_data[right_index]
        if left["end"] is None or right["start"] is None:
            continue

        max_left_end = float(right["start"]) - 0.01
        if float(left["end"]) > max_left_end:
            left["end"] = max(float(left["start"]) + 0.03, max_left_end)

    previous_end = -1.0
    for index in ordered_indices:
        item = word_data[index]
        if item["start"] is None or item["end"] is None:
            continue

        start = float(item["start"])
        end = float(item["end"])
        if start < previous_end + 0.01:
            start = previous_end + 0.01
            end = max(start + 0.03, end)

        item["start"] = start
        item["end"] = max(start + 0.03, end)
        previous_end = float(item["end"])

    return word_data


def repair_chunk_word_data(chunk, normalized_words, word_data, expected_start_time, expected_end_time):
    if not word_data:
        return distribute_chunk_words(chunk, normalized_words, expected_start_time, expected_end_time), True

    ordered = [word_data.get(index) for index in chunk]
    missing = any(item is None for item in ordered)
    starts = [float(item["start"]) for item in ordered if item is not None and item["start"] is not None]
    ends = [float(item["end"]) for item in ordered if item is not None and item["end"] is not None]
    non_monotonic = any(b < a + 0.025 for a, b in zip(starts, starts[1:]))
    phrase_duration = max(0.05, expected_end_time - expected_start_time)
    overlong = any((end - start) > max(1.2, phrase_duration * 0.65) for start, end in zip(starts, ends))
    outside_window = any(start < expected_start_time - 0.35 or end > expected_end_time + 0.35 for start, end in zip(starts, ends))
    low_confidence = False
    if ordered and all(item is not None for item in ordered):
        average_score = sum(float(item["score_sum"]) / max(1, int(item["score_count"])) for item in ordered) / len(ordered)
        low_confidence = average_score < -3.5

    if missing or non_monotonic or overlong or outside_window or low_confidence:
        return distribute_chunk_words(chunk, normalized_words, expected_start_time, expected_end_time), True

    repaired: dict[int, dict[str, float | int | None]] = {}
    cursor = expected_start_time
    for index in chunk:
        item = dict(word_data[index])
        item["start"] = max(cursor, float(item["start"]))
        item["end"] = max(float(item["start"]) + 0.05, float(item["end"]))
        item["end"] = min(item["end"], expected_end_time)
        cursor = float(item["end"]) + 0.025
        repaired[index] = item

    if any(float(item["end"]) <= float(item["start"]) for item in repaired.values()):
        return distribute_chunk_words(chunk, normalized_words, expected_start_time, expected_end_time), True

    return repaired, False


def align_full(torch, torchaudio, log_probs, normalized_words, dictionary, time_per_frame):
    target_ids, target_word_indices, normalized_transcript = build_targets(normalized_words, dictionary)
    if not target_ids:
        raise SystemExit("Lyrics contain no characters supported by the alignment model.")

    if log_probs.shape[1] < len(target_ids) + repeated_token_count(target_ids):
        raise SystemExit(
            f"Audio is too short for supplied lyrics: {log_probs.shape[1]} emission frames, "
            f"{len(target_ids)} lyric tokens."
        )

    return align_target_to_emissions(torch, torchaudio, log_probs, target_ids, target_word_indices, 0, time_per_frame), normalized_transcript


def align_by_chunks(
    torch,
    torchaudio,
    log_probs,
    normalized_words,
    chunks,
    dictionary,
    time_per_frame,
    energy_cdf,
    energy_frame_offset: int,
):
    token_counts = []
    for chunk in chunks:
        target_ids, _, _ = build_targets([normalized_words[i] for i in chunk], dictionary, chunk)
        token_counts.append(max(1, len(target_ids)))

    total_tokens = sum(token_counts)
    if total_tokens <= 0:
        raise ValueError("no lyric tokens")

    total_frames = log_probs.shape[1]
    overlap_frames = max(50, int(1.25 / time_per_frame))
    cumulative_tokens = 0
    raw_word_data_by_index: dict[int, dict[str, float | int | None]] = {}
    merged_word_data: dict[int, dict[str, float | int | None]] = {}
    chunk_windows: list[tuple[list[int], float, float]] = []
    transcript_parts: list[str] = []
    repaired_chunks = 0

    for chunk, token_count in zip(chunks, token_counts):
        target_ids, target_word_indices, transcript = build_targets([normalized_words[i] for i in chunk], dictionary, chunk)
        if not target_ids:
            cumulative_tokens += token_count
            continue

        expected_start = frame_for_fraction(torch, energy_cdf, cumulative_tokens / total_tokens, energy_frame_offset)
        cumulative_tokens += token_count
        expected_end = frame_for_fraction(torch, energy_cdf, cumulative_tokens / total_tokens, energy_frame_offset)
        expected_end = max(expected_start + 1, expected_end)
        expected_start_time = expected_start * time_per_frame
        expected_end_time = expected_end * time_per_frame
        chunk_windows.append((chunk, expected_start_time, expected_end_time))
        frame_start = max(0, expected_start - overlap_frames)
        frame_end = min(total_frames, expected_end + overlap_frames)

        minimum_frames = len(target_ids) + repeated_token_count(target_ids) + 4
        if frame_end - frame_start < minimum_frames:
            pad = (minimum_frames - (frame_end - frame_start)) // 2 + 1
            frame_start = max(0, frame_start - pad)
            frame_end = min(total_frames, frame_end + pad)

        try:
            word_data = align_target_to_emissions(
                torch,
                torchaudio,
                log_probs[:, frame_start:frame_end, :],
                target_ids,
                target_word_indices,
                frame_start,
                time_per_frame,
            )
        except Exception:
            word_data = {}

        raw_word_data_by_index.update(word_data)
        word_data, repaired = repair_chunk_word_data(
            chunk,
            normalized_words,
            word_data,
            expected_start_time,
            expected_end_time,
        )
        repaired_chunks += int(repaired)
        merged_word_data.update(word_data)
        transcript_parts.append(transcript)

    for chunk, window_start, window_end in chunk_windows:
        anchored_chunk = fill_chunk_from_anchors(
            torch,
            chunk,
            normalized_words,
            merged_word_data,
            raw_word_data_by_index,
            window_start,
            window_end,
            time_per_frame,
            energy_cdf,
            energy_frame_offset,
        )
        if anchored_chunk:
            merged_word_data.update(anchored_chunk)

    enforce_monotonic_word_data(merged_word_data, len(normalized_words))

    if len(merged_word_data) < max(1, int(len(normalized_words) * 0.45)):
        raise ValueError("chunk alignment produced too few words")

    return merged_word_data, "|".join(transcript_parts), repaired_chunks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio", type=Path)
    parser.add_argument("lyrics", type=Path)
    parser.add_argument("--model", choices=["base", "large"], default="base")
    args = parser.parse_args()

    if not args.audio.exists():
        raise SystemExit(f"Audio file does not exist: {args.audio}")
    if not args.lyrics.exists():
        raise SystemExit(f"Lyrics file does not exist: {args.lyrics}")

    torch, torchaudio = load_torchaudio()

    raw_text = args.lyrics.read_text(encoding="utf-8", errors="ignore")
    pairs = lyric_pairs_from_text(raw_text)
    display_words = [display for display, _ in pairs]
    normalized_words = [normalized for _, normalized in pairs]
    if not normalized_words:
        raise SystemExit("Lyrics file contains no alignable words.")
    chunks = lyric_line_chunks(raw_text, normalized_words)

    bundle = (
        torchaudio.pipelines.WAV2VEC2_ASR_LARGE_960H
        if args.model == "large"
        else torchaudio.pipelines.WAV2VEC2_ASR_BASE_960H
    )
    labels = bundle.get_labels()
    dictionary = {label: index for index, label in enumerate(labels)}

    waveform, sample_rate = torchaudio.load(str(args.audio))
    if waveform.shape[0] > 1:
        waveform = waveform.mean(dim=0, keepdim=True)

    if sample_rate != bundle.sample_rate:
        waveform = torchaudio.functional.resample(waveform, sample_rate, bundle.sample_rate)
        sample_rate = bundle.sample_rate

    model = bundle.get_model().eval()
    with torch.inference_mode():
        emissions, _ = model(waveform)
        log_probs = torch.nn.functional.log_softmax(emissions, dim=-1)

    time_per_frame = waveform.shape[1] / sample_rate / log_probs.shape[1]
    energy_cdf, energy_frame_start, energy_frame_end = energy_cdf_for_waveform(torch, waveform, log_probs.shape[1])
    try:
        word_data_by_index, normalized_transcript, repaired_chunks = align_by_chunks(
            torch,
            torchaudio,
            log_probs,
            normalized_words,
            chunks,
            dictionary,
            time_per_frame,
            energy_cdf,
            energy_frame_start,
        )
        alignment_mode = "chunked_energy"
    except Exception:
        word_data_by_index, normalized_transcript = align_full(
            torch,
            torchaudio,
            log_probs,
            normalized_words,
            dictionary,
            time_per_frame,
        )
        repaired_chunks = 0
        alignment_mode = "full"

    aligned_words = []
    for index, item in sorted(word_data_by_index.items()):
        if item["start"] is None or item["end"] is None:
            continue

        score_count = max(1, int(item["score_count"]))
        aligned_words.append(
            {
                "index": index,
                "text": display_words[index],
                "normalized": normalized_words[index],
                "start": float(item["start"]),
                "end": float(item["end"]),
                "confidence": float(item["score_sum"]) / score_count,
            }
        )

    print(
        json.dumps(
            {
                "model": args.model,
                "alignment_mode": alignment_mode,
                "chunk_count": len(chunks),
                "repaired_chunk_count": repaired_chunks,
                "sample_rate": sample_rate,
                "active_start": energy_frame_start * time_per_frame,
                "active_end": energy_frame_end * time_per_frame,
                "normalized_transcript": normalized_transcript,
                "word_count": len(normalized_words),
                "aligned_word_count": len(aligned_words),
                "words": aligned_words,
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
