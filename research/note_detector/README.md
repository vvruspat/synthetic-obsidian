# Note Detector Research

This folder is the research workspace for a vocal note and syllable detector.
The goal is to train a model that produces stable note blocks with musical
rhythm, then use the result in the C++ UI/offline analysis path.

## Dataset Strategy

Use a two-stage dataset:

1. **DALI pretraining / weak labels**  
   DALI gives aligned lyrics and vocal-note annotations at note, word, line,
   and paragraph levels. Its labels are useful for learning broad vocal-note
   and syllable structure, but they are not clean enough to treat as final
   ground truth.

2. **Local MIDI + vocal fine-tuning**  
   Our highest-quality labels should come from aligned vocal audio plus MIDI or
   manually corrected notes. This is the data that should define the final
   timing, segmentation, and note-splitting behavior.

DALI is licensed as CC BY-NC-SA 4.0. Keep it in the research pipeline unless we
explicitly resolve the commercial/model-distribution implications.

## Normalized Label Format

`import_dali.py` writes:

- `labels/<dali_id>.json` with source metadata, all notes, and text segments.
- `manifest.jsonl` with trainable phrase chunks pointing back to the label file
  and optional local audio.

Each note is normalized to:

```json
{
  "start": 12.534,
  "end": 12.659,
  "freq_hz": 466.16,
  "midi": 70.0,
  "text": "wo",
  "index": 0
}
```

Each manifest row is a phrase-like chunk:

```json
{
  "dali_id": "song_id",
  "audio": "/path/to/song_id.wav",
  "label": "/path/to/labels/song_id.json",
  "start": 12.534,
  "end": 18.2,
  "text": "lyric phrase",
  "segment_level": "lines",
  "note_count": 8,
  "audio_missing": false
}
```

## Importing DALI

The DALI GitHub repository includes the code and a ground-truth timing file, but
not the per-song annotation data. Download the DALI_data `.gz` annotations first,
then run:

```bash
cd /Users/aleksandrkolesov/synthetic-obsidian
/opt/anaconda3/bin/python3 research/note_detector/import_dali.py \
  --dali-root /path/to/DALI_data \
  --dali-code-root /path/to/DALI/code \
  --ground-truth /path/to/gt_v1.0_22:11:18.gz \
  --audio-root /path/to/dali_audio \
  --output-dir data/note_detector/dali
```

For annotation-only import, omit `--audio-root`. `--dali-code-root` is optional;
the importer can unpickle normal DALI annotation objects without importing the
full package, but the flag is useful if future DALI versions add custom classes.
Add `--require-audio` when building an actual training manifest that must only
contain playable examples.

## Downloading Assets

The public pieces can be downloaded immediately:

```bash
cd /Users/aleksandrkolesov/synthetic-obsidian
/opt/anaconda3/bin/python3 research/note_detector/download_dali_assets.py \
  --output-dir data/raw/dali \
  --skip-restricted
```

The full DALI archive is restricted on Zenodo. Request access at:

- v1: https://zenodo.org/records/2577915
- v2/latest: https://zenodo.org/records/3576083

After access is approved, set a Zenodo token and rerun:

```bash
export ZENODO_TOKEN=...
/opt/anaconda3/bin/python3 research/note_detector/download_dali_assets.py \
  --output-dir data/raw/dali \
  --record-id 2577915
```

## Next Step

After importing, build the first training dataset from 3-12 second line chunks:

- audio input: mono vocal/mix excerpt
- targets: framewise voiced probability, MIDI pitch, note onset/offset, syllable
  boundary
- postprocess: combine model onsets with pitch plateaus into UI note blocks

## Local MIDI + Vox Import

For our own dataset of paired vocal audio and MIDI, use:

```bash
cd /Users/aleksandrkolesov/synthetic-obsidian
/opt/anaconda3/bin/python3 research/note_detector/import_midi_vox_dataset.py \
  --input-dir /path/to/segments \
  --output-dir data/note_detector/local_segments
```

This importer:

- matches `.wav` and `.mid` by basename
- stores full-note labels per source file
- measures rough `audio_activity_start` vs `first_midi_note_start`
- splits long files into trainable chunks using MIDI rest gaps and a max chunk size
- writes `manifest.jsonl`, `labels/`, and `summary.json`

The alignment is intentionally tolerant: imperfect MIDI-to-vocal sync is expected,
so the importer preserves offset metadata instead of pretending the pair is exact.

## Candidate-Match Dataset Builder

When we have a noisy CSV of possible `audio <-> MIDI` matches instead of clean
basename pairs, use the alignment-aware builder:

