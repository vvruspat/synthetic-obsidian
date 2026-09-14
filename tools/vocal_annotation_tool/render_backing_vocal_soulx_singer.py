#!/usr/bin/env python3
"""Score-conditioned backing-vocal rendering with SoulX-Singer.

This is an offline worker helper. The generated MIDI is the source of pitch and
timing; the lead/profile recording is used only as the zero-shot timbre prompt.
When real aligned English lyrics are unavailable, the target is rendered as an
``ooh`` backing vocal instead of inventing words.
"""

from __future__ import annotations

import math
import os
import re
import sys
from pathlib import Path
from typing import Any

os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")

import numpy as np
import soundfile as sf
import torch

from render_backing_vocal_soulx import (
    DEFAULT_CONFIG_PATH,
    DEFAULT_INFERENCE_CFG,
    DEFAULT_INFERENCE_STEPS,
    DEFAULT_SOURCE_ROOT,
    SOULX_F0_RATE,
    SOULX_HOP_SIZE,
    SOULX_SAMPLE_RATE,
    CpuVocoderBridge,
    build_sample_mask,
    choose_device,
    extract_f0,
    load_mono_24k,
    select_best_prompt,
)


DEFAULT_SINGER_MODEL_PATH = (
    Path.home() / ".synthetic_obsidian" / "models" / "SoulX-Singer" / "model.pt"
)
MAX_TARGET_SECONDS = 10.0
MAX_TARGET_NOTES = 40
PHRASE_GAP_SECONDS = 0.9
SEGMENT_PADDING_SECONDS = 0.12
_WORD_RE = re.compile(r"[A-Za-z]+(?:'[A-Za-z]+)?")
_PLACEHOLDER_LYRICS = {
    "",
    "bv",
    "syllable",
    "rearticulation",
    "legato",
    "breath",
    "noise",
    "pause",
    "ignore",
}


class EnglishPhonemizer:
    """Small, failure-tolerant bridge to SoulX's English phone inventory."""

    def __init__(self, phoneset_path: Path) -> None:
        import json

        self.phones = set(json.loads(phoneset_path.read_text(encoding="utf-8")))
        try:
            from g2p_en import G2p  # pylint: disable=import-outside-toplevel

            self.g2p = G2p()
        except (ImportError, LookupError, OSError):
            self.g2p = None

    @staticmethod
    def clean_word(value: Any) -> str:
        text = str(value or "").strip().replace("-", "")
        words = _WORD_RE.findall(text)
        return "".join(words) if words else ""

    def phonemize(self, lyric: Any) -> tuple[str, bool]:
        word = self.clean_word(lyric)
        if word.lower() in _PLACEHOLDER_LYRICS or self.g2p is None:
            return "en_UW1", False

        try:
            raw_phones = self.g2p(word.lower())
        except (LookupError, OSError, ValueError):
            return "en_UW1", False
        phones = [str(phone).strip() for phone in raw_phones if str(phone).strip()]
        phones = [phone for phone in phones if f"en_{phone}" in self.phones]
        if not phones:
            return "en_UW1", False
        return "en_" + "-".join(phones), True


def _note_times(note: dict[str, Any]) -> tuple[float, float]:
    start = float(note.get("voiced_start", note.get("start", 0.0)))
    end = float(note.get("voiced_end", note.get("end", start)))
    return start, end


def _lead_lyrics(request: dict[str, Any]) -> dict[str, str]:
    result: dict[str, str] = {}
    for note in request.get("lead_notes", []):
        if not isinstance(note, dict):
            continue
        syllable_id = str(note.get("syllable_id") or "")
        lyric = str(note.get("lyric") or "")
        if syllable_id and lyric and syllable_id not in result:
            result[syllable_id] = lyric
    return result


def resolve_note_lyric(note: dict[str, Any], lead_lyrics: dict[str, str]) -> str:
    lyric = str(note.get("lyric") or "").strip()
    if lyric.lower() not in _PLACEHOLDER_LYRICS:
        return lyric
    return lead_lyrics.get(str(note.get("syllable_id") or ""), "")


