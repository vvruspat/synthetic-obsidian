# Controlled vowel melismas with OpenUtau / DiffSinger

This pipeline generates MIDI, renders a sustained vowel across legato notes using the real OpenUtau core,
and exports native annotation bundles. It does not invoke or retrain the note-boundary model.

Current dataset:
`/Users/aleksandrkolesov/Music/Models/Sasha-dataset/melismas-openutau-2026-10-23-v1`.
See its README, `summary.json` and `provenance/final-verification.json` for measured results and limits.

## Local prerequisites

- `research/vendor/OpenUtau`, pinned at release `0.1.576`, commit `bc194b6fe4897427a73066390b286e11205fd10f`.
- Local .NET 10 SDK at `research/.dotnet`; the renderer references the unmodified `OpenUtau.Core` project.
  Its assembly name is `OpenUtau.Test`, which the upstream core explicitly exposes its offline internals to.
- Python environment `research/.venv_diffsinger`, requirements in `requirements-synthesis.txt`.
  These are offline tools; none are runtime dependencies of the JUCE application/plugin.
- PJS DiffSinger and Qixuan 2.7.0 banks at `~/.synthetic_obsidian/models/diffsinger/`.
- PC-NSF-HiFiGAN 2025.02 under `~/Library/OpenUtau/Dependencies/` on macOS.
- `NativeNoteAnnotatorTests` built for independent native YIN extraction.

Upstream voicebank terms and source links are preserved with the dataset. The Qixuan portion is prepared
for boundary/activity detection, not a voice synthesis/conversion training set. Do not silently reuse it
for a different model task.

## New run

Use a new folder. The planner refuses to replace an existing frozen plan.

```sh
research/.venv_diffsinger/bin/python -m research.native_notes.prepare_melismas \
  --output /path/to/new-dataset --scores 64 --seed 2431802

research/.dotnet/dotnet build research/native_notes/openutau_render/OpenUtauRender.csproj -c Release

research/.dotnet/dotnet research/native_notes/openutau_render/bin/Release/net10.0/OpenUtau.Test.dll \
  /path/to/singers /path/to/new-dataset/jobs.json

research/.venv_diffsinger/bin/python -m research.native_notes.finish_melismas \
  --dataset /path/to/new-dataset \
  --native-runner build-native-note/tools/native_note_annotator/NativeNoteAnnotatorTests_artefacts/Release/NativeNoteAnnotatorTests
```

The OpenUtau harness resumes by completed recipe ID. Pitch and gender curves are finalized **before**
building render phrases, so the upstream cache hash includes their values. One vowel spans each legato
phrase; repeated attacks use new phrases with actual gaps. Control F0 is supplied explicitly through PITD.
`snap_first` is disabled for the empty local per-note curve so exported USTX can be reopened safely.

Passing a directory instead of jobs.json to the harness checks all its USTX files through the real core loader.
The generation/packaging code was tested on macOS ARM64; Windows execution was not tested.

QA uses finite/sample-grid checks, independent native YIN versus prescribed F0, raw clipping limits,
RMS phrase-edge alignment, and observed pitch-crossing alignment for already prescribed legato events.
It does not invent arbitrary note events from F0 or use boundary-model predictions as labels.
Uncertain events are masked; failed clips remain diagnostic artifacts and are omitted from `manifest.json`.

## Merge for training

```sh
research/.venv_seed_vc/bin/python -m research.native_notes.synthesis_manifest \
  --base /path/to/previous/prepared.json \
  --synthesis /path/to/new-dataset/manifest.json \
  --output /path/to/new-dataset/prepared-with-melismas.json
```

The merger preserves existing source/derivative records, labels, and validation/test roles. New synthetic
clips stay train-only. The prepared loader verifies pinned recipes, MIDI, QA and source-bank identities,
plus the ordinary WAV/annotation hashes and aligned boundary correspondence. Synthetic sampling uses
`--synthetic-weight`; optional `--controlled-synthetic-weight` overrides it for these new controlled
renders only. Without the override, both synthetic sources retain the same weight. The training report
records the effective weights and counts controlled synthesis separately.

Keep the human-corrected first 32 s of `LelianaSong/vox/Vox_1.wav` outside this training set.
It is development control after being used to guide this experiment, not an independent final test.
No training starts as a side effect of any generation, finishing or merging command.

## Fine-tuning and scoped development selection

Train from the merged manifest with the previous checkpoint, `--save-epoch-checkpoints` and `--skip-test`.
For v11 the fixed weights are 1.0 for human recordings, 0.1 for earlier augmented recordings, and 0.5
for controlled melismas; the learning-rate scale is 0.2 and the extra interior-boundary penalty is off
(`--interior-boundary-weight 1`). This is a full-network fine-tune of v10, with the same native v4 inputs.

`select_guarded --cohort-guards --extended-thresholds --partial-review /path/to/spec.json
--native-runner /path/to/NativeNoteAnnotatorTests` can include an explicitly reviewed prefix as an
additional development cohort. The spec pins WAV/manual hashes, sets `reviewed_start` to 0,
`reviewed_end` to the actual review limit and `role` to `validation_only_for_selection_not_training`.
A note crossing that limit is rejected rather than treating its unreviewed end as ground truth.
Feature normalization still uses the full recording, matching application inference; scoring excludes
the unreviewed tail. The control cannot already appear in the prepared manifest. Selection preserves
the incumbent's connected onset/offset matched counts separately in each validation cohort, then ranks
eligible candidates by equal cohort mean F1. Freeze epoch and thresholds before opening the test set.
