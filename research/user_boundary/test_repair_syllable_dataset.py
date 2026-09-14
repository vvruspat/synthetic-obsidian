from __future__ import annotations

import unittest

try:
    from .repair_syllable_dataset import (
        choose_boundaries,
        count_mnemonic_syllables,
        group_segments,
        is_structural_word,
        merge_intervals,
        reject_reason,
        RepairConfig,
    )
except ImportError:
    from repair_syllable_dataset import (
        RepairConfig,
        choose_boundaries,
        count_mnemonic_syllables,
        group_segments,
        is_structural_word,
        merge_intervals,
        reject_reason,
    )


class RepairSyllableDatasetTest(unittest.TestCase):
    def test_counts_phonetic_not_orthographic_syllables(self) -> None:
        examples = {
            "st'eI": 1,
            "bIk,Vm": 2,
            "'EvrI2": 2,
            "m'eIbi:": 2,
            "f'aI@": 2,
            "m;il'od;ija": 4,
        }
        for mnemonic, expected in examples.items():
            self.assertEqual(
                count_mnemonic_syllables(mnemonic),
                expected,
                mnemonic,
            )

    def test_recognizes_section_labels(self) -> None:
        for word in ("intro", "pre-chorus", "chorus2", "outro"):
            self.assertTrue(is_structural_word(word), word)
        self.assertFalse(is_structural_word("outside"))

    def test_groups_only_consecutive_occurrences(self) -> None:
        segments = [
            {"word_index": 1, "word": "every"},
            {"word_index": 1, "word": "every"},
            {"word_index": -1, "word": ""},
            {"word_index": 1, "word": "every"},
        ]
        groups = group_segments(segments)
        self.assertEqual([len(group) for group in groups], [2, 1, 1])

    def test_acoustic_score_selects_best_excess_boundary(self) -> None:
        segments = [
            {"start": 0.0, "end": 0.2, "confidence": 0.9},
            {"start": 0.2, "end": 0.4, "confidence": 0.9},
            {"start": 0.4, "end": 0.7, "confidence": 0.9},
        ]
        scores = {0.2: 0.1, 0.4: 1.0}
        selected = choose_boundaries(
            segments,
            expected_count=2,
            minimum_distance=0.06,
            acoustic_score=scores.__getitem__,
        )
        self.assertEqual(selected, (0.0, 0.4))

    def test_rejects_boundaries_that_are_too_close(self) -> None:
        segments = [
            {"start": 0.0, "end": 0.02, "confidence": 0.9},
            {"start": 0.02, "end": 0.04, "confidence": 0.9},
        ]
        self.assertIsNone(
            choose_boundaries(
                segments,
                expected_count=2,
                minimum_distance=0.06,
                acoustic_score=lambda _: 1.0,
            )
        )

    def test_merges_only_nearly_touching_trusted_intervals(self) -> None:
        merged = merge_intervals(
            [[0.0, 0.2], [0.21, 0.4], [0.6, 0.8]],
            maximum_gap=0.015,
        )
        self.assertEqual(merged, [[0.0, 0.4], [0.6, 0.8]])

    def test_rejects_word_groups_with_missing_spelling(self) -> None:
        config = RepairConfig(
            minimum_confidence=0.8,
            minimum_boundary_distance=0.06,
            interval_merge_gap=0.015,
            allow_acoustic_multisyllable_collapse=False,
        )
        group = [
            {
                "syllable": "ma",
                "start": 0.0,
                "end": 0.1,
                "confidence": 0.95,
                "match_type": "word-anchor+ctc",
                "unassigned": False,
            },
            {
                "syllable": "y",
                "start": 0.1,
                "end": 0.2,
                "confidence": 0.95,
                "match_type": "word-anchor+ctc",
                "unassigned": False,
            },
        ]
        self.assertEqual(
            reject_reason(
                group,
                word="maybe",
                expected_count=2,
                duration=1.0,
                config=config,
            ),
            "incomplete_word_spelling",
        )


if __name__ == "__main__":
    unittest.main()
