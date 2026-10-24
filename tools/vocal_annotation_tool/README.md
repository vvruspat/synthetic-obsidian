# Vocal Annotation Tool

For the separate **native JUCE waveform annotation application** (without a
WebView), build `NativeNoteAnnotator`. See
[`../native_note_annotator/README.md`](../native_note_annotator/README.md).

Standalone offline annotator for creating gold vocal note/syllable datasets.

See [native control parity and validation](UI_PARITY.md) for the migrated control inventory.

## Tracks and clips

Use **+** above the track list to add an audio track. **File Import** (also in
the Project menu) or dropping audio files into the window inserts clips at the
playhead on the single selected track. With no single track selected, import
creates and selects a new track. Shift-click toggles track selection, including
clearing the last selected track. Multiple files are placed consecutively from
that playhead position, without moving existing clips. Drag
a clip horizontally to change its time or vertically to another track; its notes
and pitch curves move with it. A focused clip also supports Left/Right (0.1 s),
Shift+Left/Right (1 s), and Delete. The clip's trash button removes just that clip.
Project Undo/Redo restores arrangement edits.

Each source has a separate backing group row with its own **Add Back** menu.
Generated backings are indented below that group and share their source color;
repeated generation adds more children. The group disclosure arrow hides or shows its children
without changing playback. Removing a parent removes its descendants too, with
confirmation and undo. There is no fixed track count limit.

Projects store an additive `arrangement_tracks` field with clip source, start,
offset and duration, track parent, mute/solo and collapsed state. Older projects
are migrated to one clip per existing audio track. Consolidated saves copy all
clip media, and stem export includes clip positions and silent gaps. Older app
versions do not understand the new arrangement field; use this version to edit
and save projects containing multiple clips.

Dropped native files are passed directly to the existing background import worker; no browser transfer or temporary base64 upload is needed.

Playback plans and source buffers are prepared on a background worker. The audio
callback only reads prepared buffers and returns retired plans for worker-side
destruction. Unique source files are decoded into RAM, so practical project size
depends on available memory and CPU. Python/RMVPE/SoulX processing stays offline;
model schemas and worker APIs are unchanged. Analyze enriches the selected track's
existing note boundaries; AI Parts rebuilds that track's parts per source clip.

Configure `-DSYNTHETIC_OBSIDIAN_ARRANGEMENT_TESTS=ON`, build `ArrangementTests`,
and run CTest in `vocal_annotation_tool_build` for placement, hierarchy, legacy
migration, save/load, export, and playback checks at 44.1/48/96 kHz with 64–1024
sample blocks. The arrangement CI workflow builds the app and runs these checks
on macOS and Windows, including native command binding and settings checks.

The tool is intentionally isolated from the realtime plugin target. Audio loading,
JSON import/export, pYIN subprocess analysis, and manual editing all live under
`tools/vocal_annotation_tool`.

Its visible interface is now `NativeStudioView`, built from `Obsidian::UIKit` and
`Obsidian::EditorUI`. `NativeStudioSettings` presents audio devices, voice profiles,
training jobs and backing generation. JUCE owns both presentation and document
state. The existing command/event contract is delivered as in-process values;
no browser, embedded frontend bundle or WebView2 SDK is loaded. The annotation
component remains an internal analysis/selection helper.

All note IDs, source clip IDs and track IDs stay attached to real document data.
Native surfaces integrate tempo changes into one shared quarter-note viewport;
signature changes determine bar/beat lines and the transport display. Waveforms
come from the decoded playback worker cache, are cropped by source offset and
prepared outside paint. Transport and meters display backend state.

Project actions are in **Project**; per-track import, audio/note layers and backing
actions are in each track menu. **Audio and voice settings** contains device
selection/test output, source recordings, voice profiles and persistent training
jobs. Record is disabled because the current editor has no recording command.
Only Instrumental has independent track gain in the current playback model;
other gain sliders are disabled. Individual note gain is available in **Note actions**.