```bash
cd /Users/aleksandrkolesov/synthetic-obsidian
/opt/anaconda3/bin/python3 research/note_detector/build_candidate_dataset.py \
  --candidate-csv /Users/aleksandrkolesov/Develop/vocals-preparation/result/candidate_matches.csv \
  --output-dir data/note_detector/candidate_matches_dataset \
  --min-confidence 0.78 \
  --min-quality-score 0.48 \
  --max-pitch-mae 3.5
```

This builder does more than filename matching:

- filters weak metadata candidates early
- prefers lead-vocal-like track names over obvious backing/chorus tracks
- enforces one-to-one `audio_id <-> midi_id` selection
- scores coarse alignment with audio onset strength vs MIDI note onsets
- estimates pitch agreement with `librosa.pyin`
- shifts MIDI notes by the best detected global offset before chunking

The current full run produced:

- `26` kept pairs from `410` raw candidates
- `844` trainable chunks
- `108.7` minutes of vocal audio

Artifacts are written to:

- `/Users/aleksandrkolesov/synthetic-obsidian/data/note_detector/candidate_matches_dataset/manifest.jsonl`
- `/Users/aleksandrkolesov/synthetic-obsidian/data/note_detector/candidate_matches_dataset/summary.json`
- `/Users/aleksandrkolesov/synthetic-obsidian/data/note_detector/candidate_matches_dataset/labels/`

## Baseline Training

The first baseline is a framewise detector trained on local MIDI+vox chunks:

- input: log-mel spectrogram
- targets: `voiced`, `onset`, `pitch_class`
- model: lightweight temporal CNN
- loss: BCE for voiced/onset + cross-entropy for pitch bins

Run:

```bash
cd /Users/aleksandrkolesov/synthetic-obsidian/research
/opt/anaconda3/bin/python3 -m nvt.note_detector.train \
  --manifest /Users/aleksandrkolesov/synthetic-obsidian/data/note_detector/local_segments/manifest.jsonl \
  --output-dir /Users/aleksandrkolesov/synthetic-obsidian/research/checkpoints_note_detector
```

This is intentionally a baseline, not the final architecture. Its job is to tell
us whether the local dataset is learnable before we invest in a more advanced
pitch+boundary model.

The current baseline uses coarse MIDI bins rather than raw pitch regression and
trims pitch supervision near note boundaries, because local MIDI/audio alignment
is not sample-accurate.

## Baseline Inference

Decode note blocks from a trained baseline checkpoint:

```bash
cd /Users/aleksandrkolesov/synthetic-obsidian/research
/opt/anaconda3/bin/python3 -m nvt.note_detector.infer \
  --checkpoint /Users/aleksandrkolesov/synthetic-obsidian/research/checkpoints_note_detector_cls/best.pt \
  --audio /path/to/vocal.wav \
  --output-json /tmp/note_detector_output.json
```

Or evaluate one prepared manifest chunk directly:

```bash
/opt/anaconda3/bin/python3 -m nvt.note_detector.infer \
  --checkpoint /Users/aleksandrkolesov/synthetic-obsidian/research/checkpoints_note_detector_cls/best.pt \
  --manifest /Users/aleksandrkolesov/synthetic-obsidian/data/note_detector/local_segments/manifest.jsonl \
  --manifest-row sample_000004_chunk_000
```

The current recommended decoder is `--decode-mode hybrid`, which combines:

- model voiced probability
- model onset head
- audio onset strength
- audio energy flux

This hybrid pass improved note-count matching slightly over the pure ML decoder
in local validation, while keeping pitch accuracy roughly the same.

## Phrase Benchmarking And Decoder Tuning

For short hand-labeled phrases, use:

```bash
/opt/anaconda3/bin/python3 research/note_detector/benchmark_phrase.py \
  --checkpoint /Users/aleksandrkolesov/synthetic-obsidian/research/checkpoints_note_detector_finetune/best.pt \
  --audio /path/to/phrase.wav \
  --midi /path/to/phrase.mid \
  --start 0.0 \
  --end 8.0
```

The benchmark now reports both raw pitch metrics and octave-normalized pitch
metrics, which is important for vocal references where the MIDI may be written
in a different octave/register than the recorded take.

For a small dev-set of phrase snippets, decoder tuning can be grid-searched with:

```bash
/opt/anaconda3/bin/python3 research/note_detector/tune_decoder.py \
  --checkpoint /Users/aleksandrkolesov/synthetic-obsidian/research/checkpoints_note_detector_finetune/best.pt \
  --config-json /Users/aleksandrkolesov/synthetic-obsidian/research/note_detector/dev_phrases.json
```

The current best dev-set profile found by this search is:

- `voiced_threshold = 0.51`
- `onset_threshold / hybrid_threshold = 0.26`
- `pitch_jump_semitones = 1.15`
- `min_note_seconds = 0.11`
- `min_same_pitch_split_seconds = 0.18`
