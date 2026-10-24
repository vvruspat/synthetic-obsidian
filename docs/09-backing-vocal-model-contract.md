# Backing Vocal Model Contract

Synthetic Obsidian generates backing-note tracks either locally with a compact
non-autoregressive MLX Transformer or, when explicitly selected, through the
OpenAI Responses API. Both backends predict one semitone offset for each
lead-vocal note; timing and source-note identity remain deterministic.

## Model

The production model lives at:

`~/.synthetic_obsidian/models/backing_vocals/backing_transformer_v1`

The self-contained macOS build bundles the same directory under the
`backing-notes` runtime pack. Qwen and MLX-LM are not production dependencies.

The experimental cloud choices are `gpt-5.6-luna` and `gpt-5.6-terra`. Luna is
the default cloud option; Local remains available for offline work. On macOS the
API key is read from the login Keychain entry with service
`com.syntheticobsidian.openai` and account `synthetic-obsidian`, or from the
`OPENAI_API_KEY` environment variable. The key is never written to project JSON
or worker logs.

## Supported Styles

| ID | Name |
|---|---|
| `UNISON` | Unison Double |
| `OCT_UP` | Octave Above |
| `OCT_DOWN` | Octave Below |
| `THIRD_UP` | Third Above |
| `THIRD_DOWN` | Third Below |
| `SIXTH_UP` | Sixth Above |
| `SIXTH_DOWN` | Sixth Below |
| `FIFTH_UP` | Fifth Above |
| `FIFTH_DOWN` | Fifth Below |
| `FOURTH_UP` | Fourth Above |
| `FOURTH_DOWN` | Fourth Below |
| `DRONE_ROOT` | Drone Root |
| `DRONE_FIFTH` | Drone Fifth |
| `DRONE_THIRD` | Drone Third |
| `CONTRARY` | Contrary Motion Harmony |
| `OBLIQUE` | Oblique Motion Harmony |
| `PAR3` | Parallel Thirds |
| `PAR6` | Parallel Sixths |
| `CHOIR_SOP` | Choir Soprano |
| `CHOIR_ALT` | Choir Alto |
| `CHOIR_TEN` | Choir Tenor |
| `CHOIR_BASS` | Choir Bass |
| `SUSP` | Suspension Harmony |
| `PASSING` | Passing Tone Harmony |
| `TENSION_RES` | Tension-Resolution Harmony |
| `DYNAMIC_CP` | Dynamic Counterpoint |

## Input

The worker receives a JSON request containing:

- `style_id`
- optional `generation_backend` and `generation_model`
- `bpm`, `meter`, and `key`
- `duration`
- `tempo_segments`
- `chords`
- `lead_notes`

Each lead note must include a stable `id`, start and duration in seconds, MIDI
pitch, velocity, phrase identity, and optional lyric metadata.

## Output

The worker returns JSON containing:

- `engine: compact_transformer`
- requested style ID and name
- generated window and note counts
- generated notes

Each generated note retains `source_lead_ref`, lyric metadata, legato flags, and
deterministic timing from its lead note. The cloud model cannot add, remove, or
move notes: strict structured output accepts only one pitch offset per supplied
lead-note reference. Pitch output is harmonically post-processed against the
active chord and key before insertion into the project.

The native result handoff uses `source_lead_ref` to inherit the source pitch
curve from an immutable local snapshot, transposed around the generated note.
Before merging source plateaus, raw detector samples more than three semitones
from their original note centre are replaced by interpolation between reliable
neighbours (nearest reliable value at an edge). This excludes harmonic jumps
and dropouts without smoothing valid drift/vibrato or modifying source analysis.
Explicitly edited source curves bypass this detector cleanup.
After transposition, adjacent backing notes also detect counter-direction
edge excursions beyond the generated interval plus a one-semitone (100-cent)
margin. An offending excursion is reduced proportionally, preserving its shape
instead of clipping individual samples to a flat ceiling. The first/last 80 ms
are guarded, with a smooth recovery to the original contour
within 220 ms. Touching notes with a pitch discontinuity always share a smooth
transition on both sides, including boundaries without an excessive excursion.
Already continuous boundaries and sustained vibrato remain unchanged. Windows
shrink for short notes. Pauses over 80 ms, overlapping voices and separate
clips are not connected. This idempotent offline repair runs at generation and
before audio rendering, so saved backing parts can be repaired by rendering
again, with pitch undo available and the displayed/rendered curves in agreement.
The curve is displayed and saved on the backing note; it is not sent to the
note-generation model. Notes with inherited or manually scaled contours carry
the additive `explicit_pitch_curve` flag. Audio requests map this to the optional
`preserve_pitch_curve` field so the SVC renderer does not overwrite the curve
with automatic vibrato, attacks or legato. Unmatched source references retain
the existing flat-curve fallback and automatic expression.

## Threading

Model loading and inference happen in the persistent `backing-notes` background
worker. MLX inference and OpenAI network requests never run on the audio or JUCE
message thread. Cloud requests contain musical context (style, key, meter,
chords, canonical lead pitches and syllable labels), not source audio.