The piano toolbar has independent **Click** and **Command-click** tool menus
(**Ctrl-click** on Windows/Linux). Both contain Pointer, Pencil, Eraser, Scissors,
Join, Flex, Vibrato, Gain and Zoom; the defaults are Pointer and Scissors.
Right-click also uses the secondary tool. The tool is chosen at gesture start.

Pointer moves selected notes vertically with continuous fractional pitch and
live audition, while keeping their timing. Shift-click toggles selection and
Cmd/Ctrl+A selects all. Pencil draws a note in either horizontal direction;
a click creates a half-second note. Flex stretches the boundary nearest the
clicked half of a note. Eraser deletes, Scissors splits, and Join merges the
selected group or an adjacent pair. Gain and Vibrato preview during vertical
drag and commit once on release. Zoom doubles/halves time scale at the click.
The existing note menu and pitch history remain available.

Vertical zoom spans 1×–4× and preserves the visible pitch centre. The keyboard
and notes share one vertical scroller. Alt+wheel changes vertical zoom,
Cmd/Ctrl+wheel changes horizontal zoom, Shift+wheel pans time and plain wheel
scrolls pitches. The collapse button leaves the toolbar visible, expands the
arrangement, and restores pitch zoom, scroll and selection when reopened.

Escape, losing focus, hiding/disabling the editor and incoming project data
cancel uncommitted gestures and end audition. Moving a displayed vocal fragment
applies deltas to its full source note, preserving unvoiced regions and exact
pitch. A group gesture creates one undo action.

Run `Synthetic Obsidian --project /absolute/path/to/project-folder` to open an
existing project directly. For checks configure
`-DSYNTHETIC_OBSIDIAN_ARRANGEMENT_TESTS=ON`, build `ArrangementTests`,
`NativeStudioTests` and `ObsidianUIKitTests`, then run CTest. The native studio
suite exercises real command/event binding, duplicate display names, silent
snapshots, async command feedback, multi-note moves and settings lifecycles.

The bundled offline analyzer currently infers one global 3/4 or 4/4 signature
from detected beat accents and falls back to low-confidence 4/4 when evidence is
weak. Imported annotation JSON may contain arbitrary signature changes and
denominators; those are preserved and rendered without flattening.

Instrumental harmony analysis retains the detected beat phase, evaluates chord
states on half-beat cells, smooths them as a sequence, and then refines each
state change against frame-level harmonic novelty. CQT/CENS chroma, low-register
chroma, tuning compensation, and a weak detected-key prior are combined so mix
residue is less likely to be mistaken for a seventh or suspension. Every chord
segment includes confidence and alternative labels; low-confidence context is
not treated as an absolute pitch-correction constraint.

## Build

`AI Parts` now uses the native **Sasha v11** note-boundary TCN instead of the
GTSinger syllable/breath detector. It proposes note intervals and pauses, keeping
intervals whose pitch is uncertain. There is no YIN sibilant-rejection postfilter.
Model inference, decoding, and file loading run on the offline worker; weights
are loaded lazily and reused between requests. The exact same C++ implementation
and v11 weights are used by `NativeNoteAnnotator`.

After v11, the separate native **Sasha Sibilants v1** classifier attaches a note
to the nearest non-sibilant note when **more than 50% of its duration** is classified as sibilant
at the classifier's balanced 0.48 threshold. Partial edge frames are weighted
by their overlap with the note. Distance is the gap between intervals; equal gaps
prefer the previous note. Gaps over 120 ms and clips with no vocal neighbour
remain unchanged. Whole intervals are retained, including their attacks/tails;
notes with 50% or less sibilant time keep their boundaries. Marker generation and subsequent
RMVPE enrichment use the merged intervals. Attached notes carry
`sibilant_attached` / `sibilants:<model-id>` flags and the status reports the count.
The classifier is loaded lazily in the same background worker and needs no
Python runtime. Missing/invalid weights keep raw v11 intervals and show a warning.