def notes_from_f0(f0: np.ndarray, duration: float) -> list[dict[str, Any]]:
    """Make a compact neutral-vowel score for an unannotated prompt recording."""
    if not len(f0):
        return []
    voiced = f0 > 32.0
    midi = np.zeros(len(f0), dtype=np.int16)
    midi[voiced] = np.rint(69.0 + 12.0 * np.log2(f0[voiced] / 440.0)).astype(np.int16)

    notes: list[dict[str, Any]] = []
    start = 0
    current_pitch = int(midi[0]) if voiced[0] else 0
    for index in range(1, len(f0) + 1):
        next_pitch = int(midi[index]) if index < len(f0) and voiced[index] else 0
        changed = index == len(f0) or (next_pitch == 0) != (current_pitch == 0)
        if not changed and current_pitch > 0 and abs(next_pitch - current_pitch) >= 2:
            changed = True
        if not changed:
            continue
        start_time = start / SOULX_F0_RATE
        end_time = min(duration, index / SOULX_F0_RATE)
        if current_pitch > 0 and end_time - start_time >= 0.06:
            region = midi[start:index]
            region = region[region > 0]
            notes.append(
                {
                    "start": start_time,
                    "end": end_time,
                    "pitch": int(np.median(region)) if len(region) else current_pitch,
                    "lyric": "ooh",
                    "syllable_id": f"prompt_{len(notes):04d}",
                }
            )
        start = index
        current_pitch = next_pitch
    return notes


def build_score_metadata(
    notes: list[dict[str, Any]],
    segment_start: float,
    segment_end: float,
    phonemizer: EnglishPhonemizer,
    lead_lyrics: dict[str, str] | None = None,
) -> tuple[dict[str, Any], int, int]:
    """Convert absolute-time app notes to one contiguous SoulX metadata item."""
    lead_lyrics = lead_lyrics or {}
    durations: list[float] = []
    texts: list[str] = []
    phonemes: list[str] = []
    pitches: list[int] = []
    note_types: list[int] = []
    cursor = segment_start
    previous_syllable = ""
    previous_phoneme = "en_UW1"
    real_lyrics = 0
    sung_notes = 0

    def append(duration: float, text: str, phoneme: str, pitch: int, note_type: int) -> None:
        if duration < 0.015:
            return
        durations.append(duration)
        texts.append(text)
        phonemes.append(phoneme)
        pitches.append(pitch)
        note_types.append(note_type)

    for note in sorted(notes, key=_note_times):
        note_start, note_end = _note_times(note)
        note_start = max(segment_start, note_start, cursor)
        note_end = min(segment_end, note_end)
        if note_end - note_start < 0.04:
            continue
        if note_start > cursor + 0.015:
            append(note_start - cursor, "<SP>", "<SP>", 0, 1)

        syllable = str(note.get("syllable_id") or "")
        continuation = bool(
            (syllable and syllable == previous_syllable)
            or note.get("melisma_continuation")
            or note.get("legato_from_previous")
        )
        lyric = resolve_note_lyric(note, lead_lyrics)
        phoneme, has_real_lyric = phonemizer.phonemize(lyric)
        if continuation:
            phoneme = previous_phoneme
        if has_real_lyric:
            real_lyrics += 1
        append(
            note_end - note_start,
            lyric or "ooh",
            phoneme,
            max(1, min(127, int(round(float(note.get("pitch", 60)))))),
            3 if continuation else 2,
        )
        sung_notes += 1
        cursor = note_end
        previous_syllable = syllable
        previous_phoneme = phoneme

    if cursor < segment_end - 0.015:
        append(segment_end - cursor, "<SP>", "<SP>", 0, 1)
    if not durations:
        raise ValueError("score segment has no usable notes")

    return (
        {
            "index": "synthetic_obsidian_score",
            "language": "English",
            "time": [0, int(round((segment_end - segment_start) * 1_000.0))],
            "duration": " ".join(f"{value:.5f}" for value in durations),
            "text": " ".join(texts),
            "phoneme": " ".join(phonemes),
            "note_pitch": " ".join(str(value) for value in pitches),
            "note_type": " ".join(str(value) for value in note_types),
        },
        real_lyrics,
        sung_notes,
    )


