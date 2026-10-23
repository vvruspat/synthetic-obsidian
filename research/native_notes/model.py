"""Small TCN and portable feature/decoding contract shared with the C++ runner."""
from __future__ import annotations

import numpy as np
import librosa
import torch
from torch import nn

from research.gtsinger_alignment.train_baseline import BoundaryNet

SCHEMA = "synthetic-obsidian-note-boundary-tcn-v1"
SAMPLE_RATE = 16000
HOP = 160
N_FFT = 512
MELS = 64
DILATIONS = (1, 2, 4, 8)
HEADS = ("onset", "offset", "activity")
DRAFT_POLICY = "raw_boundary_intervals"


class NoteBoundaryNet(BoundaryNet):
    def __init__(self):
        super().__init__(MELS)
        self.output_heads = nn.Conv1d(64, len(HEADS), 1)

    def initialize_encoder(self, checkpoint: dict) -> None:
        state = checkpoint["model_state"]
        encoder = {key: value for key, value in state.items() if not key.startswith("output_heads.")}
        missing, unexpected = self.load_state_dict(encoder, strict=False)
        if set(missing) != {"output_heads.weight", "output_heads.bias"} or unexpected:
            raise ValueError("Pretrained encoder architecture does not match the native TCN")
        if checkpoint["model"]["event_names"] != ("phoneme", "syllable", "breath", "silence"):
            raise ValueError("Unsupported pretraining output contract")
        with torch.no_grad():
            self.output_heads.weight[0].copy_(state["output_heads.weight"][1])
            self.output_heads.bias[0].copy_(state["output_heads.bias"][1])
            self.output_heads.weight[2].copy_(-state["output_heads.weight"][2:4].mean(0))
            self.output_heads.bias[2].copy_(-state["output_heads.bias"][2:4].mean(0))


def mel_filters() -> np.ndarray:
    return librosa.filters.mel(sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=MELS,
                              fmin=40.0, fmax=7600.0).astype(np.float32)


def features(audio: np.ndarray, sample_rate: int) -> np.ndarray:
    """No hidden resampling library: the identical operation is implemented in JUCE.

    Strongest channel, linear resampling, centered periodic Hann FFT, Slaney mel,
    80-dB dynamic range, per-clip/per-bin normalization. Returns [mel, frame].
    """
    if audio.ndim == 1:
        audio = audio[:, None]
    if not len(audio) or not np.isfinite(audio).all() or sample_rate < 8000:
        raise ValueError("Invalid audio")
    channel = int(np.argmax(np.square(audio, dtype=np.float64).sum(axis=0)))
    mono = audio[:, channel]
    count = int(np.ceil(len(mono) * SAMPLE_RATE / sample_rate))
    positions = np.arange(count, dtype=np.float64) * sample_rate / SAMPLE_RATE
    mono = np.interp(positions, np.arange(len(mono)), mono).astype(np.float32)
    padded = np.pad(mono, (N_FFT // 2, N_FFT // 2))
    windows = np.lib.stride_tricks.sliding_window_view(padded, N_FFT)[::HOP]
    hann = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N_FFT) / N_FFT)).astype(np.float32)
    power = np.abs(np.fft.rfft(windows * hann)).astype(np.float64) ** 2
    mel = mel_filters().astype(np.float64) @ power.T
    db = (10 * np.log10(np.maximum(mel, 1e-10))).astype(np.float32)
    db = np.maximum(db, db.max() - 80.0)
    mean = db.mean(axis=1, keepdims=True, dtype=np.float64)
    std = db.std(axis=1, keepdims=True, dtype=np.float64)
    return ((db - mean) / np.maximum(std, 1e-5)).astype(np.float32)


def targets(notes: list[dict], frame_count: int) -> np.ndarray:
    output = np.zeros((3, frame_count), dtype=np.float32)
    for note in notes:
        start, end = float(note["start"]), float(note["end"])
        begin = max(0, int(np.ceil(start * 100 - 1e-6)))
        stop = min(frame_count, int(np.ceil(end * 100 - 1e-6)))
        output[2, begin:stop] = 1.0
        for head, time in ((0, start), (1, end)):
            center = int(np.floor(time * 100 + 0.5))
            for delta in range(-3, 4):
                frame = center + delta
                if 0 <= frame < frame_count:
                    output[head, frame] = max(output[head, frame], np.exp(-0.5 * (delta / 1.5) ** 2))
    return output


