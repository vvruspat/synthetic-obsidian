# Synthetic Obsidian

Synthetic Obsidian is a JUCE 8 / CMake audio project for vocal processing and vocal dataset tooling.

The repository currently contains two connected pieces of work:

- a JUCE VST3/Standalone plugin scaffold for vocal processing experiments;
- a standalone Vocal Annotation Tool with a React/TypeScript interface and a
  C++/Python backend for building and validating note, syllable, breath, pause,
  and pitch-curve annotations.

The most mature part right now is the annotation tool. It includes an offline ML-assisted boundary detector and a small breath-detection checkpoint trained from paired `vox` / `breath` stems.

## Current status

- C++20 project built with CMake.
- JUCE 8 is fetched automatically through CMake `FetchContent`.
- Plugin targets: VST3 and Standalone.
- Tool target: `VocalAnnotationTool`.
- ML inference runs offline through Python subprocesses; it is not part of the realtime audio thread.
- Large datasets, training runs, build outputs, and experiment checkpoints are intentionally excluded from Git.

## Repository layout

```text
Source/                       JUCE plugin source
  ai/                         Python / model bridge experiments
  analysis/                   Offline audio analysis helpers
  dsp/                        DSP modules
  state/                      Project/session state models
  ui/                         Plugin UI components
  web/                        JUCE WebView host for the embedded React UI

frontend/                     React/TypeScript application UI
tools/vocal_annotation_tool/  Standalone annotation app
research/gtsinger_alignment/  Boundary model training/inference utilities
docs/                         Product, design, and architecture notes
data/gtsinger_alignment/      Lightweight runtime checkpoint only
```

## Build the Vocal Annotation Tool

The checked-in `frontend/dist` bundle is embedded into the native app, so a
normal C++ build does not require Node.js and the app does not start a local web
server. When the frontend source changes, refresh the bundle first:

```bash
cd frontend
npm ci
npm run check
cd ..
```

```bash
cmake -S . -B build-vocal-tool2 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-vocal-tool2 --target VocalAnnotationTool -j 4
```

On macOS, the app is generated at:

```text
build-vocal-tool2/vocal_annotation_tool_build/VocalAnnotationTool_artefacts/Debug/Vocal Annotation Tool.app
```

Run it with:

```bash
open "build-vocal-tool2/vocal_annotation_tool_build/VocalAnnotationTool_artefacts/Debug/Vocal Annotation Tool.app"
```

## Build the plugin

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target SyntheticObsidian -j 4
```

For more platform-specific build notes, see [BUILD.md](BUILD.md).

## Vocal Annotation Tool

The tool is designed for offline vocal annotation and review. It can:

- run its entire visible UI in an embedded React WebView;
- load audio and annotation JSON;
- create and edit note blocks, syllable splits, breaths, pauses, noise regions, and legato/rearticulation boundaries;
- draw pitch curves and export MIDI with pitch-bend events;
- run pYIN analysis in a background subprocess;
- run the GTSinger-derived boundary model in a background subprocess;
- detect breath intervals and draw breath waveform regions in white;
- save annotations beside audio as `<audio-name>.annotation.json`.

The current ML boundary checkpoint used by the tool is:

```text
data/gtsinger_alignment/multilingual_mps_full_breath_diffcut_encoder_thr075_tcn.pt
```

That checkpoint is small enough to keep in Git. The full training dataset and intermediate experiment checkpoints are not committed.

More tool details live in [tools/vocal_annotation_tool/README.md](tools/vocal_annotation_tool/README.md).

## Breath model notes

The breath detector was trained from paired vocal and breath stems. The current dataset builder derives breath targets by comparing the two stems:

- breath starts are taken from non-silent activity in the isolated `breath` stem;
- breath ends are trimmed when `vox - breath` diverges, which usually means the signal has transitioned from breath into sung vocal;
- this avoids teaching the model to include right-padding or syllable attacks as breath.

Relevant scripts:

```text
research/gtsinger_alignment/build_breath_pair_manifest.py
research/gtsinger_alignment/train_baseline.py
research/gtsinger_alignment/evaluate_breath_pairs.py
tools/vocal_annotation_tool/infer_gtsinger_boundaries.py
```

## Validation

Python unit tests:

```bash
PYTHONPATH=research/gtsinger_alignment:tools/vocal_annotation_tool \
  research/.venv_seed_vc/bin/python -m unittest \
  tools/vocal_annotation_tool/test_infer_gtsinger_boundaries.py \
  research/gtsinger_alignment/test_pipeline.py
```

Annotation JSON validation:

```bash
python3 tools/vocal_annotation_tool/validate_annotation.py /path/to/sample.annotation.json
```

## What is intentionally not in Git

The `.gitignore` excludes local-heavy artifacts such as:

- CMake build directories;
- Python virtual environments and caches;
- raw datasets and downloaded corpora;
- generated audio/inference outputs;
- large experiment checkpoints and training logs;
- nested third-party checkouts.

If a future model checkpoint is required at runtime and is small enough for Git, explicitly allowlist that file in `.gitignore` the same way the current breath checkpoint is allowlisted.

## Realtime safety note

The plugin and tool share some research code, but the ML-heavy workflows are offline/background tasks. ONNX/Python/model inference must not run from realtime audio processing code.

The JS/native bridge is message-thread-only. It updates atomics used by
playback, while analysis, backing-vocal generation, rendering, and Python model
calls remain on their existing background workers.