A positive voiced-core guard vetoes attachment when the original note contains
a continuous non-sibilant span with native YIN confidence >= 0.8. The required
span is 25% of note duration, clamped to 30–80 ms so short notes can be protected.
Pitch may change during that span; vibrato and legato do not require a constant
pitch. Sparse or uncertain pitch alone does not decide attachment. The guard
uses the already computed offline analysis, preserving valid notes with long
consonants while allowing standalone sibilant fragments to attach.

The temporary white diagnostic overlay and its separate inference worker are
removed from the release UI. Sibilant classification and the voiced-core guard
remain part of AI Parts. Diagnostic frame masks are still available through
the native checks executable's `--inspect-sibilants` mode.

The classifier uses the separate `synthetic-obsidian-sibilant-native-v1` JSON
contract exported by `research.sibilants.export_native`, including mel filters,
frozen normalization and transposed convolution arrays. It is discovered at
`Models/sibilants/sasha-v1-experimental/native-model.json` (Application Support,
then legacy), or the bundled `Resources/sibilant-model/native-model.json`.
Bundle with `SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL`; the DMG builder includes it.
Project serialization and parameter IDs are unchanged; all classification,
resampling and allocations remain outside the audio callback.

`Analyze` obtains RMVPE pitch curves for the current note blocks. It preserves
their IDs and boundaries, including manual splits, joins, deletions, and moved
edges. Annotation markers do not reconstruct or resegment notes during pitch
analysis. When no AI Parts exist it first runs v11. If the pitch
worker fails, the parts and available native YIN pitch remain. Missing or invalid
Sasha weights leave the existing document unchanged with a status message.

The model is discovered in Application Support `Models/note-boundaries/sasha-v11-melismas/`,
then the legacy `~/.synthetic_obsidian/models/note-boundaries/sasha-v11-melismas/`,
then the app's bundled `Resources/note-model/model.json`. For a portable build,
pass `-DSYNTHETIC_OBSIDIAN_APP_NOTE_MODEL=/path/to/model.json` to CMake. The DMG
builder includes that resource, with an optional environment variable of the
same name. No model weights are checked into Git.

Optional integration checks: configure `-DSYNTHETIC_OBSIDIAN_VOCAL_ANALYSIS_TESTS=ON`,
build `SashaNoteAnalysisTests`, and run CTest in the tool's build directory.
They cover pauses, connected notes, missing pitch, piano-roll edits, and stale
results: tempo/chord updates do not cancel vocal analysis, while edits to notes,
parts, or backing tracks do. The annotation format and audio callback are unchanged.

```bash
cmake -S /Users/aleksandrkolesov/synthetic-obsidian \
  -B /Users/aleksandrkolesov/synthetic-obsidian/build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /Users/aleksandrkolesov/synthetic-obsidian/build --target VocalAnnotationTool
```

## Validation

```bash
python3 /Users/aleksandrkolesov/synthetic-obsidian/tools/vocal_annotation_tool/validate_annotation.py \
  /path/to/sample.annotation.json
```

## SoulX-Singer runtime

`Render BV` expects an isolated Python 3.10 runtime and the official SVC
checkpoint outside the repository. SVC mode preserves the source vocal's words,
timing, consonants, breath, and timbre while replacing its F0 with the generated
backing-vocal melody.

```bash
runtime="$HOME/.synthetic_obsidian/runtime/soulx-singer"
models="$HOME/.synthetic_obsidian/models/SoulX-Singer"

git clone https://github.com/Soul-AILab/SoulX-Singer.git "$runtime/source"
git -C "$runtime/source" checkout 81aeb3ae772c70093c3de74dc23c92d983801ae4
uv venv --python 3.10 "$runtime/.venv"
uv pip install --python "$runtime/.venv/bin/python" \
  'numpy<2' torch==2.2.0 torchaudio==2.2.0 transformers==4.41.2 \
  accelerate==1.11.0 beartype==0.22.9 einops==0.8.2 librosa==0.11.0 \
  omegaconf==2.3.0 scipy==1.15.3 soundfile==0.13.1 tqdm==4.67.1 \
  pyworld==0.3.5 'setuptools<81'
"$runtime/.venv/bin/hf" download Soul-AILab/SoulX-Singer model-svc.pt \
  --local-dir "$models"
```

