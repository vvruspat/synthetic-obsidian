from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
import wave
from pathlib import Path

try:
    from .build_manifest import build_record, stable_split
    from .labels import derive_labels, is_vowel_phone
except ImportError:
    from build_manifest import build_record, stable_split
    from labels import derive_labels, is_vowel_phone


TORCH_STACK_AVAILABLE = all(
    importlib.util.find_spec(module) is not None
    for module in ("librosa", "numpy", "soundfile", "torch")
)
if TORCH_STACK_AVAILABLE:
    try:
        from .train_baseline import (
            BoundaryNet,
            ChunkDataset,
            Example,
            TCN_DILATIONS,
            calibrate_thresholds,
            clone_state_dict,
            event_metrics,
            extract_event_frames,
            extract_rising_edges,
            make_targets,
            match_events,
            positive_weights,
            primary_validation_score,
            receptive_field_frames,
            segmentation_metrics,
            training_loss,
        )
    except ImportError:
        from train_baseline import (
            BoundaryNet,
            ChunkDataset,
            Example,
            TCN_DILATIONS,
            calibrate_thresholds,
            clone_state_dict,
            event_metrics,
            extract_event_frames,
            extract_rising_edges,
            make_targets,
            match_events,
            positive_weights,
            primary_validation_score,
            receptive_field_frames,
            segmentation_metrics,
            training_loss,
        )
    import numpy as np
    import torch


class LabelsTest(unittest.TestCase):
    def test_cross_inventory_vowels(self) -> None:
        for phone in ("AE1", "ə", "uo", "iː"):
            self.assertTrue(is_vowel_phone(phone), phone)
        for phone in ("HH", "tʰ", "ʃ", "<SP>"):
            self.assertFalse(is_vowel_phone(phone), phone)

    def test_language_context_disambiguates_english_y(self) -> None:
        self.assertFalse(is_vowel_phone("Y", language="English"))
        self.assertTrue(is_vowel_phone("y", language="French"))
        self.assertFalse(is_vowel_phone("<AP>", language="French"))

    def test_english_y_is_not_a_syllable_nucleus(self) -> None:
        annotation = [
            {
                "word": "you",
                "start_time": 0.0,
                "end_time": 0.5,
                "ph": ["Y", "UW1"],
                "ph_start": [0.0, 0.1],
                "ph_end": [0.1, 0.5],
            }
        ]
        labels = derive_labels(annotation, 0.5, language="English")
        self.assertEqual(labels.events["syllable"], [0.1])

    def test_derives_four_event_types(self) -> None:
        annotation = [
            {
                "word": "<SP>",
                "start_time": 0.0,
                "end_time": 0.2,
                "ph": ["<SP>"],
                "ph_start": [0.0],
                "ph_end": [0.2],
            },
            {
                "word": "hi",
                "start_time": 0.2,
                "end_time": 0.6,
                "ph": ["HH", "AY1"],
                "ph_start": [0.2, 0.35],
                "ph_end": [0.35, 0.6],
            },
            {
                "word": "<AP>",
                "start_time": 0.6,
                "end_time": 0.8,
                "ph": ["<AP>"],
                "ph_start": [0.6],
                "ph_end": [0.8],
            },
        ]
        labels = derive_labels(annotation, 0.8)
        self.assertEqual(labels.events["phoneme"], [0.2, 0.35])
        self.assertEqual(labels.events["syllable"], [0.35])
        self.assertEqual(labels.events["breath"], [0.6])
        self.assertEqual(labels.events["silence"], [0.0])


class ManifestTest(unittest.TestCase):
    def test_builds_record_from_expected_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            clip = root / "English/EN-Alto-1/Vibrato/song/Control_Group/0001.wav"
            clip.parent.mkdir(parents=True)
            with wave.open(str(clip), "wb") as wav_file:
                wav_file.setnchannels(1)
                wav_file.setsampwidth(2)
                wav_file.setframerate(16_000)
                wav_file.writeframes(b"\0\0" * 16_000)
            annotation = [
                {
                    "word": "a",
                    "start_time": 0.0,
                    "end_time": 1.0,
                    "ph": ["AH0"],
                    "ph_start": [0.0],
                    "ph_end": [1.0],
                }
            ]
            clip.with_suffix(".json").write_text(json.dumps(annotation))

            record = build_record(root, clip)

            self.assertEqual(record["language"], "English")
            self.assertEqual(record["event_counts"]["phoneme"], 1)
            self.assertEqual(record["event_counts"]["syllable"], 1)
            self.assertEqual(record["split"], stable_split(record["group_key"]))


