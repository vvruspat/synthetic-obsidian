from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import soundfile as sf
import torch
import torch.nn.functional as F
import torchaudio
from torch.utils.data import Dataset


@dataclass(frozen=True)
class ChunkEntry:
    chunk_id: str
    source_id: str
    audio_path: Path
    label_path: Path
    split: str
    start: float
    end: float
    duration_seconds: float
    note_start_index: int
    note_end_index: int


class NoteDetectorDataset(Dataset):
    """Dataset for local vocal+MIDI chunks prepared by import_midi_vox_dataset.py."""

    def __init__(
        self,
        manifest_path: str | Path,
        split: str = "train",
        sample_rate: int = 24000,
        n_fft: int = 1024,
        hop_length: int = 256,
        n_mels: int = 80,
        min_duration_seconds: float = 0.15,
        midi_min: int = 36,
        midi_max: int = 84,
        onset_radius_frames: int = 2,
        alignment_pad_frames: int = 1,
    ) -> None:
        self.manifest_path = Path(manifest_path)
        self.sample_rate = sample_rate
        self.hop_length = hop_length
        self.n_mels = n_mels
        self.n_fft = n_fft
        self.min_duration_seconds = min_duration_seconds
        self.midi_min = midi_min
        self.midi_max = midi_max
        self.n_pitch_bins = midi_max - midi_min + 1
        self.onset_radius_frames = onset_radius_frames
        self.alignment_pad_frames = alignment_pad_frames

        if not self.manifest_path.exists():
            raise FileNotFoundError(f"Manifest not found: {self.manifest_path}")

        self.entries = self._load_manifest(split)
        if not self.entries:
            raise ValueError(f"No entries found for split={split}: {self.manifest_path}")

        self.label_cache: dict[Path, dict[str, Any]] = {}
        self.mel_transform = torchaudio.transforms.MelSpectrogram(
            sample_rate=sample_rate,
            n_fft=n_fft,
            hop_length=hop_length,
            n_mels=n_mels,
            center=True,
            power=2.0,
        )

        print(f"[NoteDetectorDataset] split={split} entries={len(self.entries)}")

    def _load_manifest(self, split: str) -> list[ChunkEntry]:
        entries: list[ChunkEntry] = []
        with self.manifest_path.open("r", encoding="utf-8") as handle:
            for line in handle:
                if not line.strip():
                    continue
                item = json.loads(line)
                if item.get("split") != split:
                    continue
                duration_seconds = float(item["duration_seconds"])
                if duration_seconds < self.min_duration_seconds:
                    continue
                note_range = item["notes_range"]
                entries.append(
                    ChunkEntry(
                        chunk_id=str(item["id"]),
                        source_id=str(item["source_id"]),
                        audio_path=Path(item["audio"]),
                        label_path=Path(item["label"]),
                        split=str(item["split"]),
                        start=float(item["start"]),
                        end=float(item["end"]),
                        duration_seconds=duration_seconds,
                        note_start_index=int(note_range[0]),
                        note_end_index=int(note_range[1]),
                    )
                )
        return entries

    def __len__(self) -> int:
        return len(self.entries)

    def _load_label(self, path: Path) -> dict[str, Any]:
        cached = self.label_cache.get(path)
        if cached is not None:
            return cached
        data = json.loads(path.read_text(encoding="utf-8"))
        self.label_cache[path] = data
        return data

    def _load_audio(self, entry: ChunkEntry) -> tuple[torch.Tensor, int]:
        info = sf.info(str(entry.audio_path))
        start_frame = max(0, int(entry.start * info.samplerate))
        frame_count = max(1, int((entry.end - entry.start) * info.samplerate))
        audio, sample_rate = sf.read(
            str(entry.audio_path),
            start=start_frame,
            frames=frame_count,
            dtype="float32",
            always_2d=False,
        )
        audio_tensor = torch.as_tensor(audio, dtype=torch.float32)
        if audio_tensor.ndim > 1:
            audio_tensor = audio_tensor.mean(dim=1)
        return audio_tensor, int(sample_rate)

    def _resample(self, audio: torch.Tensor, sample_rate: int) -> torch.Tensor:
        if sample_rate == self.sample_rate:
            return audio
        return torchaudio.functional.resample(audio, sample_rate, self.sample_rate)

    def _build_targets(self, entry: ChunkEntry, frame_count: int) -> dict[str, torch.Tensor]:
        label = self._load_label(entry.label_path)
        notes = label["notes"][entry.note_start_index : entry.note_end_index + 1]

        voiced = torch.zeros(frame_count, dtype=torch.float32)
        onset = torch.zeros(frame_count, dtype=torch.float32)
        pitch_class = torch.full((frame_count,), -100, dtype=torch.long)
        pitch_mask = torch.zeros(frame_count, dtype=torch.float32)

        chunk_start = entry.start
        frame_seconds = self.hop_length / self.sample_rate

        for note in notes:
            note_start = float(note["start"])
            note_end = float(note["end"])
            rel_start = note_start - chunk_start
            rel_end = note_end - chunk_start
            start_frame = max(0, int(rel_start / frame_seconds))
            end_frame = min(frame_count, max(start_frame + 1, int(rel_end / frame_seconds) + 1))
            if end_frame <= 0 or start_frame >= frame_count:
                continue
            note_frames = end_frame - start_frame
            pad = min(self.alignment_pad_frames, max(0, note_frames // 2))
            voiced_start = max(0, start_frame - pad)
            voiced_end = min(frame_count, end_frame + pad)
            voiced[voiced_start:voiced_end] = 1.0

            trim = min(self.alignment_pad_frames, max(0, (note_frames - 1) // 2))
            pitch_start = start_frame + trim
            pitch_end = max(pitch_start + 1, end_frame - trim)
            pitch_end = min(frame_count, pitch_end)

            midi_value = int(round(float(note["midi"])))
            midi_value = max(self.midi_min, min(self.midi_max, midi_value))
            pitch_class[pitch_start:pitch_end] = midi_value - self.midi_min
            pitch_mask[pitch_start:pitch_end] = 1.0

            boundary_frames = [int(rel_start / frame_seconds), max(start_frame, end_frame - 1)]
            for boundary_frame in boundary_frames:
                for frame_index in range(
                    boundary_frame - self.onset_radius_frames,
                    boundary_frame + self.onset_radius_frames + 1,
                ):
                    if 0 <= frame_index < frame_count:
                        distance = abs(frame_index - boundary_frame)
                        onset[frame_index] = max(
                            onset[frame_index],
                            1.0 - 0.25 * distance,
                        )

        return {
            "voiced": voiced,
            "onset": onset,
            "pitch_class": pitch_class,
            "pitch_mask": pitch_mask,
        }

    def __getitem__(self, index: int) -> dict[str, torch.Tensor | str]:
        entry = self.entries[index]
        audio, source_rate = self._load_audio(entry)
        audio = self._resample(audio, source_rate)

        mel = self.mel_transform(audio)
        log_mel = torch.log(mel.clamp_min(1e-5))
        frame_count = log_mel.shape[-1]
        targets = self._build_targets(entry, frame_count)

        return {
            "chunk_id": entry.chunk_id,
            "audio": audio,
            "log_mel": log_mel,
            "voiced": targets["voiced"],
            "onset": targets["onset"],
            "pitch_class": targets["pitch_class"],
            "pitch_mask": targets["pitch_mask"],
        }


def collate_note_detector_batch(batch: list[dict[str, torch.Tensor | str]]) -> dict[str, torch.Tensor | list[str]]:
    max_audio = max(item["audio"].shape[-1] for item in batch)  # type: ignore[index]
    max_frames = max(item["log_mel"].shape[-1] for item in batch)  # type: ignore[index]
    n_mels = batch[0]["log_mel"].shape[0]  # type: ignore[index]

    audio_batch = []
    mel_batch = []
    voiced_batch = []
    onset_batch = []
    pitch_class_batch = []
    pitch_mask_batch = []
    frame_mask_batch = []
    chunk_ids: list[str] = []

    for item in batch:
        chunk_ids.append(str(item["chunk_id"]))
        audio = item["audio"]  # type: ignore[assignment]
        log_mel = item["log_mel"]  # type: ignore[assignment]
        voiced = item["voiced"]  # type: ignore[assignment]
        onset = item["onset"]  # type: ignore[assignment]
        pitch_class = item["pitch_class"]  # type: ignore[assignment]
        pitch_mask = item["pitch_mask"]  # type: ignore[assignment]

        audio_batch.append(F.pad(audio, (0, max_audio - audio.shape[-1])))
        mel_batch.append(F.pad(log_mel, (0, max_frames - log_mel.shape[-1])))
        voiced_batch.append(F.pad(voiced, (0, max_frames - voiced.shape[-1])))
        onset_batch.append(F.pad(onset, (0, max_frames - onset.shape[-1])))
        pitch_class_batch.append(F.pad(pitch_class, (0, max_frames - pitch_class.shape[-1]), value=-100))
        pitch_mask_batch.append(F.pad(pitch_mask, (0, max_frames - pitch_mask.shape[-1])))

        valid_frames = torch.ones(log_mel.shape[-1], dtype=torch.float32)
        frame_mask_batch.append(F.pad(valid_frames, (0, max_frames - log_mel.shape[-1])))

    return {
        "chunk_id": chunk_ids,
        "audio": torch.stack(audio_batch),
        "log_mel": torch.stack(mel_batch).reshape(len(batch), n_mels, max_frames),
        "voiced": torch.stack(voiced_batch),
        "onset": torch.stack(onset_batch),
        "pitch_class": torch.stack(pitch_class_batch),
        "pitch_mask": torch.stack(pitch_mask_batch),
        "frame_mask": torch.stack(frame_mask_batch),
    }
