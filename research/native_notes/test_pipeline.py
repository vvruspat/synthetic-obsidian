import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import soundfile as sf
import torch

from .model import features, targets, decode, matched_events, scores
from .train import read_dataset, Windows, metrics, calibrate, selection_score
from .train import boundary_loss_weights
from .multiscale import MultiScaleNoteBoundaryNet, multiscale_features
from .model import NoteBoundaryNet
from .compare import fragmentation, connected_boundaries
from .voicing import VoicingNoteBoundaryNet, voicing_features
from .corrections import error_centers, replay_starts, add_pairs, digest
from .augmentation import SCHEMA as AUGMENTATION_SCHEMA, derivative_record, read_prepared, mask_uncertain_targets
from .select_guarded import calibrated_cohorts
from .align_synthetic import ContentEncoder, align, map_notes
from .build_augmented_dataset import cached_tensor_call


class PipelineTests(unittest.TestCase):
    def test_inference_cache_keys_waveform_values_parameters_and_evicts_old_entries(self):
        calls = []
        def operation(wave, scale=1.):
            calls.append(1)
            return wave * scale
        cached = cached_tensor_call(operation, capacity=2)
        torch.testing.assert_close(cached(torch.tensor([1., 2.]), scale=2.), torch.tensor([2., 4.]))
        cached(torch.tensor([1., 2.]), scale=2.)
        self.assertEqual(len(calls), 1)
        cached(torch.tensor([1., 3.]), scale=2.)
        cached(torch.tensor([1., 2.]), scale=3.)
        self.assertEqual(len(calls), 3)
        cached(torch.tensor([1., 2.]), scale=2.)
        self.assertEqual(len(calls), 4)

    def test_long_content_encoding_retains_a_single_global_grid(self):
        from types import SimpleNamespace
        encoder = ContentEncoder.__new__(ContentEncoder)
        encoder.device = "cpu"
        encoder.extractor = lambda wave, **kwargs: SimpleNamespace(input_features=torch.from_numpy(wave)[None, None])
        def fake_model(features, **kwargs):
            hidden = features[:, :, ::320].transpose(1, 2)
            return SimpleNamespace(hidden_states=[hidden] * 9)
        encoder.model = fake_model
        encoder.pitch = SimpleNamespace(infer_from_audio=lambda wave, **kwargs: np.full(len(wave) // 160 + 1, 220.))
        audio = np.arange(37 * 16000, dtype=np.float32) / 16000
        encoded = encoder.encode(audio, 16000)
        self.assertEqual(len(encoded["content"]), len(audio) // 320 + 1)
        self.assertEqual(len(encoded["f0"]), len(encoded["content"]))
        np.testing.assert_allclose(encoded["content"][:-1, 0], encoded["times"][:-1], atol=1e-5)
        self.assertEqual(encoded["duration"], 37.)

    def test_uncertain_synthetic_regions_remove_all_supervision_including_activity(self):
        expected = np.ones((3, 101), np.float32)
        mask_uncertain_targets(expected, {"ignore_intervals": [[.3, .6]]})
        self.assertTrue(np.isnan(expected[:, 30:61]).all())
        self.assertTrue(np.isfinite(expected[:, :30]).all())
        self.assertTrue(np.isfinite(expected[:, 61:]).all())
        # Class balancing must not treat masked frames as positives or negatives.
        np.testing.assert_array_equal(np.nansum(expected, axis=1), [70, 70, 70])

    def test_sampling_can_balance_many_synthetic_copies_against_the_human_original(self):
        from torch.utils.data import WeightedRandomSampler
        examples = [{"features": np.zeros((138, 400), np.float32), "targets": np.zeros((3, 400), np.float32)}
                    for _ in range(5)]
        for example in examples[1:]: example["augmentation"] = {}
        dataset = Windows(examples)
        sampler = WeightedRandomSampler(dataset.sampling_weights(.25), 2000, replacement=True,
                                        generator=torch.Generator().manual_seed(5))
        human_fraction = np.mean([dataset.index[i][0] == 0 for i in sampler])
        self.assertGreater(human_fraction, .45)
        self.assertLess(human_fraction, .55)

    def test_masked_synthetic_frames_have_no_loss_gradient(self):
        expected = targets([{"start": .1, "end": .7}], 100)
        mask_uncertain_targets(expected, {"ignore_intervals": [[.25, .5]]})
        expected = torch.from_numpy(expected)[None]
        logits = torch.zeros_like(expected, requires_grad=True)
        valid = torch.isfinite(expected)
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(logits, torch.nan_to_num(expected), reduction="none")
                * valid).sum() / valid.sum()
        loss.backward()
        self.assertTrue(torch.isfinite(loss))
        self.assertTrue(torch.all(logits.grad[:, :, 25:51] == 0))
        self.assertTrue(torch.any(logits.grad[:, :, :25] != 0))

    def test_synthetic_alignment_recovers_time_warp_with_timbre_offset_and_octave_shift(self):
        rng = np.random.default_rng(5)
        count = 151
        times = np.arange(count) * .02
        # Smooth, distinctive phone-like trajectories and an independent pitch transition.
        from scipy.ndimage import gaussian_filter1d
        content = gaussian_filter1d(rng.normal(size=(count, 24)), 1, axis=0)
        source = {"content": content, "f0": 250 + 30 * np.sin(times * 3), "energy": np.sin(times * 5)}
        target_times = times + .06 * np.sin(np.pi * times / times[-1]) ** 2
        rendered = {"content": np.stack([np.interp(times, target_times, c) for c in content.T], axis=1) + 2,
                    "f0": np.interp(times, target_times, source["f0"]) / 2,
                    "energy": np.interp(times, target_times, source["energy"]) + 1}
        mapping = align(source, rendered, -12)
        estimated = np.interp(times, mapping["source_times"], mapping["target_times"])
        self.assertLess(float(np.mean(abs(estimated - target_times))), .011)
        self.assertTrue(np.all(np.diff(mapping["source_times"]) > 0))
        self.assertTrue(np.all(np.diff(mapping["target_times"]) > 0))

    def test_synthetic_note_transfer_keeps_shared_boundaries_and_flags_weak_matches(self):
        mapping = {"source_times": np.array([0., .5, 1.]), "target_times": np.array([0., .54, 1.]),
                   "alternate_times": np.array([0., .54, 1.]), "content_cost": np.ones((51, 51))}
        source = [{"id": "n1", "start": .1, "end": .5, "pitch": 60, "curve": [{"time": .2}]},
                  {"id": "n2", "start": .5, "end": .9, "pitch": 62}]
        transferred, audit = map_notes(source, mapping, 1., -12)
        self.assertEqual(len(transferred), len(source))
        self.assertEqual(transferred[0]["end"], transferred[1]["start"])
        self.assertNotIn("curve", transferred[0])
        self.assertEqual(transferred[0]["pitch"], 48)
        self.assertTrue(all("weak_content_match" in a["review_reasons"] for a in audit))
        self.assertEqual(source[0]["end"], .5)

    def test_synthetic_alignment_uses_pitch_change_inside_a_sustained_vowel(self):
        count = 151
        content = np.zeros((count, 8), np.float32)
        f0 = np.full(count, 300.); f0[75:] = 350.
        converted = np.full(count, 150.); converted[78:] = 175.
        source = {"content": content, "f0": f0, "energy": np.zeros(count)}
        rendered = {"content": content, "f0": converted, "energy": np.zeros(count)}
        mapping = align(source, rendered, -12)
        mapped = np.interp(1.5, mapping["source_times"], mapping["target_times"])
        self.assertLessEqual(abs(mapped - 1.56), .02000001)

    def test_prepared_derivatives_cannot_leak_heldout_audio_or_change_human_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = []
            for i, split in enumerate(("train", "validation", "test")):
                audio = root / f"original-{i}.wav"
                sf.write(audio, np.sin(np.arange(8000) * (.1 + i * .01)).astype(np.float32) * .1, 8000)
                annotation = root / f"original-{i}.json"
                notes = [{"id": "n1", "start": .1, "end": .7}]
                annotation.write_text(json.dumps({"sample_rate": 8000, "duration": 1., "notes": notes}))
                records.append({"id": f"original-{i}", "audio": str(audio), "audio_sha256": digest(audio),
                    "annotation": str(annotation), "annotation_sha256": digest(annotation), "seconds": 1.,
                    "sample_rate": 8000, "split": split, "group": f"clip:{i}", "notes": notes})
            base = root / "base.json"; base.write_text(json.dumps(records))
            derivative_audio = root / "derivative.wav"
            sf.write(derivative_audio, np.sin(np.arange(8000) * .06).astype(np.float32) * .1, 8000)
            derivative = derivative_record(records[0], derivative_audio, -6, "test-render", {})
            document = {"schema": AUGMENTATION_SCHEMA, "base_manifest": str(base),
                        "base_manifest_sha256": digest(base), "records": records + [derivative]}
            prepared = root / "prepared.json"
            def check():
                prepared.write_text(json.dumps(document)); return read_prepared(prepared)
            self.assertEqual(check()[-1]["split"], "train")
            self.assertEqual(check()[-1]["notes"][0]["start"], .1)
            self.assertNotIn("pitch", check()[-1]["notes"][0])
            derivative["augmentation"]["parent_id"] = records[1]["id"]
            with self.assertRaisesRegex(ValueError, "original training"): check()
            derivative["augmentation"]["parent_id"] = records[0]["id"]
            derivative["augmentation"]["reference_id"] = records[2]["id"]
            with self.assertRaisesRegex(ValueError, "references"): check()
            del derivative["augmentation"]["reference_id"]
            document["records"][1]["split"] = "train"
            with self.assertRaisesRegex(ValueError, "frozen original"): check()
            document["records"][1]["split"] = "validation"
            annotation = Path(derivative["annotation"])
            mapping_path = root / "alignment.json"
            mapping = {"source_times": [0., .5, 1.], "target_times": [0., .54, 1.],
                       "source_sha256": records[0]["audio_sha256"], "synthetic_sha256": derivative["audio_sha256"]}
            mapping_path.write_text(json.dumps(mapping))
            derivative["augmentation"].update(alignment=str(mapping_path), alignment_sha256=digest(mapping_path))
            warped = json.loads(annotation.read_text())
            for note in warped["notes"]:
                for key in ("start", "end"):
                    note[key] = float(np.interp(note[key], mapping["source_times"], mapping["target_times"]))
            annotation.write_text(json.dumps(warped))
            derivative["notes"] = warped["notes"]; derivative["annotation_sha256"] = digest(annotation)
            self.assertEqual(len(check()), 4)
            mapping["target_times"] = [0., .8, .7]
            mapping_path.write_text(json.dumps(mapping))
            derivative["augmentation"]["alignment_sha256"] = digest(mapping_path)
            with self.assertRaisesRegex(ValueError, "alignment map"): check()
            mapping["target_times"] = [0., .54, 1.]
            mapping_path.write_text(json.dumps(mapping))
            derivative["augmentation"]["alignment_sha256"] = digest(mapping_path)
            altered = json.loads(annotation.read_text()); altered["notes"][0]["start"] = .2
            annotation.write_text(json.dumps(altered))
            derivative["annotation_sha256"] = digest(annotation)
            derivative["notes"] = altered["notes"]
            with self.assertRaisesRegex(ValueError, "parent labels"): check()

    def test_cohort_calibration_cannot_trade_one_cohorts_transitions_for_another(self):
        p = np.zeros((3, 100), np.float32)
        p[2, 10:90] = 1; p[0, [10, 50]] = [1, .3]; p[1, [50, 90]] = [.3, 1]
        q = p.copy(); q[:2, 50] = 0
        examples = [{"seconds": 1., "validation_cohort": name,
                     "notes": [{"start": .1, "end": .5}, {"start": .5, "end": .9}]}
                    for name in ("legacy", "new")]
        floors = {name: {"onsets_matched": 1, "offsets_matched": 1} for name in ("legacy", "new")}
        self.assertIsNone(calibrated_cohorts([p, q], examples, floors))
        candidate = calibrated_cohorts([p, p], examples, floors)
        self.assertIsNotNone(candidate)
        self.assertLessEqual(candidate[1][0], .3)

    def test_extended_calibration_removes_confident_false_split_and_keeps_transition(self):
        p = np.zeros((3, 100), np.float32)
        p[2, 10:90] = 1
        p[0, [10, 30, 50]] = [1, .88, .96]
        p[1, [30, 50, 90]] = [.88, .96, 1]
        examples = [{"seconds": 1., "validation_cohort": "legacy",
                     "notes": [{"start": .1, "end": .5}, {"start": .5, "end": .9}]}]
        floors = {"legacy": {"onsets_matched": 1, "offsets_matched": 1}}
        coarse = calibrated_cohorts([p], examples, floors)
        extended = calibrated_cohorts([p], examples, floors, extended_thresholds=True)
        self.assertGreater(extended[0], coarse[0])
        evaluation = extended[2]["legacy"]
        self.assertEqual(evaluation["onset_50ms"]["extra"], 0)
        self.assertEqual(evaluation["note_boundaries"]["f1"], 1.)
        self.assertEqual(evaluation["connected_boundaries"]["onsets_matched"], 1)
        self.assertEqual(evaluation["connected_boundaries"]["offsets_matched"], 1)

    def test_voicing_warm_start_exactly_preserves_multiscale_predictions(self):
        torch.manual_seed(1701)
        old = MultiScaleNoteBoundaryNet().eval()
        new = VoicingNoteBoundaryNet().eval()
        new.initialize_multiscale(old.state_dict())
        x = torch.randn(1, 138, 150)
        with torch.inference_mode():
            torch.testing.assert_close(new(x), old(x[:, :131]), atol=1e-6, rtol=1e-5)

    def test_voicing_cues_detect_transition_and_are_transposition_invariant(self):
        frames = np.tile([60.0, 0.95, 1.0], (200, 1))
        frames[100:, 0] = 62
        cues = voicing_features(frames, 201)
        self.assertAlmostEqual(float(cues[4, 100]), 2 / 6, places=6)
        self.assertEqual(float(cues[5, 100]), 1.0)
        self.assertAlmostEqual(float(cues[4, 50]), 0.0, places=6)
        shifted = frames.copy(); shifted[:, 0] += 12
        np.testing.assert_allclose(cues, voicing_features(shifted, 201), atol=1e-6)
        self.assertEqual(float(cues[1, -1]), 0.0)  # STFT's terminal frame is not invented voicing.

    def test_unvoiced_and_unsupported_pitch_is_never_filled_through_gaps(self):
        frames = np.tile([60.0, 0.95, 1.0], (100, 1))
        frames[30:70, 0] = 110
        frames[30:70, 1:] = 0
        cues = voicing_features(frames, 101)
        self.assertTrue(np.isfinite(cues).all())
        np.testing.assert_array_equal(cues[1:6, 50], 0)
        np.testing.assert_array_equal(voicing_features(np.zeros((100, 3)), 101), 0)
        with self.assertRaises(ValueError): voicing_features(frames, 103)
        frames[0, 2] = 0.5
        with self.assertRaises(ValueError): voicing_features(frames, 101)

    def test_correction_replay_balances_false_and_missed_edges(self):
        reference = [{"start": 0.1, "end": 0.6}, {"start": 0.6, "end": 1.0}]
        proposal = [{"start": 0.1, "end": 0.3}, {"start": 0.3, "end": 1.0}]
        centers = error_centers(reference, proposal)
        self.assertEqual(centers, {"extra": [0.3], "missed": [0.6]})
        example = {"features": np.zeros((138, 1000)), "correction_centers": centers}
        self.assertEqual(len(replay_starts(example, 400, 2)), 4)
        self.assertTrue(all(0 <= start <= 600 for start in replay_starts(example, 400, 2)))

    def test_explicit_correction_cannot_reassign_heldout_audio_or_changed_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            audio = root / "voice.wav"
            sf.write(audio, np.zeros(8000, np.float32), 8000)
            audio_hash = digest(audio)
            document = {"audio": "voice.wav", "duration": 1.0, "sample_rate": 8000,
                        "dataset": {"audio_sha256": audio_hash}, "notes": [{"id": "n1", "start": 0.1, "end": 0.6}]}
            manual, proposal = root / "manual.json", root / "proposal.json"
            manual.write_text(json.dumps(document)); proposal.write_text(json.dumps(document))
            pair = {"audio": str(audio), "manual": str(manual), "proposal": str(proposal),
                    "audio_sha256": audio_hash, "manual_sha256": digest(manual), "proposal_sha256": digest(proposal)}
            manifest = root / "pairs.json"; manifest.write_text(json.dumps([pair]))
            heldout = [{"id": audio_hash, "audio_sha256": audio_hash, "split": "test"}]
            with self.assertRaises(ValueError): add_pairs(heldout, manifest)
            records = []; add_pairs(records, manifest)
            self.assertEqual(records[0]["split"], "train")
            self.assertEqual(records[0]["review_authority"], "explicit_human_correction_pair")
            manual.write_text(manual.read_text() + " ")
            with self.assertRaises(ValueError): add_pairs([], manifest)

    def test_calibration_reports_no_candidate_when_transition_floor_is_impossible(self):
        p = np.zeros((3, 100), np.float32)
        p[2, 10:90] = 1; p[0, 10] = 1; p[1, 90] = 1
        examples = [{"seconds": 1.0, "notes": [{"start": 0.1, "end": 0.5}, {"start": 0.5, "end": 0.9}]}]
        _, evaluation = calibrate([p], examples, {"onsets_matched": 1, "offsets_matched": 1})
        self.assertIsNone(evaluation)

    def test_calibration_can_retain_a_weak_connected_transition(self):
        p = np.zeros((3, 100), np.float32)
        p[2, 10:90] = 1; p[0, [10, 50]] = [1, 0.3]; p[1, [50, 90]] = [0.3, 1]
        examples = [{"seconds": 1.0, "notes": [{"start": 0.1, "end": 0.5}, {"start": 0.5, "end": 0.9}]}]
        thresholds, evaluation = calibrate([p], examples, {"onsets_matched": 1, "offsets_matched": 1})
        self.assertIsNotNone(evaluation)
        self.assertLessEqual(thresholds[0], 0.3)
        self.assertEqual(evaluation["connected_boundaries"]["onsets_missed"], 0)

    def test_connected_transitions_count_merged_notes_as_missed(self):
        examples = [{"notes": [{"start": 0.1, "end": 0.5}, {"start": 0.5, "end": 1.0},
                               {"start": 1.2, "end": 1.6}, {"start": 1.62, "end": 2.0}]}]
        merged = connected_boundaries([[(0.1, 1.0), (1.2, 2.0)]], examples)
        self.assertEqual(merged["reference_transitions"], 2)
        self.assertEqual(merged["onsets_missed"], 2)
        self.assertEqual(merged["offsets_missed"], 2)
        split = connected_boundaries([[(0.1, 0.5), (0.5, 1.0), (1.2, 1.6), (1.62, 2.0)]], examples)
        self.assertEqual(split["onset_recall"], 1.0)
        self.assertEqual(split["offset_recall"], 1.0)

    def test_default_loss_has_no_extra_interior_penalty(self):
        y = targets([{"start": 0.1, "end": 0.5}, {"start": 0.5, "end": 0.9}], 100)
        weights = boundary_loss_weights(torch.from_numpy(y)[None], 1.0)
        torch.testing.assert_close(weights, torch.ones_like(weights))
        self.assertEqual(y[0, 50], 1.0)
        self.assertEqual(y[1, 50], 1.0)
        self.assertTrue((y[2, 10:90] == 1.0).all())

    def test_fragmentation_does_not_confuse_one_delayed_note_with_a_split(self):
        examples = [{"notes": [{"start": 0.1, "end": 0.8}]}]
        self.assertEqual(fragmentation([[(0.2, 0.8)]], examples)["extra_fragments"], 0)
        self.assertEqual(fragmentation([[(0.1, 0.25), (0.25, 0.8)]], examples)["extra_fragments"], 1)

    def test_multiscale_training_crop_has_full_inference_context(self):
        torch.manual_seed(1701)
        model = MultiScaleNoteBoundaryNet().eval()
        x = np.random.default_rng(1701).normal(size=(131, 800)).astype(np.float32)
        cropped, _ = Windows([{"features": x, "targets": np.zeros((3, 800), np.float32)}], context=47)[1]
        with torch.inference_mode():
            full = model(torch.from_numpy(x)[None])[0, :, 200:600]
            window = model(cropped[None])[0, :, 47:447]
        torch.testing.assert_close(full, window, atol=1e-6, rtol=1e-5)

    def test_multiscale_warm_start_preserves_legacy_predictions(self):
        torch.manual_seed(1701)
        legacy = NoteBoundaryNet().eval()
        model = MultiScaleNoteBoundaryNet().eval()
        model.initialize_legacy(legacy.state_dict())
        x = torch.randn(1, 131, 150)
        with torch.inference_mode():
            torch.testing.assert_close(legacy(x[:, :64]), model(x), atol=1e-6, rtol=1e-5)

    def test_multiscale_inputs_distinguish_noise_and_keep_short_branch(self):
        rng = np.random.default_rng(1701)
        tone = (0.2 * np.sin(np.arange(16000) * 2 * np.pi * 220 / 16000)).astype(np.float32)
        x = multiscale_features(tone, 16000)
        noise = multiscale_features(rng.normal(0, 0.2, 16000).astype(np.float32), 16000)
        self.assertEqual(x.shape, (131, 101))
        self.assertTrue(np.isfinite(x).all())
        np.testing.assert_array_equal(x[:64], features(tone, 16000))
        self.assertGreater(noise[128, 20:80].mean(), x[128, 20:80].mean() + 0.1)
        quiet = multiscale_features(np.zeros(16000, np.float32), 16000)
        self.assertTrue(np.isfinite(quiet).all())
        np.testing.assert_allclose(quiet[130], 0, atol=1e-8)

    def test_interior_penalty_keeps_consonant_attack_and_tail_active(self):
        y = targets([{"start": 0.1, "end": 0.8, "voiced_start": 0.25, "voiced_end": 0.65}], 100)
        weights = boundary_loss_weights(torch.from_numpy(y)[None], 3.0)[0].numpy()
        self.assertEqual(weights[0, 10], 1)
        self.assertEqual(weights[1, 80], 1)
        self.assertEqual(weights[0, 25], 3)
        self.assertEqual(weights[1, 65], 3)
        self.assertEqual(weights[2, 15], 1)
        self.assertEqual(y[2, 15], 1)
        self.assertEqual(y[2, 75], 1)

    def test_features_stereo_cancellation_and_silence(self):
        mono = np.sin(np.arange(16000) * 2 * np.pi * 220 / 16000).astype(np.float32) * 0.2
        inverted = np.stack([mono, -mono], axis=1)
        np.testing.assert_array_equal(features(mono, 16000), features(inverted, 16000))
        quiet = features(np.zeros(44100, dtype=np.float32), 44100)
        self.assertEqual(quiet.shape, (64, 101))
        self.assertTrue(np.isfinite(quiet).all())
        self.assertEqual(float(np.abs(quiet).max()), 0.0)

    def test_note_targets_keep_acoustic_attack(self):
        y = targets([{"start": 0.1, "end": 0.5, "voiced_start": 0.2}], 100)
        self.assertEqual(y[0, 10], 1.0)
        self.assertEqual(y[1, 50], 1.0)
        self.assertEqual(y[2, 10], 1.0)
        self.assertEqual(y[2, 50], 0.0)

    def test_decode_separate_and_connected_notes(self):
        p = np.zeros((3, 150), np.float32)
        p[2, 10:50] = 1; p[2, 70:130] = 1
        p[0, [10, 70, 100]] = 1; p[1, [50, 100, 130]] = 1
        np.testing.assert_allclose(decode(p, 1.5, (0.5, 0.5, 0.5)), [(0.1, 0.5), (0.7, 1.0), (1.0, 1.3)], atol=1e-9)
        self.assertEqual(decode(np.zeros_like(p), 1.5, (0.5, 0.5, 0.5)), [])

    def test_event_matching_one_to_one_and_json(self):
        self.assertEqual(matched_events([0.1, 0.2], [0.11, 0.12]), (1, 1, 1))
        json.dumps(scores(tuple(np.array([1, 2, 3], dtype=np.int64))))

    def test_selection_uses_the_calibrated_decoder(self):
        p = np.zeros((3, 100), np.float32)
        p[2, 10:80] = 1
        p[0, 10] = 1; p[0, 30] = 0.6; p[1, 80] = 1
        examples = [{"seconds": 1.0, "notes": [{"start": 0.1, "end": 0.8}]}]
        thresholds, evaluation = calibrate([p], examples)
        self.assertEqual(selection_score(evaluation), 1.0)
        self.assertGreater(selection_score(evaluation), selection_score(metrics([p], examples, (0.5, 0.5, 0.5))))
        np.testing.assert_allclose(decode(p, 1.0, thresholds), [(0.1, 0.8)])

    def test_training_metrics_keep_boundaries_without_native_pitch(self):
        p = np.zeros((3, 150), np.float32)
        p[2, 10:50] = 1; p[2, 70:130] = 1
        p[0, [10, 70]] = 1; p[1, [50, 130]] = 1
        example = {"seconds": 1.5, "notes": [{"start": 0.7, "end": 1.3}]}
        expected = metrics([p], [example], (0.5, 0.5, 0.5))
        example["voiced_times"] = np.array([0.8, 0.9])
        self.assertEqual(metrics([p], [example], (0.5, 0.5, 0.5)), expected)
        self.assertEqual(expected["onset_50ms"]["extra"], 1)

    def test_drafts_need_explicit_declaration_and_splits_are_disjoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for i in range(6):
                sample = root / str(i); sample.mkdir()
                sf.write(sample / "voice.wav", np.zeros(8000, np.float32), 8000)
                document = {"audio": "voice.wav", "duration": 1.0, "sample_rate": 8000,
                            "notes": [{"id": "n1", "start": 0.1, "end": 0.6}],
                            "dataset": {"source_sha256": str(i).zfill(64), "review_status": "draft"}}
                (sample / "voice.annotation.json").write_text(json.dumps(document))
            with self.assertRaises(ValueError): read_dataset(root, False, 1701)
            records = read_dataset(root, True, 1701)
            retained = read_dataset(root, False, 9999, trusted_manifest=records)
            self.assertEqual({r["id"]: r["split"] for r in retained}, {r["id"]: r["split"] for r in records})
            sets = {split: {r["id"] for r in records if r["split"] == split} for split in ("train", "validation", "test")}
            self.assertFalse(sets["train"] & sets["validation"] or sets["train"] & sets["test"] or sets["test"] & sets["validation"])
            self.assertEqual(len(read_dataset(root, True, 1701)), 6)
            duplicate = root / "copy"; duplicate.mkdir()
            (duplicate / "voice.wav").write_bytes((root / "0/voice.wav").read_bytes())
            (duplicate / "voice.annotation.json").write_bytes((root / "0/voice.annotation.json").read_bytes())
            self.assertEqual(len(read_dataset(root, True, 1701)), 6)

    def test_crop_padding_is_not_supervision(self):
        example = {"features": np.zeros((64, 110), np.float32), "targets": np.zeros((3, 110), np.float32)}
        x, y = Windows([example])[0]
        self.assertEqual(x.shape[1], 430)
        self.assertTrue(np.isnan(y[:, :15]).all())
        self.assertTrue(np.isnan(y[:, 125:]).all())


if __name__ == "__main__":
    unittest.main()
