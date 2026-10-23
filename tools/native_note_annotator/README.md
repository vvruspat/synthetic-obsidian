# Native Vocal Note Annotator

A separate, offline **JUCE 8 / C++20** application for making manually checked
vocal-note datasets. All visible controls and the waveform are JUCE components;
this target does not link the existing React frontend or compile a WebView.
The main plugin and the larger `VocalAnnotationTool` are unchanged.

## Build and launch

From the repository root:

```sh
cmake -S . -B build-native-note -DCMAKE_BUILD_TYPE=Release \
  -DSYNTHETIC_OBSIDIAN_BUILD_TOOLS=OFF \
  -DSYNTHETIC_OBSIDIAN_BUILD_NATIVE_ANNOTATOR=ON \
  -DSYNTHETIC_OBSIDIAN_ANNOTATOR_TESTS=ON
cmake --build build-native-note --config Release \
  --target NativeNoteAnnotator NativeNoteAnnotatorTests --parallel 8
ctest --test-dir build-native-note/tools/native_note_annotator \
  -C Release --output-on-failure
```

macOS output:
`build-native-note/tools/native_note_annotator/NativeNoteAnnotator_artefacts/Release/Vocal Note Annotator.app`.
The native target builds for **arm64 and x86_64**; it has no MLX/Python runtime.
Windows uses the same targets and produces `Vocal Note Annotator.exe` in the
Release artefacts folder. Distribution signing/notarization is separate.

## AI note drafts

The annotator can now run a **70,403-parameter residual TCN directly in C++**.
The first `Sasha notes v1` model was adapted from the existing GTSinger vocal
encoder using the user's 11 annotated exports (510.85 seconds, 641 notes).
It predicts acoustic onsets, musical offsets and note activity. Pitch still
comes from the existing YIN analysis; pitch curves were not treated as corrected
pitch supervision.

The runner also accepts the bounded `synthetic-obsidian-note-boundary-tcn-v2`
contract: 32/128-ms mel windows, three noise/energy cues, and six residual blocks
with 950-ms temporal context. Legacy models remain supported. Both contracts
keep the three onset/offset/activity outputs and run only on the existing
background worker. A new feature schema does not change annotation files,
plugin parameters or the audio callback. See `research/native_notes/README.md`
for the experiment and validation results.

AI drafts retain the model's decoded intervals even when YIN cannot estimate
their pitch. Such intervals carry `pitch_uncertain` and display `?`; missing
pitch evidence does not delete a note from the display, saved proposal or dataset
export. Pitch/YIN inputs to the model are unchanged. Sibilant rejection is left
to a separate future stage. Completely silent audio remains empty. Existing
saved labels are preserved; regenerate a draft to apply the changed policy.

The analysis row provides **AI note boundaries / Math (YIN + energy)**,
an installed-model selector, **Model...** and **Generate draft**. The selector
lists `model.json` files under `~/.synthetic_obsidian/models/note-boundaries/`
in natural order, including all installed versions, the bundled fallback and
the current custom model. **Model...** can select another JSON file. Choosing
a model remembers its path and enables AI mode; press **Generate draft** to
compare its output on the current vocal. Selecting alone keeps the open notes.
The local v1 model is installed under
`~/.synthetic_obsidian/models/note-boundaries/sasha-v1/model.json`. A packaged
model is a fallback when this file is absent. Selection is remembered, but the
weights are only loaded by the background worker on first analysis.