On Apple Silicon, the content encoder and flow model run on MPS. Vocos alone
runs on CPU because its complex ISTFT operation is not supported by MPS. The
bridge is outside the real-time playback path and only transfers the generated
mel/audio at each offline render segment.

## Notes

- Opening audio creates a first-pass offline draft from a smoothed short-window
  energy envelope. It detects editable syllable parts, likely rearticulations,
  and breath/noise boundary candidates. It is deliberately a review aid, not a
  gold pitch detector.
- `Analyze` runs `research/svc_pitch/analyze_notes_pyin.py` with the RMVPE backend
  in a background subprocess, enriching the editable AI Parts with pitch curves.
- `AI Parts` runs Sasha v11 in C++ and creates note starts and pause boundaries.
  Existing lyric text is preserved; the model does not classify breath or sibilants.
- Waveform syllable spans are highlighted with rotating colors between boundary
  markers, with note starts used as a fallback when no boundary markers exist.
- Trackpad gestures: two-finger pan scrolls time horizontally and pitch
  vertically, pinch zooms time around the cursor, and Option+pinch zooms pitch.
- Modifier scroll: Cmd/Ctrl+scroll zooms time, Option+scroll zooms pitch, and
  Shift+vertical scroll pans horizontally.
- `Undo` / `Redo` are snapshot based. Mouse drags are grouped as one undoable
  action.
- Dragging a note body changes pitch by default; Shift+drag moves the note in
  time.
- Dirty documents autosave beside the target JSON as
  `<audio-name>.annotation.autosave.json`.
- Space toggles playback when the editor has focus.
- Clicking a note auditions its pitch as a generated tone. Clicking the waveform
  body loops that detected audio part; use the playback mode menu to hear
  `Notes Only` or `Notes + Sound`. Generated note playback follows the stored
  pitch curve, so legato slides/drift are audible instead of flat MIDI steps.
- MIDI export writes pitch-bend range setup and pitch-wheel events from note
  curves. This represents vocal slides in standard monophonic MIDI playback.
- `Render BV` uses SoulX-Singer-SVC in a persistent offline worker. The main
  vocal supplies words, consonants, breaths, timing, and timbre; generated
  backing notes supply the replacement F0 contour. The worker is started with
  the application and reused for later renders; no model code runs on the audio
  thread. Runtime files and the checkpoint live outside the repository under
  `~/.synthetic_obsidian/`.
- Completed SoulX LoRA profiles include a small singer-expression profile next
  to the adapter. It captures robust vibrato and pitch-transition statistics
  from the training vocals. These profiles remain available to the legacy SVC
  renderer. The SVC F0 builder uses those statistics for vibrato and legato
  while ignoring tiny resampling jitter that is not real expression.
- `Lyrics` imports a text file and runs an offline torchaudio wav2vec2 forced
  alignment subprocess against the currently loaded audio. The aligned word
  timings are split into rough syllables, placed back onto the waveform, and used
  to add/correct syllable and rearticulation splits. For longer lyrics, line
  breaks are treated as phrase chunks; if CTC alignment drifts or collapses on
  sung vocals, the tool falls back to monotonic energy-based phrase placement so
  later phrases are not dropped or folded into earlier audio. Word starts that land
  inside an already-detected vocal note are snapped back to that note onset so
  the beginning of the waveform is not left as a noise/unlabeled segment.
  A second acoustic refinement pass snaps syllable starts to nearby waveform
  onsets/valleys and adds continuation rearticulation markers inside long,
  visibly multi-attack spans. Continuations without a strong new attack are
  marked as `legato` so pitch drift does not force a new note split. Lyric
  syllables with a clear attack stay `syllable`; `rearticulation` is reserved
  for extra detected attacks that do not map to a lyric syllable.
  Multi-syllable words use leading/trailing dashes such as `Moon-` and `-light`.
- Candidate syllables/slides are not treated as ground truth by this first pass.
- JSON saves beside the audio by default as `<audio-name>.annotation.json`.
