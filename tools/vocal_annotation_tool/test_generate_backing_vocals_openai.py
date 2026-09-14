#!/usr/bin/env python3

from __future__ import annotations

import json
import unittest
from unittest.mock import patch

from backing_vocal_music import harmonic_postprocess, style_by_id
from generate_backing_vocals_openai import predict_window


class OpenAiBackingVocalTest(unittest.TestCase):
    def test_model_only_controls_pitch_offsets(self) -> None:
        lead = [
            {
                "id": "lead_0001",
                "start": 0.0,
                "duration": 1.0,
                "bar": 1,
                "beat": 1.0,
                "phrase_id": "phrase_001",
                "pitch": "C4",
                "midi_note": 60,
                "syllable": "la",
                "syllable_id": "syllable_001",
                "legato_from_previous": False,
                "legato_to_next": True,
                "melisma_continuation": False,
            },
            {
                "id": "lead_0002",
                "start": 1.0,
                "duration": 2.0,
                "bar": 1,
                "beat": 2.0,
                "phrase_id": "phrase_001",
                "pitch": "D4",
                "midi_note": 62,
                "syllable": "la",
                "syllable_id": "syllable_001",
                "legato_from_previous": True,
                "legato_to_next": False,
                "melisma_continuation": True,
            },
        ]
        chords = [
            {
                "start": 0.0,
                "duration": 4.0,
                "chord": "C",
                "notes": ["C4", "E4", "G4"],
            }
        ]
        response = {
            "output_text": json.dumps(
                {
                    "notes": [
                        {"source_lead_ref": "lead_0001", "semitone_offset": 4},
                        {"source_lead_ref": "lead_0002", "semitone_offset": 3},
                    ]
                }
            )
        }

        with patch("generate_backing_vocals_openai._post_json", return_value=response) as post:
            notes = predict_window(
                {"key": "C major", "time_signatures": [{"numerator": 4, "denominator": 4}]},
                style_by_id("THIRD_UP"),
                lead,
                chords,
                "gpt-5.6-luna",
                "test-key",
            )

        self.assertEqual([note["midi_note"] for note in notes], [64, 65])
        self.assertEqual([note["start"] for note in notes], [0.0, 1.0])
        self.assertEqual([note["duration"] for note in notes], [1.0, 2.0])
        self.assertEqual([note["source_lead_ref"] for note in notes], ["lead_0001", "lead_0002"])
        self.assertTrue(notes[0]["legato_to_next"])
        self.assertTrue(notes[1]["legato_from_previous"])

        payload = post.call_args.args[1]
        schema = payload["text"]["format"]["schema"]["properties"]["notes"]
        self.assertEqual(schema["minItems"], 2)
        self.assertEqual(schema["maxItems"], 2)
        self.assertFalse(payload["store"])

    def test_low_confidence_chord_does_not_override_valid_scale_tone(self) -> None:
        notes = [{"start": 1.0, "midi_note": 71, "pitch": "B4"}]
        chords = [
            {
                "start": 0.0,
                "duration": 4.0,
                "chord": "C",
                "notes": ["C4", "E4", "G4"],
                "confidence": 0.3,
            }
        ]

        corrected = harmonic_postprocess(notes, chords, "C major", "THIRD_UP")

        self.assertEqual(corrected[0]["midi_note"], 71)

    def test_note_near_boundary_may_use_next_chord(self) -> None:
        notes = [{"start": 1.9, "midi_note": 71, "pitch": "B4"}]
        chords = [
            {
                "start": 0.0,
                "duration": 2.0,
                "chord": "C",
                "notes": ["C4", "E4", "G4"],
                "confidence": 0.9,
            },
            {
                "start": 2.0,
                "duration": 2.0,
                "chord": "G",
                "notes": ["G4", "B4", "D5"],
                "confidence": 0.9,
            },
        ]

        corrected = harmonic_postprocess(notes, chords, "C major", "THIRD_UP")

        self.assertEqual(corrected[0]["midi_note"], 71)


if __name__ == "__main__":
    unittest.main()
