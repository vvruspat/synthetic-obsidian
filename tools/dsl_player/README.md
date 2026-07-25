# DSL Player

Small browser tool for inspecting generated Synthetic Obsidian DSL YAML files.

## Run

```bash
python3 -m http.server 8765 --directory tools/dsl_player
```

Then open:

```text
http://127.0.0.1:8765/
```

Use **Open DSL YAML** to load a generated file from:

```text
/Users/aleksandrkolesov/Downloads/multitrack/dsl
```

## Features

- Lead-vocal piano-roll visualization.
- Chord-grid visualization.
- Generated `backing_vocals` visualization, one lane per backing part.
- WebAudio preview synth for lead vocal and chords.
- WebAudio preview synth for generated backing vocal parts.
- Mute/solo controls per track or backing part.
- Seek, stop, play/pause, zoom, and follow-playhead controls.
- Click anywhere on the timeline to move the playhead.

The parser is intentionally scoped to the YAML structure emitted by
`scripts/prepare_multitrack_midi_dsl.py`.
