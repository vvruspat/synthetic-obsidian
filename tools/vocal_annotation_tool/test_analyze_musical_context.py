from __future__ import annotations

import unittest

import numpy as np

from analyze_musical_context import (
    estimate_chords_from_features,
    estimate_key,
    infer_time_signature,
    phase_aligned_beat_times,
    score_chord,
)


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


class ChordInferenceTests(unittest.TestCase):
    def test_stable_tempo_grid_preserves_detected_phase(self) -> None:
        detected = np.asarray([0.23, 0.73, 1.23, 1.73, 2.23])

        beats = phase_aligned_beat_times(3.0, 120.0, detected)

        self.assertTrue(np.allclose(beats[:5], detected, atol=1e-6))

    def test_scores_seventh_chord_template(self) -> None:
        chroma = np.zeros(12)
        chroma[[7, 11, 2, 5]] = 1.0
        bass = np.zeros(12)
        bass[7] = 1.0

        name, confidence = score_chord(chroma, bass)

        self.assertEqual(name, "G7")
        self.assertGreater(confidence, 0.3)

    def test_estimates_minor_key(self) -> None:
        chroma = np.full((12, 16), 0.01)
        chroma[[10, 0, 1, 3, 5, 6, 8], :] = np.asarray(
            [1.0, 0.35, 0.58, 0.7, 0.62, 0.5, 0.68]
        )[:, None]

        name, confidence = estimate_key(chroma)

        self.assertEqual(name, "Bb minor")
        self.assertGreater(confidence, 0.2)

    def test_detects_and_refines_chord_change(self) -> None:
        frame_times = np.arange(0.0, 4.0, 0.025)
        chroma = np.full((12, frame_times.size), 0.005)
        bass = np.full((12, frame_times.size), 0.002)
        first = frame_times < 2.08
        second = ~first
        chroma[np.ix_([0, 4, 7], first)] = 1.0
        bass[0, first] = 1.0
        chroma[np.ix_([7, 11, 2, 5], second)] = 1.0
        bass[7, second] = 1.0

        segments = estimate_chords_from_features(
            chroma,
            bass,
            frame_times,
            np.arange(0.0, 4.01, 0.5),
            4.0,
            0.025,
        )

        self.assertEqual([segment["name"] for segment in segments], ["C", "G7"])
        self.assertAlmostEqual(float(segments[0]["end"]), 2.08, delta=0.08)
        self.assertEqual(segments[0]["end"], segments[1]["start"])


if __name__ == "__main__":
    unittest.main()
