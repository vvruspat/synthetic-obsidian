# Synthetic Obsidian

Synthetic Obsidian is a JUCE 8/CMake audio project with an offline vocal annotation tool and experimental ML-assisted vocal boundary detection.

The current repository snapshot includes the Vocal Annotation Tool and a lightweight breath-detection checkpoint used by the tool. Large local datasets, build products, and training artifacts are intentionally excluded from Git.

## Build

```bash
cmake -S . -B build-vocal-tool2
cmake --build build-vocal-tool2 --target VocalAnnotationTool -j 4
```

The app artifact is generated under:

```text
build-vocal-tool2/vocal_annotation_tool_build/VocalAnnotationTool_artefacts/Debug/
```

## Notes

- C++20 / JUCE 8 via CMake FetchContent.
- ML inference for the annotation tool runs offline through Python scripts, not on the audio thread.
- Heavy datasets and experiment checkpoints are local-only by default.
