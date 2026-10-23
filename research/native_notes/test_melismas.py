import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import mido
import numpy as np

from .prepare_melismas import composition, pitch_control, write_midi, write_json, digest
from .finish_melismas import pitch_crossing
from .synthesis_manifest import QA_POLICY, SCHEMA, validate_record


class ControlledMelismaTests(unittest.TestCase):
    def test_random_scores_include_legato_pauses_and_same_pitch_reattacks(self):
        scores = [composition(i, 2431802) for i in range(20)]
        self.assertEqual(scores, [composition(i, 2431802) for i in range(20)])
        self.assertEqual({s["category"] for s in scores},
                         {"legato", "mixed_phrases", "reattacks", "held_vibrato", "edge_drift"})
        for s in scores:
            for a, b in zip(s["notes"], s["notes"][1:]):
                if a["phrase"] == b["phrase"]:
                    self.assertEqual(a["position"]+a["duration"], b["position"])
                else:
                    self.assertGreater(b["position"], a["position"]+a["duration"])
            if s["category"] == "reattacks":
                self.assertEqual(s["notes"][0]["tone"], s["notes"][1]["tone"])
            if s["category"] in ("held_vibrato", "edge_drift"):
                self.assertEqual(len(s["notes"]), 1)

    def test_pitch_control_does_not_insert_notes_into_vibrato_or_drift(self):
        for index in (8, 9):
            score = composition(index, 1802)
            before = json.dumps(score, sort_keys=True)
            curve = np.asarray(pitch_control(score, score["notes"]))
            self.assertTrue(np.isfinite(curve).all())
            self.assertLess(float(abs(curve-score["notes"][0]["tone"]).max()), 1.5)
            self.assertEqual(json.dumps(score, sort_keys=True), before)

    def test_pitch_alignment_handles_up_down_and_missing_voicing(self):
        t = np.arange(200)/100
        for before, after in ((60, 62), (64, 61)):
            p = before+(after-before)*np.clip((t-.98)/.08, 0, 1)
            self.assertAlmostEqual(pitch_crossing(t, p, np.ones(200, bool), 1.0, before, after), 1.02)
            self.assertIsNone(pitch_crossing(t, p, np.zeros(200, bool), 1.0, before, after))

    def test_midi_events_round_trip_with_adjacent_note_off_before_note_on(self):
        with tempfile.TemporaryDirectory() as directory:
            score = composition(0, 1802)
            path = Path(directory)/"score.mid"
            write_midi(path, {**score, "singer": "test"}, "a")
            messages = mido.MidiFile(path).tracks[0]
            current, active, starts = 0, set(), []
            for m in messages:
                current += m.time
                if m.type == "note_off": active.remove(m.note)
                if m.type == "note_on":
                    self.assertFalse(active)
                    active.add(m.note)
                    starts.append(current)
            self.assertFalse(active)
            self.assertEqual(starts, [n["position"] for n in score["notes"]])

    def fixture(self, root):
        recipe = {"bank": "fixture", "bank_files_sha256": {"model.onnx": "weight-digest"},
                  "score": {"bpm": 120}, "notes": [{"position": 480, "duration": 960, "tone": 60}]}
        identity = hashlib.sha256(json.dumps(recipe, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        write_json(root/"recipe.json", recipe)
        plan = {"schema": SCHEMA, "records": [{"id": identity, "recipe_sha256": digest(root/"recipe.json")}],
                "banks": {"fixture": {"files_sha256": recipe["bank_files_sha256"]}}}
        write_json(root/"plan.json", plan)
        write_json(root/"qa.json", {"policy": QA_POLICY, "accepted": True, "ignore_intervals": [],
                   "aligned_boundaries": [{"kind": "phrase_onset", "nominal": .5, "aligned": .48},
                                          {"kind": "phrase_offset", "nominal": 1.5, "aligned": 1.53}]})
        (root/"score.mid").write_bytes(b"fixture-only")
        record = {"id": identity, "split": "train", "seconds": 2,
                  "review_authority": "controlled_synthesis_not_human_reviewed",
                  "annotation": str(root/"annotation.json"), "ignore_intervals": [],
                  "notes": [{"id": "n0001", "start": .48, "end": 1.53, "flags": ["synthetic"]}],
                  "synthesis": {"bank": "fixture"}}
        for key, filename in (("recipe", "recipe.json"), ("plan", "plan.json"), ("qa", "qa.json"), ("midi", "score.mid")):
            record["synthesis"].update({key: filename, key+"_sha256": digest(root/filename)})
        write_json(root/"annotation.json", {"dataset": {"review_status": "synthetic"},
                   "training": {"ignore_intervals": []}})
        return record

    def test_training_contract_rejects_relabelled_or_heldout_synthetic_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = self.fixture(root)
            validate_record(record, root, {})
            record["split"] = "validation"
            with self.assertRaises(ValueError): validate_record(record, root, {})
            record["split"] = "train"
            record["notes"][0]["start"] += .1
            with self.assertRaises(ValueError): validate_record(record, root, {})

    def test_training_contract_rejects_changed_qa_and_false_human_review(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = self.fixture(root)
            (root/"qa.json").write_text("{}")
            with self.assertRaises(ValueError): validate_record(record, root, {})
            record = self.fixture(root)
            write_json(root/"annotation.json", {"dataset": {"review_status": "human_reviewed"}})
            with self.assertRaises(ValueError): validate_record(record, root, {})


if __name__ == "__main__":
    unittest.main()
