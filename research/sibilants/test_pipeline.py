import copy
from pathlib import Path
import tempfile
import unittest
import numpy as np
import soundfile as sf
import torch

from .pipeline import FEATURES, SibilantNet, features, labels, merge_sibilant_notes, pair_statistics
from .train import probabilities, thresholds


class PipelineChecks(unittest.TestCase):
    def test_aligned_pair_produces_frame_labels_without_loading_whole_files(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            raw = np.full((48000, 2), 0.1, np.float32)
            ess = np.zeros_like(raw); ess[9600:28800] = raw[9600:28800]
            sf.write(root / "raw.wav", raw, 48000, subtype="FLOAT")
            sf.write(root / "ess.wav", ess, 48000, subtype="FLOAT")
            values = pair_statistics(root / "raw.wav", root / "ess.wav")
            target = labels(values["raw_rms"], values["ess_rms"])
            self.assertEqual(len(target), 100)
            self.assertEqual(float(target[40]), 1.0)
            self.assertEqual(float(target[5]), 0.0)

    def test_attachment_threshold_is_not_more_permissive_than_balanced(self):
        prediction = np.array([0.99, 0.95, 0.8, 0.2, 0.01, 0.03, 0.04, 0.02])
        target = np.array([1, 1, 1, 1, 0, 0, 0, 0], np.float32)
        balanced, strict = thresholds(prediction, target)
        self.assertGreaterEqual(strict[0] + 1e-9, max(0.5, balanced[0]))

    def test_labels_use_stem_and_ignore_uncertain_fades(self):
        raw = np.full(100, 0.1, np.float32)
        ess = np.zeros(100, np.float32)
        ess[20:60] = 0.1
        ess[70:80] = 1e-6
        target = labels(raw, ess)
        self.assertEqual(float(target[40]), 1)
        self.assertEqual(float(target[5]), 0)
        self.assertTrue(np.isnan(target[20]))
        self.assertTrue(np.isnan(target[75]))

    def test_short_sibilants_keep_a_positive_training_frame(self):
        raw = np.full(20, 0.1, np.float32)
        ess = np.zeros(20, np.float32)
        ess[10:12] = 0.1
        target = labels(raw, ess)
        self.assertTrue(np.any(target[10:12] == 1))

    def test_feature_timing_and_silence_are_finite(self):
        for sr in (16000, 24000, 44100, 48000):
            x = features(np.zeros((sr, 2), np.float32), sr)
            self.assertEqual(x.shape, (FEATURES, 100))
            self.assertTrue(np.isfinite(x).all())
        with self.assertRaises(ValueError):
            features(np.full(100, np.nan), 48000)

    def test_chunked_predictions_match_full_context(self):
        torch.set_num_threads(2)
        torch.manual_seed(4)
        model = SibilantNet().eval()
        x = np.random.default_rng(1).normal(size=(FEATURES, 4500)).astype(np.float32)
        with torch.inference_mode():
            expected = torch.sigmoid(model(torch.from_numpy(x[None])))[0].numpy()
        actual = probabilities(model, x, np.zeros(FEATURES), np.ones(FEATURES), torch.device("cpu"))
        self.assertLess(float(np.max(np.abs(actual - expected))), 1e-6)

    def test_join_sibilant_tail_keeps_source_and_timing(self):
        notes = [{"id": "voice", "start": 0.0, "end": 0.5}, {"id": "ess", "start": 0.5, "end": 0.8}]
        original = copy.deepcopy(notes)
        p = np.zeros(100); p[50:80] = 1
        merged, proposals = merge_sibilant_notes(notes, p, 0.9)
        self.assertEqual(notes, original)
        self.assertEqual(len(merged), 1)
        self.assertEqual((merged[0]["start"], merged[0]["end"]), (0.0, 0.8))
        self.assertEqual(proposals[0]["direction"], "previous")

    def test_join_attack_to_following_note(self):
        notes = [{"id": "ess", "start": 0.0, "end": 0.2}, {"id": "voice", "start": 0.2, "end": 0.8}]
        p = np.zeros(100); p[:20] = 1
        merged, proposals = merge_sibilant_notes(notes, p, 0.9)
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0]["id"], "voice")
        self.assertEqual(proposals[0]["direction"], "next")

    def test_ties_prefer_previous_and_partial_or_distant_notes_are_kept(self):
        notes = [{"id": "a", "start": 0.0, "end": 0.3}, {"id": "ess", "start": 0.3, "end": 0.5},
                 {"id": "b", "start": 0.5, "end": 0.8}]
        p = np.zeros(100); p[30:50] = 1
        merged, proposals = merge_sibilant_notes(notes, p, 0.9)
        self.assertEqual(len(merged), 2)
        self.assertEqual(proposals[0]["action"], "merge")
        self.assertEqual(proposals[0]["direction"], "previous")
        p[40:50] = 0
        self.assertEqual(len(merge_sibilant_notes(notes, p, 0.9)[0]), 3)
        distant = [{"id": "a", "start": 0.0, "end": 0.1}, {"id": "ess", "start": 0.5, "end": 0.8}]
        p[:] = 0; p[50:80] = 1
        self.assertEqual(len(merge_sibilant_notes(distant, p, 0.9)[0]), 2)

    def test_majority_white_time_and_fractional_edges(self):
        notes = [{"id": "left", "start": 0.0, "end": 0.4}, {"id": "ess", "start": 0.4, "end": 0.6},
                 {"id": "right", "start": 0.6, "end": 1.0}]
        p = np.zeros(100); p[40:50] = 0.6
        self.assertEqual(len(merge_sibilant_notes(notes, p, 0.48)[0]), 3)
        p[50] = 0.6
        merged, proposals = merge_sibilant_notes(notes, p, 0.48)
        self.assertEqual(len(merged), 2)
        self.assertAlmostEqual(proposals[0]["coverage"], 0.55)
        notes[1].update(start=0.409, end=0.421)
        notes[2]["start"] = 0.421
        p[:] = 0; p[41] = 0.6
        merged, proposals = merge_sibilant_notes(notes, p, 0.48)
        self.assertEqual(len(merged), 2)
        self.assertEqual(proposals[0]["target_id"], "right")

    def test_voiced_core_protects_notes_with_long_consonants(self):
        notes = [{"id": "left", "start": 0.0, "end": 0.4}, {"id": "ess", "start": 0.4, "end": 0.6},
                 {"id": "right", "start": 0.6, "end": 1.0}]
        p = np.zeros(100); p[40:51] = 0.6
        notes[1]["curve"] = [{"time": i / 100, "midi": 60 + (i - 52) * 0.5, "confidence": 0.95} for i in range(52, 60)]
        self.assertEqual(len(merge_sibilant_notes(notes, p, 0.48)[0]), 3)
        for point in notes[1]["curve"]:
            point["confidence"] = 0.6
        self.assertEqual(len(merge_sibilant_notes(notes, p, 0.48)[0]), 2)
        notes[1]["end"] = notes[2]["start"] = 0.5
        p[:] = 0; p[40:46] = 0.6
        notes[1]["curve"] = [{"time": i / 100, "midi": 60, "confidence": 0.95} for i in range(46, 50)]
        self.assertEqual(len(merge_sibilant_notes(notes, p, 0.48)[0]), 3)
        p[46:50] = 0.6
        self.assertEqual(len(merge_sibilant_notes(notes, p, 0.48)[0]), 2)


if __name__ == "__main__":
    unittest.main()
