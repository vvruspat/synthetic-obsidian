# Backing Vocal Model Contract

This document defines the backing vocal style IDs supported by Synthetic Obsidian and the data contract for the local backing-vocal DSL model.

## Supported Backing Vocal Types

| ID | Name | Description |
|---|---|---|
| `UNISON` | Unison Double | Unison double following the lead melody. |
| `OCT_UP` | Octave Above | Octave doubling above the lead. |
| `OCT_DOWN` | Octave Below | Octave doubling below the lead. |
| `THIRD_UP` | Third Above | Chord-aware upper third harmony. |
| `THIRD_DOWN` | Third Below | Chord-aware lower third harmony. |
| `SIXTH_UP` | Sixth Above | Upper sixth harmony with range correction. |
| `SIXTH_DOWN` | Sixth Below | Lower sixth harmony with range correction. |
| `FIFTH_UP` | Fifth Above | Upper fifth harmony, softened toward chord tones when needed. |
| `FIFTH_DOWN` | Fifth Below | Lower fifth harmony, softened toward chord tones when needed. |
| `FOURTH_UP` | Fourth Above | Upper fourth harmony used as a color interval. |
| `FOURTH_DOWN` | Fourth Below | Lower fourth harmony used as a color interval. |
| `DRONE_ROOT` | Drone Root | Phrase-level sustained chord root. |
| `DRONE_FIFTH` | Drone Fifth | Phrase-level sustained chord fifth. |
| `DRONE_THIRD` | Drone Third | Phrase-level sustained chord third. |
| `PEDAL_ROOT` | Pedal Tone Root | Long pedal on the global tonic or chord root. |
| `PEDAL_FIFTH` | Pedal Tone Fifth | Long pedal on the global fifth. |
| `CONTRARY` | Contrary Motion Harmony | Harmony shaped to move against the lead contour. |
| `OBLIQUE` | Oblique Motion Harmony | Harmony that sustains common tones while lead moves. |
| `PAR3` | Parallel Thirds | Mostly parallel thirds with chord-tone correction. |
| `PAR6` | Parallel Sixths | Mostly parallel sixths with chord-tone correction. |
| `CHOIR_SOP` | Choir Soprano | SATB soprano line. |
| `CHOIR_ALT` | Choir Alto | SATB alto line. |
| `CHOIR_TEN` | Choir Tenor | SATB tenor line. |
| `CHOIR_BASS` | Choir Bass | SATB bass line. |
| `HARMONY_2P` | Two-Part Harmony | Natural two-part backing around the lead. |
| `HARMONY_3P` | Three-Part Harmony | Three-part chord-aware backing. |
| `HARMONY_4P` | Four-Part Harmony | SATB-style four-part backing. |
| `STYLE_POP` | Pop Harmony | Modern pop thirds/sixths with restrained tension. |
| `STYLE_FOLK` | Folk Harmony | Open folk voicing with fifths and drones. |
| `STYLE_GOSPEL` | Gospel Harmony | Dense gospel-inspired chord tones and color tones. |
| `STYLE_CLASSICAL` | Classical Choral Harmony | Conservative classical SATB voice leading. |
| `STYLE_BSHOP` | Barbershop Harmony | Close-position barbershop-inspired harmony. |
| `SUSP` | Suspension Harmony | Prepared suspensions resolving into chord tones. |
| `PASSING` | Passing Tone Harmony | Chord-tone harmony with passing motion between anchors. |
| `TENSION_RES` | Tension-Resolution Harmony | Intentional tension resolving to stable chord tones. |
| `DYNAMIC_CP` | Dynamic Counterpoint | Phrase-aware mixture of parallel, contrary, and oblique motion. |

## Model Purpose

The model generates one backing-vocal arrangement for one requested style and one vocal window.

It does not generate chords, lead vocal, drums, bass, piano, orchestration, effects, pitch bend, MIDI CC, or analysis metadata.

## Current Model

Primary local model:

`Qwen/Qwen3-0.6B` with LoRA adapter:

`~/.synthetic_obsidian/models/backing_vocals/qwen3_0_6b_stage2_yaml_windows_full_lora/adapters.safetensors`

Training format:

- Native YAML DSL vocal windows.
- One example contains only the part of the song where lead vocal exists.
- Chords are included around the vocal window with small timing padding.
- Output contains exactly one requested backing-vocal style.

## Input Contract

The model receives a chat prompt with:

1. A system message.
2. A user message containing task metadata and source DSL YAML.

System message:

```text
You generate Synthetic Obsidian backing-vocal YAML DSL. Return only valid YAML containing tracks.backing_vocals. Do not include markdown, analysis, chords, lead_vocal, or explanations.
```

User message shape:

