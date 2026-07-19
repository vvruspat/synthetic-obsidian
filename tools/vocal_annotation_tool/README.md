# Vocal Annotation Tool

Standalone offline annotator for creating gold vocal note/syllable datasets.

The tool is intentionally isolated from the realtime plugin target. Audio loading,
JSON import/export, pYIN subprocess analysis, and manual editing all live under
`tools/vocal_annotation_tool`.

## Build

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

## SoulX-Singer-SVC runtime

`Render BV` expects an isolated Python 3.10 runtime and the official SVC
checkpoint outside the repository:

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
- `Analyze` runs the existing `research/svc_pitch/analyze_notes_pyin.py` script
  in a background subprocess. It first creates `AI Parts` when none exist, then
  conforms pYIN note blocks to the visible AI/lyric boundaries. Manually adjusted
  AI splitters are therefore treated as the source of truth for note timing.
- `AI Parts` runs the multilingual GTSinger residual TCN checkpoint in a
  background Python subprocess. It detects syllable-onset candidates and
  breath/silence intervals. With aligned lyrics present, existing lyric text is
  preserved and its boundaries are snapped only to nearby TCN candidates.
  Syllable times receive an acoustic refinement pass within -180/+50 ms. It
  finds spectral/RMS attacks and backtracks each one to the beginning of the RMS
  rise, keeping splitters out of internal waveform peaks.
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
  vocal supplies phonetic timing and a short same-singer timbre prompt, while
  generated backing notes supply a replacement 50 Hz F0 contour. The worker is
  loaded lazily on the first render and reused for later renders; no model code
  runs on the audio thread. Runtime files and the 2.8 GB checkpoint live outside
  the repository under `~/.synthetic_obsidian/`.
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