def split_target_phrases(notes: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    phrases: list[list[dict[str, Any]]] = []
    current: list[dict[str, Any]] = []
    phrase_start = 0.0
    previous_end = 0.0
    for note in sorted(notes, key=_note_times):
        start, end = _note_times(note)
        if end <= start:
            continue
        should_split = bool(
            current
            and (
                start - previous_end > PHRASE_GAP_SECONDS
                or end - phrase_start > MAX_TARGET_SECONDS
                or len(current) >= MAX_TARGET_NOTES
            )
        )
        if should_split:
            phrases.append(current)
            current = []
        if not current:
            phrase_start = start
        current.append(note)
        previous_end = end
    if current:
        phrases.append(current)
    return phrases


class SoulXSingerRenderer:
    """Persistent full SoulX-Singer renderer using score control."""

    def __init__(
        self,
        source_root: Path = DEFAULT_SOURCE_ROOT,
        model_path: Path = DEFAULT_SINGER_MODEL_PATH,
        config_path: Path = DEFAULT_CONFIG_PATH,
        device: str = "auto",
        steps: int = DEFAULT_INFERENCE_STEPS,
        cfg: float = DEFAULT_INFERENCE_CFG,
    ) -> None:
        self.source_root = source_root.expanduser().resolve()
        self.model_path = model_path.expanduser().resolve()
        self.config_path = config_path.expanduser().resolve()
        self.device = choose_device(device)
        self.steps = steps
        self.cfg = cfg
        if not self.model_path.is_file():
            raise FileNotFoundError(f"SoulX-Singer SVS checkpoint not found: {self.model_path}")

        sys.path.insert(0, str(self.source_root))
        from cli.inference import build_model  # pylint: disable=import-outside-toplevel
        from soulxsinger.utils.data_processor import DataProcessor  # pylint: disable=import-outside-toplevel
        from soulxsinger.utils.file_utils import load_config  # pylint: disable=import-outside-toplevel

        self.config = load_config(str(self.config_path))
        self.model = build_model(
            model_path=str(self.model_path),
            config=self.config,
            device=self.device,
            use_fp16=False,
        )
        if self.device == "mps":
            self.model.vocoder = CpuVocoderBridge(self.model.vocoder)
        phoneset_path = self.source_root / "soulxsinger" / "utils" / "phoneme" / "phone_set.json"
        self.phonemizer = EnglishPhonemizer(phoneset_path)
        self.processor = DataProcessor(
            hop_size=self.config.audio.hop_size,
            sample_rate=self.config.audio.sample_rate,
            phoneset_path=str(phoneset_path),
            device=self.device,
        )

    def _build_prompt(self, request: dict[str, Any], target_f0: np.ndarray) -> dict[str, Any]:
        audio_path = Path(request["audio"]).expanduser().resolve()
        source_audio = load_mono_24k(audio_path)
        source_f0 = extract_f0(source_audio)
        prompt_audio, prompt_f0, _, prompt_source = select_best_prompt(
            request,
            audio_path,
            source_audio,
            source_f0,
            target_register_f0=target_f0,
        )
        prompt_duration = len(prompt_audio) / SOULX_SAMPLE_RATE
        prompt_notes = notes_from_f0(prompt_f0, prompt_duration)
        if not prompt_notes:
            raise ValueError("selected SoulX timbre prompt contains no voiced notes")
        prompt_meta, _, _ = build_score_metadata(
            prompt_notes,
            0.0,
            prompt_duration,
            self.phonemizer,
        )
        prompt_data = self.processor.process(prompt_meta, None)
        prompt_frames = prompt_data["mel2note"].shape[1]
        prompt_data["waveform"] = (
            torch.from_numpy(prompt_audio)
            .unsqueeze(0)
            .to(self.device)[:, : prompt_frames * SOULX_HOP_SIZE]
        )
        return {"data": prompt_data, "source": str(prompt_source), "duration": prompt_duration}

    def render(self, request: dict[str, Any]) -> dict[str, Any]:
        audio_path = Path(request["audio"]).expanduser().resolve()
        output_path = Path(request["output"]).expanduser().resolve()
        notes = request.get("backing_notes", [])
        if not isinstance(notes, list) or not notes:
            raise ValueError("request has no backing_notes")
        source_audio = load_mono_24k(audio_path)
        source_f0 = extract_f0(source_audio)

        target_f0 = np.zeros_like(source_f0)
        for note in notes:
            start, end = _note_times(note)
            left = max(0, int(math.floor(start * SOULX_F0_RATE)))
            right = min(len(target_f0), int(math.ceil(end * SOULX_F0_RATE)))
            if right > left:
                pitch = float(note.get("pitch", note.get("pitch_exact", 60.0)))
                target_f0[left:right] = 440.0 * 2.0 ** ((pitch - 69.0) / 12.0)

        prompt = self._build_prompt(request, target_f0)
        lead_lyrics = _lead_lyrics(request)
        output = np.zeros(len(source_audio), dtype=np.float32)
        total_real_lyrics = 0
        total_sung_notes = 0
        phrases = split_target_phrases(notes)
        inference_steps = int(request.get("steps", self.steps))
        inference_cfg = float(request.get("cfg", self.cfg))

        for phrase in phrases:
            phrase_start = max(0.0, _note_times(phrase[0])[0] - SEGMENT_PADDING_SECONDS)
            phrase_end = min(
                len(source_audio) / SOULX_SAMPLE_RATE,
                _note_times(phrase[-1])[1] + SEGMENT_PADDING_SECONDS,
            )
            target_meta, real_lyrics, sung_notes = build_score_metadata(
                phrase,
                phrase_start,
                phrase_end,
                self.phonemizer,
                lead_lyrics,
            )
            target_data = self.processor.process(target_meta, None)
            with torch.inference_mode():
                generated = self.model.infer(
                    {"prompt": prompt["data"], "target": target_data},
                    auto_shift=False,
                    pitch_shift=0,
                    n_steps=inference_steps,
                    cfg=inference_cfg,
                    control="score",
                    use_fp16=False,
                )
            rendered = generated.squeeze().float().cpu().numpy()
            start_sample = int(round(phrase_start * SOULX_SAMPLE_RATE))
            available = max(0, len(output) - start_sample)
            rendered = rendered[:available]
            output[start_sample:start_sample + len(rendered)] = rendered
            total_real_lyrics += real_lyrics
            total_sung_notes += sung_notes

        mask_frames = np.zeros(len(source_f0), dtype=np.float32)
        for note in notes:
            start, end = _note_times(note)
            left = max(0, int(math.floor(start * SOULX_F0_RATE)))
            right = min(len(mask_frames), int(math.ceil(end * SOULX_F0_RATE)))
            mask_frames[left:right] = 1.0
        output *= build_sample_mask(mask_frames, len(output))
        peak = float(np.max(np.abs(output))) if len(output) else 0.0
        if peak > 0.98:
            output *= 0.98 / peak

        output_path.parent.mkdir(parents=True, exist_ok=True)
        sf.write(str(output_path), output, SOULX_SAMPLE_RATE)
        return {
            "output": str(output_path),
            "sample_rate": SOULX_SAMPLE_RATE,
            "duration": len(output) / SOULX_SAMPLE_RATE,
            "notes": len(notes),
            "rendered_notes": total_sung_notes,
            "phrases": len(phrases),
            "engine": "SoulX-Singer",
            "control": "score",
            "device": self.device,
            "steps": inference_steps,
            "cfg": inference_cfg,
            "prompt_source": prompt["source"],
            "prompt_duration": prompt["duration"],
            "lyrics": "aligned-English" if total_real_lyrics else "neutral-ooh",
            "voice_adapter": "",
        }