Opening an unannotated vocal uses the selected method automatically. Opening
existing annotations (including the exported WAV's `.annotation.json` sidecar)
preserves the saved notes. **Generate draft** produces a replacement proposal
in a separate `.proposal.annotation.json`, keeping the original labels intact.
Undo/Redo restores both notes and their generating-model metadata. Subsequent
manual editing uses the same waveform controls as before. Review the entire
clip, mark it checked, and export a new dataset revision.

Missing/incompatible models show an explanation and fall back to mathematical
analysis. Inference, feature extraction and weight loading stay off the audio
and message threads. No Python runtime is needed to use this model.

Build with a model included:

```sh
cmake -S . -B build-native-note \
  -DSYNTHETIC_OBSIDIAN_NOTE_MODEL="$HOME/.synthetic_obsidian/models/note-boundaries/sasha-v1/model.json"
cmake --build build-native-note --config Release --target NativeNoteAnnotator
```

macOS places the JSON in `Contents/Resources/note-model`; Windows places it
beside the executable under `models/note-boundaries`. Other models can be
selected without rebuilding. See [the training workflow](../../research/native_notes/README.md)
for training/retraining and the frozen song split.

## Annotating

1. Open or drop a mono/stereo WAV, AIFF or FLAC. Saved annotation JSON can also
   be opened. The entire clip is decoded and analyzed in a worker; no internet,
   model download or Python installation is required.
2. Initial colored regions are **draft musical notes**, labelled with MIDI note
   names. In Math mode, YIN-style F0 estimation at 10 ms intervals, RMS gating, median
   smoothing, sustained pitch changes and energy valleys propose the notes.
   The stronger stereo channel is analyzed to avoid phase cancellation.
3. Note start means the **beginning of the acoustic attack**, including up to
   120 ms of audible onset before stable F0. It is not the waveform peak.
   `voiced_start`/`voiced_end` separately describe the pitched portion. Slides
   and normal vibrato should remain in a note's pitch curve; repeated attacks
   can be separate notes even when their MIDI pitches match.
4. Cmd+click (Ctrl+click on Windows) splits an existing note at the click,
   retaining its end, or creates a note in an empty span with the cached
   acoustic end proposal. The next existing note caps the proposal. When no
   voiced evidence exists, the default length is 250 ms and the pitch label
   shows `?`; assign its pitch with the arrow keys or delete the region.
5. Drag either vertical edge/handle to change timing. A shared boundary moves
   both adjoining edges; separated notes cannot be resized into each other.
   Minimum manual note length is 25 ms.
6. Click a note to hear its audio. Click the timeline/empty waveform to seek.
   Space plays/stops the clip. Up/Down changes the selected MIDI pitch;
   Delete removes it; M merges it with the next note. Cmd/Ctrl+Z and
   Shift+Cmd/Ctrl+Z undo/redo, with one action per drag.
7. Use +/- or pinch to zoom. Scroll pans time; Cmd/Ctrl+scroll zooms around the
   pointer. Fit shows the whole clip.
8. Fill in singer and song/session IDs and check **Entire clip checked by hand**
   only after checking all attacks, offsets, pitches, missing and extra notes.
   Any subsequent note edit clears this review flag. Unlabelled spans mean no
   musical note; do not mark an incompletely reviewed clip as checked.

Save writes `<source-stem>.notes.annotation.json` beside the original audio,
using an atomic temporary-file replacement. Changes autosave after 2.5 seconds
of inactivity. Opening another clip or quitting first saves pending changes;
write failures keep the document open. Saving/exporting run in the worker.
Save as (Shift+Cmd/Ctrl+S) chooses another location, including for read-only
source folders. A failed autosave waits for a new edit or a manual save retry.
Reopening checks duration, sample rate and the saved audio checksum.

Drafts are useful proposals, not a quality guarantee on real singing. Low
voices below approximately 70 Hz, falsetto above approximately 1 kHz, breathy
phonation, heavy reverb, accompaniment and fast melismas need extra review.
Keep vocals isolated and preferably dry. The tool accepts 8–192 kHz audio with
at most 512 MiB of decoded float samples. Very long sessions should be split
into manageable clips before annotation.

## Dataset export

Export dataset chooses a destination directory and creates a new bundle:

```text
vocal_<first-24-hex-digits-of-source-SHA256>/
  vocal_<id>.wav
  vocal_<id>.annotation.json
  vocal_<id>.notes.csv
  vocal_<id>.notes.txt
```

The WAV is PCM 24-bit, preserving sample rate, channel count, sample count and
the full timeline. There is no trimming, resampling or loudness normalization.
Float recordings exceeding full scale are rejected rather than silently clipped;
lower their source level before export.
The JSON refers to the sibling WAV by a relative path, so the folder is portable.
The complete bundle is published by renaming a staging directory; failures do
not leave a partially published sample. Repeated exports get a new folder
instead of overwriting earlier labels. Deduplicate them by `source_sha256` when
assembling training data; the newest reviewed revision can supersede older ones.

The JSON retains the existing version-1 note schema (IDs, seconds, integer MIDI
pitch, exact pitch, voiced interval, full pitch curve and flags), and adds
`dataset` metadata: schema, source and exported-audio SHA256, singer/session IDs,
`draft` or `human_reviewed` status, units and annotation policy. Only explicitly
checked clips should be treated as ground truth. The source hash stays stable
across WAV re-encoding. The audio hash checks the file paired with this revision.

CSV columns:
`onset_seconds,offset_seconds,pitch_midi,pitch_hz,onset_sample,offset_sample`.
Sample indices are rounded positions on the original timeline; offsets are
exclusive. The TXT has no header and three tab-separated columns: onset seconds,
offset seconds, pitch Hertz. This matches
[`mir_eval.io.load_valued_intervals`](https://mir-eval.readthedocs.io/latest/api/io.html#mir_eval.io.load_valued_intervals)
and the [transcription evaluator](https://mir-eval.readthedocs.io/latest/api/transcription.html).
The Hz column uses the corrected musical MIDI pitch, not each vibrato frame.
The rich JSON is our documented schema, not a claim of a universal JSON standard.

## How much material to prepare

These are planning estimates for a small model, not measured learning curves:

| Stage | Checked audio | 15-second clips | Expected purpose |
| --- | ---: | ---: | --- |
| Pipeline check | 30–60 minutes | 120–240 | Confirm labels, export and evaluation work |
| First useful experiment | 2–5 hours | 480–1,200 | Fine-tune/prototype a compact note detector |
| Broader singer/style coverage | 10–20 hours | 2,400–4,800 | Improve generalization and difficult attacks |
| More demanding coverage | 20–50 hours | 4,800–12,000 | Diverse unseen singers and recording conditions |

Prioritize 10–20 singers, different registers, repeated equal-pitch notes,
legato, consonant attacks, quiet/breathy singing, pauses, slides, vibrato and
melismas over many nearly identical examples. Keep 10–20% of the material for
validation/test, grouped by **singer and song/session before cropping or
augmentation**. A random split of neighboring snippets leaks the same recording.
If only one singer is available, split by independent songs/sessions and report
that the result measures adaptation to that singer rather than new-singer quality.

Budget approximately **8–20 working hours per hour of checked audio** at first:
2–5 hours of audio means roughly 16–100 hours of annotation. This range is a
working assumption: time a 10-minute pilot and replace it with your actual
review/edit speed. Review quality and consistency matter more than sheer hours.

A practical compact model is a residual TCN or small CNN over log-mel/F0/energy
features, with onset, offset, voiced and pitch outputs (roughly 0.2–1 million
parameters as a design budget). Use the existing research models as baselines
where their contracts match. No model is trained by this application.

Public pretraining can reduce how much new material must be annotated:
[GTSinger](https://arxiv.org/abs/2409.13832) reports 80.59 hours and 20 singers.
Its score/phoneme labels still need conversion and checks against our acoustic
attack policy; they are not automatically our gold musical-note boundaries.
[Basic Pitch](https://github.com/spotify/basic-pitch) is another compact
transcription baseline, not a guarantee of accurate vocal attack/offset labels.
Measure note F1 at 25/50 ms onset tolerance, offset accuracy, pitch accuracy and
over-/under-segmentation on held-out recordings before deciding to gather more.

## Safety and validation

The device callback reads an immutable decoded buffer and atomic transport
controls. Buffer replacement occurs with the callback detached. Linear playback
resampling and short fades use no heap allocation, locks, file access or inference.
Analysis, loading, JSON and WAV operations are offline worker work. Threads finish
before the component is destroyed. Plugin parameters, state, ONNX and Python APIs
are unchanged.

The optional console checks cover 44.1/48/96 kHz, stereo phase inversion, silence,
pitch transitions, vibrato, pre-F0 attacks, cancellation, insertion, shared-edge
editing, non-overlap, finite-value validation, audio/JSON round-trip, portable
paths, exact WAV sample count, standard interval columns and repeated export.
The native window should additionally be exercised with real vocal clips.
The dedicated CI workflow builds and checks this target on macOS and Windows.