def peaks(values: np.ndarray, threshold: float, distance: int = 8) -> list[int]:
    candidates = np.flatnonzero((values >= threshold)
        & (values >= np.r_[0.0, values[:-1]]) & (values > np.r_[values[1:], 0.0]))
    selected: list[int] = []
    for index in sorted(candidates.tolist(), key=lambda i: (-float(values[i]), i)):
        if all(abs(index - previous) >= distance for previous in selected):
            selected.append(index)
    return sorted(selected)


def decode(probabilities: np.ndarray, duration: float,
           thresholds: tuple[float, float, float]) -> list[tuple[float, float]]:
    """Three heads form monophonic note intervals; no pitch labels are required."""
    onsets = peaks(probabilities[0], thresholds[0])
    offsets = peaks(probabilities[1], thresholds[1])
    active = probabilities[2] >= thresholds[2]
    # Close inactive holes shorter than 40 ms without moving attacks to waveform peaks.
    active = active.copy()
    gaps = np.diff(np.r_[True, active, True].astype(np.int8))
    gap_starts = np.flatnonzero(gaps == -1)
    gap_ends = np.flatnonzero(gaps == 1)
    for start, end in zip(gap_starts, gap_ends, strict=True):
        if end - start <= 4 and start > 0 and end < len(active):
            active[start:end] = True
    edges = np.diff(np.r_[False, active, False].astype(np.int8))
    output = []
    for left, right in zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1), strict=True):
        if right - left < 8:
            continue
        starts = [x for x in onsets if left - 12 <= x < right - 3]
        if not starts:
            starts = [int(left)]
        elif starts[0] > left + 15:
            starts.insert(0, int(left))
        for index, start in enumerate(starts):
            start = max(0, int(start))
            limit = starts[index + 1] if index + 1 < len(starts) else min(len(active), right + 12)
            expected_end = min(limit, int(right))
            ends = [x for x in offsets if start + 5 <= x <= limit and abs(x - expected_end) <= 20]
            end = min(ends, key=lambda x: (abs(x - expected_end), -float(probabilities[1, x]))) if ends else expected_end
            end_time = min(duration, end * 0.01)
            start_time = start * 0.01
            if end_time - start_time >= 0.05:
                if output and start_time < output[-1][1]:
                    start_time = output[-1][1]
                if end_time - start_time >= 0.05:
                    output.append((start_time, end_time))
    return output


def matched_events(reference: list[float], estimated: list[float], tolerance: float = 0.05) -> tuple[int, int, int]:
    """Maximum-cardinality matching for sorted one-dimensional event sequences."""
    expected, predicted = sorted(reference), sorted(estimated)
    i = j = matched = 0
    while i < len(expected) and j < len(predicted):
        if abs(expected[i] - predicted[j]) <= tolerance + 1e-9:
            matched += 1; i += 1; j += 1
        elif predicted[j] < expected[i]:
            j += 1
        else:
            i += 1
    return matched, len(predicted) - matched, len(expected) - matched


def connected_boundaries(predictions, examples):
    """Recall at adjacent gold notes without a pause; this is a legato proxy.

    It deliberately uses start/end only, not uncorrected pitch/voicing curves.
    A repeated attack without silence also belongs to this subset.
    """
    onset_matched, offset_matched, total = 0, 0, 0
    for intervals, example in zip(predictions, examples, strict=True):
        notes = sorted(example["notes"], key=lambda n: n["start"])
        pairs = [(left["end"], right["start"]) for left, right in zip(notes, notes[1:])
                 if 0 <= right["start"] - left["end"] <= 0.03 + 1e-9]
        total += len(pairs)
        onset_matched += matched_events([p[1] for p in pairs], [p[0] for p in intervals])[0]
        offset_matched += matched_events([p[0] for p in pairs], [p[1] for p in intervals])[0]
    return {"reference_transitions": total, "onsets_matched": onset_matched,
            "onsets_missed": total - onset_matched, "onset_recall": onset_matched / max(1, total),
            "offsets_matched": offset_matched, "offsets_missed": total - offset_matched,
            "offset_recall": offset_matched / max(1, total)}


def scores(counts: tuple[int, int, int]) -> dict:
    true, false, missed = map(int, counts)
    precision = true / max(1, true + false)
    recall = true / max(1, true + missed)
    return {"precision": precision, "recall": recall,
            "f1": 2 * true / max(1, 2 * true + false + missed),
            "matched": true, "extra": false, "missed": missed}
