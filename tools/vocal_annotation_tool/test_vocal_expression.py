from __future__ import annotations

import json
import math
import tempfile
import unittest
from pathlib import Path

import numpy as np

from vocal_expression import (
    analyze_expression_profile,
    build_target_f0,
    default_expression_profile,
    expression_profile_path_for_adapter,
    hz_to_midi,
    load_expression_profile_for_adapter,
)


F0_RATE = 50.0


def midi_to_hz(midi: np.ndarray | float) -> np.ndarray:
    return 440.0 * np.power(2.0, (np.asarray(midi) - 69.0) / 12.0)


class ExpressionAnalysisTests(unittest.TestCase):
    def test_recovers_vibrato_rate_and_depth(self) -> None:
        times = np.arange(0.0, 5.0, 1.0 / F0_RATE)
        midi = 57.0 + 0.32 * np.sin(2.0 * np.pi * 5.8 * times)

        profile = analyze_expression_profile(
            [midi_to_hz(midi)],
            f0_rate=F0_RATE,
        )

        self.assertAlmostEqual(profile["vibrato"]["rate_hz"], 5.8, delta=0.12)
        self.assertAlmostEqual(
            profile["vibrato"]["depth_cents"],
            32.0,
            delta=4.0,
        )
        self.assertGreater(profile["vibrato"]["confidence"], 0.7)

    def test_straight_singer_profile_reduces_vibrato_usage(self) -> None:
        straight_f0 = np.full(int(5.0 * F0_RATE), 220.0, dtype=np.float32)

        profile = analyze_expression_profile(
            [straight_f0],
            f0_rate=F0_RATE,
        )

        self.assertEqual(profile["vibrato"]["sample_count"], 0)
        self.assertEqual(profile["vibrato"]["usage_probability"], 0.0)
        self.assertGreater(profile["vibrato"]["confidence"], 0.4)

    def test_profile_is_loaded_next_to_adapter(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            adapter = Path(temporary) / "soulx-lora.safetensors"
            adapter.touch()
            profile_path = expression_profile_path_for_adapter(adapter)
            payload = default_expression_profile()
            payload["vibrato"]["rate_hz"] = 6.2
            profile_path.write_text(json.dumps(payload), encoding="utf-8")

            loaded, loaded_path = load_expression_profile_for_adapter(adapter)

        self.assertIsNotNone(loaded)
        self.assertEqual(loaded_path, profile_path.resolve())
        self.assertAlmostEqual(loaded["vibrato"]["rate_hz"], 6.2)


class ExpressionGenerationTests(unittest.TestCase):
    def test_legato_uses_backing_note_pitches(self) -> None:
        profile = default_expression_profile()
        profile["vibrato"]["depth_cents"] = 0.0
        profile["vibrato"]["confidence"] = 1.0
        lead_midi = 52.0 + 0.4 * np.sin(2.0 * np.pi * 6.5 * np.arange(100) / F0_RATE)
        source_f0 = midi_to_hz(lead_midi).astype(np.float32)
        notes = [
            {
                "id": "back-a",
                "start": 0.0,
                "end": 1.0,
                "pitch": 67,
                "syllable_id": "same",
                "legato_to_next": True,
            },
            {
                "id": "back-b",
                "start": 1.0,
                "end": 2.0,
                "pitch": 64,
                "syllable_id": "same",
                "legato_from_previous": True,
            },
        ]

        target_f0, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=7,
        )
        target_midi = hz_to_midi(target_f0)

        np.testing.assert_allclose(target_midi[5:40], 67.0, atol=0.01)
        np.testing.assert_allclose(target_midi[60:95], 64.0, atol=0.01)
        transition = target_midi[47:54]
        self.assertGreater(transition[0], transition[-1])
        self.assertTrue(np.all(np.diff(transition) <= 1e-4))

    def test_vibrato_is_generated_independently_and_deterministically(self) -> None:
        profile = default_expression_profile()
        profile["vibrato"].update(
            {
                "rate_hz": 6.0,
                "rate_std_hz": 0.0,
                "depth_cents": 35.0,
                "depth_std_cents": 0.0,
                "onset_delay_seconds": 0.0,
                "attack_seconds": 0.02,
                "release_seconds": 0.02,
                "minimum_note_seconds": 0.2,
                "usage_probability": 1.0,
                "confidence": 1.0,
            }
        )
        source_f0 = np.full(100, 220.0, dtype=np.float32)
        notes = [{"id": "back-vibrato", "start": 0.0, "end": 2.0, "pitch": 60}]

        first, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=99,
        )
        second, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=99,
        )
        target_midi = hz_to_midi(first)

        np.testing.assert_array_equal(first, second)
        self.assertGreater(np.ptp(target_midi[10:-10]) * 100.0, 60.0)
        self.assertLess(np.ptp(target_midi[10:-10]) * 100.0, 80.0)

    def test_long_sustain_does_not_randomly_lose_vibrato(self) -> None:
        profile = default_expression_profile()
        profile["vibrato"].update(
            {
                "depth_cents": 30.0,
                "depth_std_cents": 0.0,
                "onset_delay_seconds": 0.0,
                "attack_seconds": 0.02,
                "release_seconds": 0.02,
                "minimum_note_seconds": 0.48,
                "usage_probability": 0.1,
                "confidence": 1.0,
            }
        )
        source_f0 = np.full(100, 220.0, dtype=np.float32)
        notes = [{"id": "long-sustain", "start": 0.0, "end": 2.0, "pitch": 60}]

        target_f0, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=1,
        )

        self.assertGreater(np.ptp(hz_to_midi(target_f0)[10:-10]) * 100.0, 50.0)

    def test_micro_curve_jitter_does_not_suppress_vibrato(self) -> None:
        profile = default_expression_profile()
        profile["vibrato"].update(
            {
                "depth_cents": 30.0,
                "depth_std_cents": 0.0,
                "onset_delay_seconds": 0.0,
                "attack_seconds": 0.02,
                "release_seconds": 0.02,
                "minimum_note_seconds": 0.2,
                "usage_probability": 1.0,
                "confidence": 1.0,
            }
        )
        source_f0 = np.full(100, 220.0, dtype=np.float32)
        notes = [
            {
                "id": "micro-jitter",
                "start": 0.0,
                "end": 2.0,
                "pitch": 60,
                "curve": [
                    {"time": time, "midi": 60.0 + 0.03 * math.sin(time * 23.0)}
                    for time in np.linspace(0.0, 2.0, 20)
                ],
            }
        ]

        target_f0, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=4,
        )

        self.assertGreater(np.ptp(hz_to_midi(target_f0)[10:-10]) * 100.0, 50.0)

    def test_connected_notes_glide_without_explicit_legato_flags(self) -> None:
        profile = default_expression_profile()
        profile["vibrato"]["depth_cents"] = 0.0
        profile["vibrato"]["confidence"] = 1.0
        source_f0 = np.full(100, 220.0, dtype=np.float32)
        notes = [
            {"id": "connected-a", "start": 0.0, "end": 1.0, "pitch": 60},
            {"id": "connected-b", "start": 1.0, "end": 2.0, "pitch": 64},
        ]

        target_f0, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=3,
        )
        transition = hz_to_midi(target_f0)[48:53]

        self.assertTrue(np.all(np.diff(transition) >= -1e-4))
        self.assertGreater(transition[-1] - transition[0], 2.0)

    def test_rearticulated_note_settles_from_a_slightly_sharp_attack(self) -> None:
        profile = default_expression_profile()
        profile["vibrato"]["depth_cents"] = 0.0
        profile["vibrato"]["confidence"] = 1.0
        source_f0 = np.full(50, 220.0, dtype=np.float32)
        notes = [{"id": "attack", "start": 0.0, "end": 1.0, "pitch": 60}]

        target_f0, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=8,
        )
        target_midi = hz_to_midi(target_f0)

        self.assertGreater(target_midi[0], 60.14)
        self.assertAlmostEqual(target_midi[6], 60.0, delta=0.01)

    def test_connected_same_pitch_notes_keep_expression_continuous(self) -> None:
        profile = default_expression_profile()
        profile["vibrato"].update(
            {
                "rate_hz": 5.6,
                "rate_std_hz": 0.0,
                "depth_cents": 38.0,
                "depth_std_cents": 0.0,
                "onset_delay_seconds": 0.0,
                "attack_seconds": 0.02,
                "release_seconds": 0.02,
                "minimum_note_seconds": 0.2,
                "usage_probability": 1.0,
                "confidence": 1.0,
            }
        )
        source_f0 = np.full(100, 220.0, dtype=np.float32)
        notes = [
            {"id": "same-a", "start": 0.0, "end": 1.0, "pitch": 60},
            {"id": "same-b", "start": 1.0, "end": 2.0, "pitch": 60},
        ]

        target_f0, _ = build_target_f0(
            notes,
            source_f0,
            profile,
            f0_rate=F0_RATE,
            seed=12,
        )
        target_midi = hz_to_midi(target_f0)

        self.assertLess(abs(target_midi[50] - target_midi[49]), 0.3)
        self.assertLess(abs(target_midi[50] - 60.0), 0.5)


if __name__ == "__main__":
    unittest.main()
