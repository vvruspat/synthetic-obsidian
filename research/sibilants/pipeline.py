from __future__ import annotations

import hashlib
from pathlib import Path

import librosa
import numpy as np
from scipy.signal import resample_poly
import torch
from torch import nn

SAMPLE_RATE = 24000
HOP = 240
N_FFT = 1024
MELS = 80
FEATURES = MELS + 6
DILATIONS = (1, 2, 4, 8, 16, 32)
CONTEXT_FRAMES = sum(DILATIONS)
LABEL_EDGE_GUARD = 1
SCHEMA = "synthetic-obsidian-sibilant-tcn-v1"


def pair_statistics(raw_path: Path, ess_path: Path):
    import soundfile as sf
    with sf.SoundFile(raw_path) as raw, sf.SoundFile(ess_path) as ess:
        if (raw.frames, raw.samplerate, raw.channels) != (ess.frames, ess.samplerate, ess.channels):
            raise ValueError("Aligned audio pair has different dimensions")
        if raw.samplerate % 100 or raw.frames <= 0:
            raise ValueError("Pair needs a positive length and an integer 10 ms sample grid")
        hop = raw.samplerate // 100
        result = {key: [] for key in ("raw_rms", "ess_rms", "correlation", "relative_difference")}
        while True:
            a = raw.read(hop * 1000, dtype="float32", always_2d=True)
            if not len(a):
                break
            b = ess.read(len(a), dtype="float32", always_2d=True)
            if not np.isfinite(a).all() or not np.isfinite(b).all():
                raise ValueError("Pair contains non-finite samples")
            count = (len(a) + hop - 1) // hop
            if len(a) % hop:
                pad = ((0, count * hop - len(a)), (0, 0))
                a, b = np.pad(a, pad), np.pad(b, pad)
            a, b = a.reshape(count, -1), b.reshape(count, -1)
            aa, bb = np.mean(a*a, axis=1, dtype=np.float64), np.mean(b*b, axis=1, dtype=np.float64)
            ab = np.mean(a*b, axis=1, dtype=np.float64)
            result["raw_rms"].append(np.sqrt(aa).astype(np.float32))
            result["ess_rms"].append(np.sqrt(bb).astype(np.float32))
            result["correlation"].append((ab / np.maximum(np.sqrt(aa*bb), 1e-20)).astype(np.float32))
            result["relative_difference"].append((np.sqrt(np.maximum(aa+bb-2*ab, 0)) / np.maximum(np.sqrt(bb), 1e-10)).astype(np.float32))
    return {key: np.concatenate(value) for key, value in result.items()}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def features(audio: np.ndarray, sample_rate: int) -> np.ndarray:
    if audio.ndim == 1:
        audio = audio[:, None]
    if sample_rate < 8000 or not len(audio) or not np.isfinite(audio).all():
        raise ValueError("Expected finite audio at >=8 kHz")
    channel = int(np.argmax(np.sum(audio.astype(np.float64) ** 2, axis=0)))
    mono = audio[:, channel].astype(np.float32)
    if sample_rate != SAMPLE_RATE:
        from math import gcd
        divisor = gcd(sample_rate, SAMPLE_RATE)
        mono = resample_poly(mono, SAMPLE_RATE // divisor, sample_rate // divisor).astype(np.float32)
    count = int(np.ceil(len(mono) / HOP))
    spectrum = librosa.stft(mono, n_fft=N_FFT, hop_length=HOP, center=True, pad_mode="constant")[:, :count]
    power = (spectrum.real ** 2 + spectrum.imag ** 2) / N_FFT ** 2
    mel = librosa.filters.mel(sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=MELS, fmin=50, fmax=11800) @ power
    log_mel = 10 * np.log10(np.maximum(mel, 1e-12))
    total = np.maximum(power.sum(axis=0), 1e-12)
    frequencies = librosa.fft_frequencies(sr=SAMPLE_RATE, n_fft=N_FFT)
    flatness = np.exp(np.log(np.maximum(power, 1e-12)).mean(axis=0)) / np.maximum(power.mean(axis=0), 1e-12)
    padded = np.pad(mono, (N_FFT // 2, N_FFT // 2))
    windows = np.lib.stride_tricks.sliding_window_view(padded, N_FFT)[::HOP][:count]
    zcr = np.mean(np.signbit(windows[:, 1:]) != np.signbit(windows[:, :-1]), axis=1)
    extra = np.stack((10 * np.log10(total), flatness,
                      power[frequencies >= 3500].sum(axis=0) / total,
                      power[frequencies >= 7000].sum(axis=0) / total,
                      (frequencies[:, None] * power).sum(axis=0) / total / (SAMPLE_RATE / 2), zcr))
    result = np.concatenate((log_mel, extra), axis=0).astype(np.float32)
    if not np.isfinite(result).all():
        raise ValueError("Non-finite spectral features")
    return result


def labels(raw_rms: np.ndarray, ess_rms: np.ndarray) -> np.ndarray:
    """Mask quiet fades and 10 ms around edited edges, retaining short consonants."""
    if raw_rms.shape != ess_rms.shape or raw_rms.ndim != 1 or not np.isfinite(raw_rms).all() or not np.isfinite(ess_rms).all():
        raise ValueError("Expected aligned finite RMS sequences")
    positive = (ess_rms >= 1e-5) & (ess_rms >= raw_rms * 0.001)
    negative = ess_rms <= 1e-7
    result = np.full(len(raw_rms), np.nan, dtype=np.float32)
    result[negative] = 0
    result[positive] = 1
    edges = np.diff(np.r_[False, positive, False].astype(np.int8))
    for start, end in zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1), strict=True):
        margin = min(LABEL_EDGE_GUARD, (end - start - 1) // 2)
        result[max(0, start - LABEL_EDGE_GUARD):start + margin] = np.nan
        result[end - margin:min(len(result), end + LABEL_EDGE_GUARD)] = np.nan
    return result


class Block(nn.Module):
    def __init__(self, channels: int, dilation: int):
        super().__init__()
        self.temporal = nn.Conv1d(channels, channels, 3, padding=dilation, dilation=dilation)
        self.mix = nn.Conv1d(channels, channels, 1)

    def forward(self, value):
        return torch.relu(value + self.mix(torch.relu(self.temporal(value))))


class SibilantNet(nn.Module):
    def __init__(self, channels=48):
        super().__init__()
        self.input = nn.Conv1d(FEATURES, channels, 1)
        self.blocks = nn.Sequential(*(Block(channels, d) for d in DILATIONS))
        self.output = nn.Conv1d(channels, 1, 1)

    def forward(self, value):
        return self.output(self.blocks(torch.relu(self.input(value)))).squeeze(1)


def frame_metrics(probabilities, targets, threshold):
    valid = np.isfinite(targets)
    truth, estimated = targets[valid] >= 0.5, probabilities[valid] >= threshold
    tp = int(np.sum(truth & estimated)); fp = int(np.sum(~truth & estimated)); fn = int(np.sum(truth & ~estimated))
    precision = tp / max(1, tp + fp); recall = tp / max(1, tp + fn)
    return {"precision": precision, "recall": recall, "f1": 2 * precision * recall / max(1e-12, precision + recall),
            "true_positive_seconds": tp / 100, "false_positive_seconds": fp / 100,
            "missed_seconds": fn / 100, "evaluated_seconds": int(valid.sum()) / 100}


def sibilant_note_coverage(note, probabilities, threshold):
    if not np.isfinite(note["start"]) or not np.isfinite(note["end"]) or note["start"] < 0 or note["end"] <= note["start"] or note["end"] > len(probabilities) / 100 + 1e-9:
        return 0.0
    lo = int(np.floor(note["start"] * 100))
    hi = int(np.ceil(note["end"] * 100 - 1e-9))
    if hi <= lo or hi > len(probabilities):
        return 0.0
    frames = np.arange(lo, hi)
    weights = np.maximum(0, np.minimum(note["end"], (frames + 1) / 100) - np.maximum(note["start"], frames / 100))
    return float(np.sum(weights[probabilities[lo:hi] >= threshold]) / (note["end"] - note["start"]))


def has_voiced_core(note, probabilities, threshold):
    if not np.isfinite(note["start"]) or not np.isfinite(note["end"]) or note["end"] <= note["start"] or not np.isfinite(threshold) or not 0 < threshold < 1:
        return False
    required = min(0.08, max(0.03, (note["end"] - note["start"]) * 0.25))
    run_start = previous = -1.0
    for point in note.get("curve", []):
        time, midi, confidence = point["time"], point["midi"], point["confidence"]
        if not np.isfinite(time) or time < note["start"] or time >= note["end"] or time < 0 or not np.isfinite(midi) or not np.isfinite(confidence) or not 0.8 <= confidence <= 1.0 or time * 100 >= len(probabilities):
            run_start = previous = -1.0
            continue
        frame = int(np.floor(time * 100 + 1e-9))
        if frame >= len(probabilities) or not np.isfinite(probabilities[frame]) or probabilities[frame] >= threshold:
            run_start = previous = -1.0
            continue
        if previous < 0 or time <= previous or time - previous > 0.015:
            run_start = time
        previous = time
        if min(note["end"], time + 0.01) - run_start >= required - 1e-9:
            return True
    return False


def merge_sibilant_notes(notes, probabilities, threshold, minimum_coverage=0.50, max_gap=0.12):
    """Attach notes above the sibilant time fraction; equal gaps favour previous."""
    import copy
    output = copy.deepcopy(notes)
    proposals = []
    if not np.isfinite(threshold) or not 0 < threshold < 1 or not np.isfinite(minimum_coverage) or not 0 < minimum_coverage <= 1 or not np.all(np.isfinite(probabilities)) or np.any((probabilities < 0) | (probabilities > 1)):
        return output, proposals
    original_coverage = {n["id"]: sibilant_note_coverage(n, probabilities, threshold) for n in output}
    sibilant = {n["id"]: original_coverage[n["id"]] > minimum_coverage + 1e-9 and not has_voiced_core(n, probabilities, threshold) for n in output}
    removed = set()
    for index, source in enumerate(notes):
        fraction = original_coverage[source["id"]]
        if not sibilant[source["id"]]:
            continue
        candidates = []
        for neighbour, target in enumerate(output):
            if sibilant[target["id"]]:
                continue
            direction = "previous" if neighbour < index else "next"
            gap = max(0.0, source["start"] - target["end"], target["start"] - source["end"])
            if gap <= max_gap:
                candidates.append((gap, direction, neighbour))
        candidates.sort(key=lambda item: (round(item[0], 9), item[2]))
        if not candidates:
            proposals.append({"source_id": source["id"], "start": source["start"], "end": source["end"],
                              "coverage": fraction, "action": "kept_no_nearby_vocal"})
            continue
        _, direction, neighbour = candidates[0]
        target = output[neighbour]
        proposals.append({"source_id": source["id"], "target_id": target["id"], "start": source["start"],
                          "end": source["end"], "coverage": fraction, "action": "merge", "direction": direction})
        target["start"] = min(target["start"], source["start"])
        target["end"] = max(target["end"], source["end"])
        if "sibilant_attached" not in target.setdefault("flags", []):
            target["flags"].append("sibilant_attached")
        removed.add(source["id"])
    return [n for n in output if n["id"] not in removed], proposals
