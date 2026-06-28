from __future__ import annotations

import unittest

import numpy as np

from infer_gtsinger_boundaries import (
    backtrack_breath_mask_starts,
    clean_interval_masks,
    clean_interval_masks_with_breath_hysteresis,
    mask_to_intervals,
    refine_onset_frames,
)


class IntervalDecodingTests(unittest.TestCase):
    def test_resolves_overlap_and_removes_short_fragments(self) -> None:
        probabilities = np.zeros((2, 20), dtype=np.float32)
        probabilities[0, 2:10] = 0.8
        probabilities[1, 7:15] = 0.9
        probabilities[0, 18:20] = 0.9

        masks = clean_interval_masks(
            probabilities,
            thresholds=(0.6, 0.6),
            min_frames=4,
            merge_gap_frames=2,
        )

        self.assertFalse(np.any(masks[0] & masks[1]))
        self.assertFalse(np.any(masks[0, 18:20]))

    def test_interval_confidence_uses_active_frames(self) -> None:
        mask = np.asarray([False, True, True, False])
        probabilities = np.asarray([0.1, 0.7, 0.9, 0.2])

        intervals = mask_to_intervals(mask, probabilities, 0.01)

        self.assertEqual(
            intervals,
            [{"start": 0.01, "end": 0.03, "confidence": 0.8}],
        )

    def test_breath_hysteresis_expands_from_confident_peak(self) -> None:
        probabilities = np.zeros((2, 20), dtype=np.float32)
        probabilities[0, 5:16] = 0.26
        probabilities[0, 8] = 0.52
        probabilities[0, 17:19] = 0.26

        masks = clean_interval_masks_with_breath_hysteresis(
            probabilities,
            thresholds=(0.5, 0.6),
            min_frames=4,
            merge_gap_frames=1,
        )

        self.assertTrue(np.all(masks[0, 5:16]))
        self.assertFalse(np.any(masks[0, 17:19]))

    def test_breath_start_backtracks_to_rms_onset(self) -> None:
        mask = np.zeros(30, dtype=bool)
        mask[15:22] = True
        energy = np.zeros(30, dtype=np.float32)
        energy[11:15] = [0.08, 0.16, 0.24, 0.32]
        energy[15:22] = 0.6

        refined = backtrack_breath_mask_starts(mask, energy, max_backtrack_frames=8)

        self.assertTrue(np.all(refined[12:22]))
        self.assertFalse(refined[10])


class OnsetRefinementTests(unittest.TestCase):
    def test_moves_tcn_prediction_to_nearby_attack(self) -> None:
        onset = np.zeros(40, dtype=np.float32)
        rms = np.zeros(40, dtype=np.float32)
        energy = np.full(40, 0.1, dtype=np.float32)
        onset[15] = 1.0
        rms[15] = 0.8
        energy[15:] = 0.9

        refined = refine_onset_frames(
            np.asarray([20]),
            onset,
            rms,
            energy,
            min_distance_frames=3,
        )

        np.testing.assert_array_equal(refined, np.asarray([15]))

    def test_keeps_order_and_minimum_spacing(self) -> None:
        onset = np.zeros(40, dtype=np.float32)
        rms = np.zeros(40, dtype=np.float32)
        energy = np.full(40, 0.1, dtype=np.float32)
        onset[10] = 1.0
        onset[11] = 0.9
        energy[10:] = 0.8

        refined = refine_onset_frames(
            np.asarray([12, 14]),
            onset,
            rms,
            energy,
            min_distance_frames=3,
        )

        self.assertGreaterEqual(refined[1] - refined[0], 3)

    def test_backtracks_internal_peak_to_start_of_energy_rise(self) -> None:
        onset = np.zeros(60, dtype=np.float32)
        rms_delta = np.zeros(60, dtype=np.float32)
        energy = np.full(60, 0.08, dtype=np.float32)
        energy[24:30] = np.linspace(0.08, 0.9, 6)
        energy[30:] = 0.9
        onset[30] = 1.0
        rms_delta[25] = 0.8

        refined = refine_onset_frames(
            np.asarray([34]),
            onset,
            rms_delta,
            energy,
            min_distance_frames=3,
        )

        self.assertLessEqual(refined[0], 26)


if __name__ == "__main__":
    unittest.main()
