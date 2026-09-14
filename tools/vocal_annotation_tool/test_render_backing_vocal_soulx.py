from __future__ import annotations

import unittest

import numpy as np

from render_backing_vocal_soulx import (
    SOULX_HOP_SIZE,
    SOULX_SAMPLE_RATE,
    select_prompt_window,
)
from render_backing_vocal_soulx_singer import (
    build_score_metadata,
    split_target_phrases,
)


class FakePhonemizer:
    def phonemize(self, lyric: object) -> tuple[str, bool]:
        return ("en_T-EH1-S-T", bool(str(lyric or "")))


class PromptSelectionTests(unittest.TestCase):
    def test_prefers_target_register_over_slightly_denser_window(self) -> None:
        frame_count = 1_000
        f0 = np.zeros(frame_count, dtype=np.float32)
        f0[:400] = 550.0
        f0[500:900] = 215.0
        f0[500:900:12] = 0.0
        audio = np.zeros(frame_count * SOULX_HOP_SIZE, dtype=np.float32)

        _, selected_f0, start = select_prompt_window(
            audio,
            f0,
            target_register_hz=205.0,
        )

        voiced = selected_f0[selected_f0 > 32.0]
        self.assertGreaterEqual(start, 8.0)
        self.assertLess(float(np.median(voiced)), 240.0)

    def test_without_target_register_prefers_densest_window(self) -> None:
        frame_count = 1_000
        f0 = np.zeros(frame_count, dtype=np.float32)
        f0[:400] = 550.0
        f0[500:900] = 215.0
        f0[500:900:12] = 0.0
        audio = np.zeros(frame_count * SOULX_HOP_SIZE, dtype=np.float32)

        selected_audio, _, start = select_prompt_window(audio, f0)

        self.assertEqual(start, 0.0)
        self.assertEqual(len(selected_audio), 8 * SOULX_SAMPLE_RATE)


class SingerScoreTests(unittest.TestCase):
    def test_score_preserves_gaps_and_marks_melisma_continuation(self) -> None:
        notes = [
            {"start": 1.0, "end": 1.5, "pitch": 60, "lyric": "moon", "syllable_id": "s1"},
            {
                "start": 1.5,
                "end": 2.0,
                "pitch": 62,
                "syllable_id": "s1",
                "melisma_continuation": True,
            },
            {"start": 2.4, "end": 2.8, "pitch": 64, "lyric": "light", "syllable_id": "s2"},
        ]

        metadata, real_lyrics, sung_notes = build_score_metadata(
            notes,
            0.9,
            2.9,
            FakePhonemizer(),  # type: ignore[arg-type]
        )

        self.assertEqual(metadata["note_type"], "1 2 3 1 2 1")
        self.assertEqual(metadata["note_pitch"], "0 60 62 0 64 0")
        self.assertEqual(real_lyrics, 2)
        self.assertEqual(sung_notes, 3)

    def test_long_gap_starts_a_new_target_phrase(self) -> None:
        notes = [
            {"start": 0.0, "end": 0.4, "pitch": 60},
            {"start": 0.5, "end": 0.9, "pitch": 62},
            {"start": 2.0, "end": 2.4, "pitch": 64},
        ]

        self.assertEqual(len(split_target_phrases(notes)), 2)


if __name__ == "__main__":
    unittest.main()