@unittest.skipUnless(TORCH_STACK_AVAILABLE, "baseline Python stack is unavailable")
class BaselineTest(unittest.TestCase):
    @staticmethod
    def make_example(targets: np.ndarray) -> Example:
        return Example(
            features=np.zeros((64, targets.shape[1]), dtype=np.float32),
            targets=targets,
            event_frames=tuple(np.asarray([], dtype=np.int64) for _ in range(4)),
            record_id="test",
        )

    def test_targets_and_padding_have_stable_shapes(self) -> None:
        targets = make_targets(
            {
                "phoneme": [0.1],
                "syllable": [0.2],
                "breath": [0.3],
                "silence": [0.4],
            },
            {
                "breath": [[0.3, 0.35]],
                "silence": [[0.4, 0.42]],
            },
            frame_count=50,
            tolerance_ms=10.0,
        )
        example = Example(
            features=np.zeros((64, 50), dtype=np.float32),
            targets=targets,
            event_frames=(
                np.asarray([10]),
                np.asarray([20]),
                np.asarray([30]),
                np.asarray([40]),
            ),
            record_id="test",
        )
        dataset = ChunkDataset([example], chunk_frames=80)
        features, padded_targets = dataset[0]
        self.assertEqual(tuple(features.shape), (64, 80))
        self.assertEqual(tuple(padded_targets.shape), (4, 80))
        self.assertEqual(float(padded_targets.sum()), 13.0)

    def test_interval_targets_fill_half_open_frame_ranges(self) -> None:
        targets = make_targets(
            {
                "phoneme": [],
                "syllable": [],
                "breath": [0.02],
                "silence": [0.07],
            },
            {
                "breath": [[0.02, 0.05]],
                "silence": [[0.07, 0.10]],
            },
            frame_count=12,
            tolerance_ms=30.0,
        )
        np.testing.assert_array_equal(
            np.flatnonzero(targets[2]), np.asarray([2, 3, 4])
        )
        np.testing.assert_array_equal(
            np.flatnonzero(targets[3]), np.asarray([7, 8, 9])
        )

    def test_peak_extraction_collapses_wide_activation(self) -> None:
        probabilities = np.asarray([0.0, 0.6, 0.8, 0.8, 0.7, 0.0])
        peaks = extract_event_frames(
            probabilities, threshold=0.5, min_distance_frames=2
        )
        np.testing.assert_array_equal(peaks, np.asarray([3]))

    def test_rising_edges_return_one_onset_per_active_interval(self) -> None:
        probabilities = np.asarray([0.8, 0.7, 0.1, 0.6, 0.9, 0.7, 0.1])
        edges = extract_rising_edges(probabilities, threshold=0.5)
        np.testing.assert_array_equal(edges, np.asarray([0, 3]))

    def test_event_matching_is_one_to_one_with_tolerance(self) -> None:
        counts = match_events(
            predicted_frames=np.asarray([9, 11, 31]),
            expected_frames=np.asarray([10, 30]),
            tolerance_frames=2,
        )
        self.assertEqual(counts, (2, 1, 0))

    def test_calibration_improves_rare_event_precision(self) -> None:
        targets = np.zeros((4, 12), dtype=np.float32)
        targets[2, 8:10] = 1.0
        targets[3, 8:10] = 1.0
        examples = [
            Example(
                features=np.zeros((64, 12), dtype=np.float32),
                targets=targets,
                event_frames=(
                    np.asarray([2]),
                    np.asarray([2]),
                    np.asarray([8]),
                    np.asarray([8]),
                ),
                record_id="test",
            )
        ]
        probabilities = [
            np.asarray(
                [
                    [0.0, 0.1, 0.9, 0.1, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.0, 0.0],
                    [0.0, 0.1, 0.9, 0.1, 0.0, 0.1, 0.0, 0.1, 0.0, 0.0, 0.0, 0.0],
                    [0.0, 0.6, 0.0, 0.6, 0.0, 0.6, 0.0, 0.1, 0.8, 0.0, 0.0, 0.0],
                    [0.0, 0.6, 0.0, 0.6, 0.0, 0.6, 0.0, 0.1, 0.8, 0.0, 0.0, 0.0],
                ],
                dtype=np.float32,
            )
        ]
        fixed = event_metrics(
            probabilities,
            examples,
            thresholds=(0.5, 0.5, 0.5, 0.5),
            tolerance_frames=0,
            min_distance_frames=1,
        )
        thresholds, calibrated = calibrate_thresholds(
            probabilities,
            examples,
            tolerance_frames=0,
            min_distance_frames=1,
        )
        self.assertGreater(thresholds[2], 0.6)
        fixed_segmentation = segmentation_metrics(
            probabilities,
            examples,
            thresholds=(0.5, 0.5, 0.5, 0.5),
        )
        self.assertGreater(
            calibrated["segmentation_metrics"]["breath"]["precision"],
            fixed_segmentation["breath"]["precision"],
        )
        self.assertGreater(
            calibrated["event_metrics"]["breath"]["precision"],
            fixed["breath"]["precision"],
        )

    def test_interval_heads_use_sqrt_inverse_frequency_weighting(self) -> None:
        targets = np.zeros((4, 10), dtype=np.float32)
        targets[0, :1] = 1.0
        targets[1, :2] = 1.0
        targets[2, :4] = 1.0
        targets[3, :1] = 1.0
        weights = positive_weights([self.make_example(targets)]).numpy()
        np.testing.assert_allclose(weights, np.asarray([9.0, 4.0, np.sqrt(1.5), 3.0]))

    def test_syllable_hard_negative_loss_penalizes_phoneme_only_frames(self) -> None:
        targets = torch.zeros(1, 4, 3)
        targets[0, 0, 1] = 1.0
        logits = torch.zeros_like(targets)
        positive_weight = torch.ones(4)

        baseline = training_loss(logits, targets, positive_weight)
        hard_negative = training_loss(
            logits,
            targets,
            positive_weight,
            syllable_hard_negative_weight=3.0,
        )

        self.assertGreater(float(hard_negative), float(baseline))

    def test_primary_validation_score_uses_all_four_tasks(self) -> None:
        validation = {
            "calibrated_event_metrics": {
                "phoneme": {"f1": 0.8},
                "syllable": {"f1": 0.6},
            },
            "calibrated_segmentation_metrics": {
                "breath": {"f1": 0.4},
                "silence": {"f1": 0.2},
            },
        }
        self.assertAlmostEqual(primary_validation_score(validation), 0.5)

    def test_checkpoint_state_clone_is_detached(self) -> None:
        model = torch.nn.Linear(2, 1)
        cloned = clone_state_dict(model)
        with torch.no_grad():
            model.weight.add_(1.0)
        self.assertFalse(torch.equal(cloned["weight"], model.weight.cpu()))

    def test_residual_tcn_preserves_time_and_four_heads(self) -> None:
        model = BoundaryNet(feature_count=64)
        output = model(torch.randn(2, 64, 401))
        self.assertEqual(tuple(output.shape), (2, 4, 401))
        dilations = tuple(
            block.temporal.dilation[0] for block in model.temporal_blocks
        )
        self.assertEqual(dilations, TCN_DILATIONS)
        self.assertLess(sum(parameter.numel() for parameter in model.parameters()), 75_000)

    def test_residual_tcn_receptive_field_is_310_ms(self) -> None:
        self.assertEqual(receptive_field_frames(), 31)
        model = BoundaryNet(feature_count=64)
        with torch.no_grad():
            for parameter in model.parameters():
                parameter.fill_(1.0)
        features = torch.ones(1, 64, 101, requires_grad=True)
        model(features)[0, 0, 50].backward()
        temporal_support = torch.nonzero(
            features.grad.abs().sum(dim=(0, 1)) > 0,
            as_tuple=False,
        ).flatten()
        self.assertEqual(len(temporal_support), receptive_field_frames())
        self.assertEqual(int(temporal_support[0]), 35)
        self.assertEqual(int(temporal_support[-1]), 65)


if __name__ == "__main__":
    unittest.main()
