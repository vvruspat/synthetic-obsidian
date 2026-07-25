from __future__ import annotations

import unittest

import numpy as np

from analyze_musical_context import infer_time_signature


def accent_pattern(meter: int, phase: int = 0) -> tuple[np.ndarray, np.ndarray, int, int]:
    sample_rate = 100
    hop_length = 10
    beat_times = np.arange(24, dtype=np.float64)
    onset_envelope = np.full(250, 0.05, dtype=np.float32)
    beat_frames = (beat_times * sample_rate / hop_length).astype(int)
    for index, frame in enumerate(beat_frames):
        onset_envelope[frame] = 1.0 if index % meter == phase else 0.2
    return beat_times, onset_envelope, sample_rate, hop_length


class TimeSignatureInferenceTests(unittest.TestCase):
    def test_detects_four_four_accent_pattern(self) -> None:
        beat_times, onset, sample_rate, hop_length = accent_pattern(4)

        signatures = infer_time_signature(beat_times, onset, sample_rate, hop_length, 24.0)

        self.assertEqual(signatures[0]["numerator"], 4)
        self.assertEqual(signatures[0]["denominator"], 4)

    def test_detects_three_four_with_shifted_downbeat_phase(self) -> None:
        beat_times, onset, sample_rate, hop_length = accent_pattern(3, phase=1)

        signatures = infer_time_signature(beat_times, onset, sample_rate, hop_length, 24.0)

        self.assertEqual(signatures[0]["numerator"], 3)
        self.assertEqual(signatures[0]["denominator"], 4)
        self.assertGreater(signatures[0]["confidence"], 0.2)

    def test_uses_low_confidence_four_four_when_too_few_beats_exist(self) -> None:
        beat_times = np.arange(6, dtype=np.float64)
        onset = np.ones(80, dtype=np.float32)

        signatures = infer_time_signature(beat_times, onset, 100, 10, 6.0)

        self.assertEqual(
            signatures,
            [
                {
                    "start": 0.0,
                    "end": 6.0,
                    "numerator": 4,
                    "denominator": 4,
                    "confidence": 0.2,
                }
            ],
        )


if __name__ == "__main__":
    unittest.main()
