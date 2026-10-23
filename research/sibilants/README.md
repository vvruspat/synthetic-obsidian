# Separate sibilant classifier

Offline experimental classifier trained on the user-confirmed, time-aligned
`raw.wav` / isolated `ess.wav` pair. It does not modify the v11 note-boundary
network, the running application, manual annotations, or source recordings.

The 23 October 2026 source pair contains 8,879.36 seconds at 48 kHz, stereo.
Complete 120-second blocks receive fixed train/validation/test roles (60/7/7).
Both margins of every block exclude 660 ms, covering model and FFT context.
Songs, singers, or repeated material may occur in different roles, so results
are an internal time-block check, not unseen-singer accuracy.

Targets come only from the isolated stem. Positive frames require ESS RMS
at least 1e-5 and at least 0.001 of raw RMS; definite negatives have ESS RMS
at most 1e-7. Quiet fades remain unknown. A 10 ms phoneme-edge guard preserves
the interior of short consonants. No v11 prediction supplies training labels.

The selected model is `sasha-sibilants-v1-726644e6f21b`: a 60,097-parameter
TCN, 48 channels and dilations 1/2/4/8/16/32, with 1.27 s receptive field.
Input uses 24 kHz, 1024-point centered periodic Hann FFT, 10 ms hop, 80 Slaney
mel bands from 50–11,800 Hz, plus log energy, spectral flatness, energy ratios
above 3.5/7 kHz, normalized spectral centroid, and zero-crossing rate.
Normalization is frozen from training blocks only. JSON contains the weights,
normalization, source hashes, and contract; `checkpoint.pt` is the training copy.
Research inference runs in Python/PyTorch on CPU or MPS. The product now also
uses a separate JUCE/C++ runner, exported with `research.sibilants.export_native`.
Its `synthetic-obsidian-sibilant-native-v1` JSON contains frozen Slaney filters
and convolution arrays in output/kernel/input order. Polyphase resampling,
spectral features, normalization and TCN inference match Python; no Python or
ONNX runtime is required by the app's classifier. Everything runs in the existing
AI Parts background worker, with lazy cached weights and cancellation.

Selected epoch: 17/25, using validation data. The final candidate improves
validation F1 over the initial wider edge masks. Thresholds are chosen on
validation: 0.48 for balanced detection and 0.89 for attachment proposals.
The stricter threshold targets at least 99.5% precision on guarded validation
interiors; it does not guarantee that precision on full phoneme boundaries.

On 829.25 seconds of test frames **including phoneme edges**, the stricter
mode has precision 97.85%, recall 93.19%, F1 95.46%. The balanced mode has
precision 96.46%, recall 95.38%, F1 95.91%. Only very quiet ambiguous fades and
block margins are excluded from that evaluation. Guarded-interior scores are
reported separately and are higher. CPU/MPS probability delta is below 7e-7.

`merge_sibilant_notes` produces a separate offline proposal when more than 50%
of a note's duration exceeds the balanced 0.48 threshold, matching the white
overlay. Partial edge frames are weighted by overlap time. Exactly 50% is kept.
It preserves the full sound interval and attaches to the closest non-sibilant note.
Attachment is vetoed by a voiced core: a continuous non-sibilant run with native
YIN confidence >= 0.8, lasting 25% of the note, clamped to 30–80 ms. The original
note curves from v11 supply this positive periodicity evidence; pitch may move
during the run. Missing pitch alone never triggers attachment. This protects
notes with long consonants, including short notes with a shorter tonal core.
The user requested this nearest-note rule on 23 October: equal gaps prefer the
previous note; gaps over 120 ms are retained. This rule is a timing heuristic;
classification alone does not supervise attachment direction.
The product applies it to newly generated v11 drafts before rebuilding markers
and RMVPE enrichment. Existing manually edited projects are changed only when
the user requests a new AI Parts draft. The manual MIDI and v11 proposal remain
comparison material, not classifier training labels.

During tuning, a temporary white waveform overlay showed every positive 10 ms
frame at the balanced 0.48 threshold. It was removed from the release UI after
review; native frame diagnostics remain available via `--inspect-sibilants`.
On 23 October the user replaced the initial 0.89 / 85% attachment policy with
the same mask threshold, a strict majority of note duration, and a voiced-core guard.

Native checks: probabilities/decisions on all 10,788 Verse_1 frames agree with
Python (maximum probability delta below 4e-6, zero threshold flips), and fixtures
pass at 16/24/44.1/48/96 kHz. The initial 0.89 / 85% policy produced 97 intervals;
the majority-white policy produces 92. Native/Python merged IDs and times agree.
The voiced-core guard restores the valid 24.10 s boundary (93 intervals), while
still attaching the separate 24.98–25.47 s sibilant fragment. These examples are
regression checks, not an estimate of generalization accuracy.
No existing model schema, document fields or parameter IDs changed. A separate
native-classifier schema was added. Windows compilation remains untested here.

Local artifacts: `data/sibilants_v1_2026-10-23/`. Selected weights are installed
separately at `~/.synthetic_obsidian/models/sibilants/sasha-v1-experimental/`.
The checkpoint, manifest, frame statistics, predictions, and source snapshot
are archived under `~/.synthetic_obsidian/training/sibilants/sasha-v1-2026-10-23/`.
The archive references the immutable original audio instead of duplicating 6.8 GB.
The product bundles `native-model.json` via `SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL`.

```sh
research/.venv_seed_vc/bin/python -m research.sibilants.prepare \
  --raw /path/to/raw.wav --ess /path/to/ess.wav \
  --output data/sibilants_v1_2026-10-23/prepared-edges

research/.venv_seed_vc/bin/python -m research.sibilants.train \
  --manifest data/sibilants_v1_2026-10-23/prepared-edges/manifest.json \
  --output data/sibilants_v1_2026-10-23/training-edges

research/.venv_seed_vc/bin/python -m research.sibilants.evaluate \
  --manifest data/sibilants_v1_2026-10-23/prepared-edges/manifest.json \
  --statistics data/sibilants_v1_2026-10-23/pair-frame-statistics.npz \
  --training data/sibilants_v1_2026-10-23/training-edges

research/.venv_seed_vc/bin/python -m research.sibilants.predict \
  --model ~/.synthetic_obsidian/models/sibilants/sasha-v1-experimental/model.json \
  --audio /path/to/vocal.wav --notes /path/to/proposal.annotation.json \
  --output data/sibilant-proposals

research/.venv_seed_vc/bin/python -m unittest research.sibilants.test_pipeline -v
```

Use the existing training environment; no additional dependencies were installed.