```text
TASK: GENERATE_BACKING_VOCAL
REQUESTED_STYLE_ID: OCT_UP
REQUESTED_STYLE_NAME: Octave Above
SOURCE_FILE: path/or/song/02_OCT_UP.yaml
WINDOW_INDEX: 1
WINDOW_START_BEAT: 15.5
WINDOW_END_BEAT: 21.5

meta:
  title: "Example Song"
  tempo: 120
  meter: 4/4
  key: C major
  ticks_per_beat: 480
  source_resolution: midi_ticks

tracks:
  chords:
    - id: chord_001
      start: 14.0
      duration: 2.0
      bar: 4
      beat: 3.0
      chord: C
      root: C
      quality: major
      bass: C
      notes: [C3, E3, G3]
      confidence: 0.98
  lead_vocal:
    - id: lead_001
      start: 15.5
      duration: 0.5
      bar: 4
      beat: 4.5
      pitch: C4
      midi_note: 60
      velocity: 95
      syllable: null
      phrase_id: phrase_001
      chord_ref: chord_001
```

Required input fields:

- `REQUESTED_STYLE_ID`
- `REQUESTED_STYLE_NAME`
- `meta.tempo`
- `meta.meter`
- `meta.key`, when known
- `tracks.chords`
- `tracks.lead_vocal`

Important input assumptions:

- `lead_vocal` note IDs are stable inside the window.
- Every lead note should have `start`, `duration`, `bar`, `beat`, `pitch`, `midi_note`, `phrase_id`, and `chord_ref`.
- Chords should cover or slightly surround the vocal window.
- The model may receive absolute song beats. For audition-only files, starts may be shifted, but production should keep original song timing.

## Output Contract

The model must return only YAML with this top-level shape:

```yaml
tracks:
  backing_vocals:
    - id: OCT_UP
      name: Octave Above
      description: Octave doubling above the lead.
      confidence: 0.74
      parts:
        - id: oct_up_part_01
          role: soprano
          strategy: oct_up
          range: C4-A5
          notes:
            - id: oct_up_001
              start: 15.5
              duration: 0.5
              bar: 4
              beat: 4.5
              pitch: C5
              midi_note: 72
              velocity: 74
              syllable: null
              phrase_id: phrase_001
              chord_ref: chord_001
              source_lead_ref: lead_001
```

Output rules:

- Return exactly one item in `tracks.backing_vocals`.
- The returned backing-vocal `id` must match `REQUESTED_STYLE_ID`.
- Do not return `meta`, `tracks.chords`, `tracks.lead_vocal`, or `analysis`.
- Do not include markdown fences, comments, prose, or explanations.
- Every part must contain `id`, `role`, `strategy`, `range`, and `notes`.
- Every note must contain:
  - `id`
  - `start`
  - `duration`
  - `bar`
  - `beat`
  - `pitch`
  - `midi_note`
  - `velocity`
  - `syllable`
  - `phrase_id`
  - `chord_ref`
  - `source_lead_ref`

## Timing Rules

For most styles, generated backing notes should align with the referenced lead note:

- `start` should usually match `source_lead_ref.start`.
- `duration` should usually match `source_lead_ref.duration`.
- `bar`, `beat`, `phrase_id`, and `chord_ref` should usually match the referenced lead note.

Exceptions:

- `DRONE_ROOT`, `DRONE_FIFTH`, and `DRONE_THIRD` may sustain chord tones across a phrase or repeat the lead rhythm depending on the arrangement goal.
- `PEDAL_ROOT` and `PEDAL_FIFTH` may use longer sustained notes.
- `SUSP`, `PASSING`, and `TENSION_RES` may use non-chord tones, but they should still resolve musically.

## Postprocessing And Validation

Model output should be validated before insertion into a full song DSL.

Required validation:

- YAML parses successfully.
- Output contains exactly one `tracks.backing_vocals` item.
- The backing-vocal style ID matches the requested style ID.
- Parts and notes are non-empty.
- Every `source_lead_ref` exists in the input `tracks.lead_vocal`.
- `pitch` and `midi_note` agree.
- For non-drone/non-pedal styles, note timing is close to the referenced lead note.

Recommended deterministic repair:

- Treat `pitch` as authoritative when possible.
- Recalculate `midi_note` from `pitch`.
- Normalize note IDs inside each part.
- Reject or regenerate outputs with missing refs, missing notes, invalid YAML, or wrong style ID.

## Full-Song Generation Strategy

The model is trained on vocal windows, not whole songs.

Production should:

1. Split a song into windows where `tracks.lead_vocal` has notes.
2. Include nearby chords around each window.
3. Call the model once per requested style and window.
4. Validate and repair each output.
5. Stitch the generated `parts` and `notes` back into the full song timeline.
6. Renumber IDs deterministically in the final full-song DSL.

This keeps the model context small while preserving native Synthetic Obsidian DSL input and output.
