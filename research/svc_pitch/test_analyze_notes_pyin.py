import unittest
from pathlib import Path
from unittest import mock

import numpy as np

import analyze_notes_pyin


class IsolatedPitchOutlierRepairTest(unittest.TestCase):
    def test_repairs_short_octave_island_between_stable_notes(self) -> None:
        midi = np.asarray([68.0] * 10 + [56.0] * 4 + [68.0] * 10, dtype=np.float32)
        confidence = np.full(len(midi), 0.8, dtype=np.float32)

        repaired, repaired_confidence = analyze_notes_pyin._repair_isolated_pitch_outliers(
            midi,
            confidence,
            0.02,
        )

        np.testing.assert_allclose(repaired, np.full(len(midi), 68.0, dtype=np.float32))
        np.testing.assert_allclose(repaired_confidence[10:14], np.full(4, 0.68, dtype=np.float32))

    def test_keeps_sustained_octave_change(self) -> None:
        midi = np.asarray([68.0] * 10 + [56.0] * 14 + [68.0] * 10, dtype=np.float32)
        confidence = np.full(len(midi), 0.8, dtype=np.float32)

        repaired, _ = analyze_notes_pyin._repair_isolated_pitch_outliers(midi, confidence, 0.02)

        np.testing.assert_allclose(repaired, midi)

    def test_uses_both_neighbours_for_a_short_subharmonic(self) -> None:
        midi = np.asarray([60.0] * 12 + [54.0] * 8 + [66.0] * 12, dtype=np.float32)
        confidence = np.full(len(midi), 0.8, dtype=np.float32)

        repaired, _ = analyze_notes_pyin._repair_isolated_pitch_outliers(midi, confidence, 0.02)

        np.testing.assert_allclose(repaired[12:20], np.full(8, 66.0, dtype=np.float32))

    def test_removes_tiny_non_octave_subharmonic_between_matching_neighbours(self) -> None:
        midi = np.asarray([67.0] * 10 + [49.0] * 4 + [66.0] * 10, dtype=np.float32)
        confidence = np.full(len(midi), 0.8, dtype=np.float32)

        repaired, repaired_confidence = analyze_notes_pyin._repair_isolated_pitch_outliers(
            midi,
            confidence,
            0.02,
        )

        self.assertTrue(np.all(np.isnan(repaired[10:14])))
        np.testing.assert_allclose(repaired_confidence[10:14], np.zeros(4, dtype=np.float32))

    def test_keeps_short_non_octave_melisma(self) -> None:
        midi = np.asarray([64.0] * 10 + [69.0] * 4 + [66.0] * 10, dtype=np.float32)
        confidence = np.full(len(midi), 0.8, dtype=np.float32)

        repaired, _ = analyze_notes_pyin._repair_isolated_pitch_outliers(midi, confidence, 0.02)

        np.testing.assert_allclose(repaired, midi)

    def test_does_not_modify_an_isolated_pitch_without_context(self) -> None:
        midi = np.asarray([np.nan] * 20 + [50.0] * 4 + [np.nan] * 20, dtype=np.float32)
        confidence = np.full(len(midi), 0.8, dtype=np.float32)

        repaired, _ = analyze_notes_pyin._repair_isolated_pitch_outliers(midi, confidence, 0.02)

        np.testing.assert_allclose(repaired, midi, equal_nan=True)

    def test_suspicious_regions_cover_octave_transitions_only(self) -> None:
        midi = np.asarray(
            [64.0] * 15 + [66.0] * 10 + [54.0] * 5 + [66.0] * 15,
            dtype=np.float32,
        )

        regions = analyze_notes_pyin._find_suspicious_pitch_regions(midi, 0.02)

        self.assertEqual(len(regions), 1)
        self.assertLessEqual(regions[0][0], 0.40)
        self.assertGreaterEqual(regions[0][1], 0.90)


class RmvpeBackendTest(unittest.TestCase):
    def test_rmvpe_backend_does_not_run_pyin(self) -> None:
        audio = np.zeros(2205, dtype=np.float32)
        frame_count = 1 + len(audio) // 256
        rmvpe_midi = np.full(frame_count, 64.25, dtype=np.float32)
        rmvpe_confidence = np.full(frame_count, 0.9, dtype=np.float32)

        with (
            mock.patch.object(analyze_notes_pyin.librosa, "load", return_value=(audio, 22050)),
            mock.patch.object(analyze_notes_pyin.librosa, "pyin", side_effect=AssertionError("pYIN must not run")),
            mock.patch.object(
                analyze_notes_pyin,
                "_compute_rmvpe_track",
                return_value=(rmvpe_midi, rmvpe_confidence),
            ) as rmvpe,
        ):
            tracks = analyze_notes_pyin._compute_pitch_tracks(
                Path("voice.wav"),
                0.72,
                duration=0.1,
                pitch_backend="rmvpe",
            )

        rmvpe.assert_called_once()
        np.testing.assert_allclose(tracks["midi_float"], rmvpe_midi)
        np.testing.assert_array_equal(tracks["midi"], np.full(frame_count, 64, dtype=np.int32))

    def test_silent_analysis_window_is_skipped_without_hiding_full_file_failures(self) -> None:
        audio = np.zeros(2205, dtype=np.float32)
        frame_count = 1 + len(audio) // 256
        unvoiced_midi = np.full(frame_count, np.nan, dtype=np.float32)
        unvoiced_confidence = np.zeros(frame_count, dtype=np.float32)

        with (
            mock.patch.object(analyze_notes_pyin.librosa, "load", return_value=(audio, 22050)),
            mock.patch.object(
                analyze_notes_pyin,
                "_compute_rmvpe_track",
                return_value=(unvoiced_midi, unvoiced_confidence),
            ),
        ):
            tracks = analyze_notes_pyin._compute_pitch_tracks(
                Path("silence.wav"),
                0.72,
                duration=0.1,
                pitch_backend="rmvpe",
                allow_unvoiced=True,
            )
            self.assertTrue(tracks["empty"])

            with self.assertRaisesRegex(RuntimeError, "did not produce a voiced contour"):
                analyze_notes_pyin._compute_pitch_tracks(
                    Path("silence.wav"),
                    0.72,
                    pitch_backend="rmvpe",
                )


if __name__ == "__main__":
    unittest.main()
