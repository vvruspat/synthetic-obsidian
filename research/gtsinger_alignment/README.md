# GTSinger Alignment Pilot

This directory contains an offline research pilot for audio-only vocal boundary
prediction. It does not integrate with the plugin or run on the audio thread.

## Scope

The manifest builder consumes each available GTSinger singing `.wav` plus its
JSON sidecar. TextGrid files are not required, which makes the builder useful
while the dataset download is incomplete. Paired speech is excluded by default
and can be enabled explicitly with `--include-paired-speech`.

Each manifest record contains:

- dataset-relative paths and audio metadata
- language, singer, technique, song, rendition group, and clip ID
- a deterministic 80/10/10 train/validation/test split grouped by
  language/singer/song to prevent renditions of the same song crossing splits
- onset events for phonemes, syllable proxies, breaths, and silences
- complete breath and silence intervals

GTSinger explicitly labels breaths as `<AP>` and silences as `<SP>`. It does
not provide a syllable tier. The pilot therefore uses the onset of every
detected vowel nucleus as a `syllable` proxy. The heuristic supports the
observed ARPABET, IPA, and romanized Chinese phone inventories. It is useful
for a first experiment, but it is not a substitute for reviewed
language-specific syllabification. Training rederives this proxy from the
annotation sidecars by default so corrected inventory rules apply to existing
manifests. `--use-manifest-syllable-labels` restores the original manifest
events for comparison.

## Existing Stack

The baseline uses packages already present in `research/.venv_seed_vc` and
already represented in `research/pyproject.toml`: PyTorch, librosa, NumPy, and
SoundFile. No dependency is added.

The model is a compact residual dilated temporal convolutional network over
normalized 64-bin log-mel features. It keeps 64 hidden channels and uses four
kernel-3 residual blocks with dilations `1, 2, 4, 8`. This gives an exact
31-frame, 310 ms encoder receptive field at the 10 ms frame rate while
remaining below 75,000 parameters and using only MPS-friendly `Conv1d`, ReLU,
residual addition, and 1x1 projections.

The network retains the same four outputs:

- `phoneme` and `syllable` predict onset probabilities; targets are widened by
  30 ms by default for annotation and frame quantization tolerance
- `breath` and `silence` predict whether each frame lies inside the annotated
  interval; manifest intervals are rasterized as half-open `[start, end)` masks

Inverse-frequency positive weighting is retained for sparse onset heads.
Interval heads use square-root inverse-frequency weighting to avoid making rare
silence frames dominate the loss. The exact weights and policy are stored in
the checkpoint.

The checkpoint records the encoder channels, kernel size, dilation schedule,
convolutions per block, and receptive field. Checkpoints from the earlier
two-layer CNN are not state-dict compatible with this TCN architecture.

Validation scores the output according to each head's task:

- phoneme/syllable use local peaks and one-to-one onset matching within 50 ms
- breath/silence use framewise segmentation precision, recall, and F1
- breath/silence onset predictions are additionally derived from rising edges
  of their thresholded interval masks and matched within 50 ms
- thresholds are selected per class from `0.10` through `0.90`; onset heads
  optimize event F1, while interval heads optimize segmentation F1
- fixed-threshold metrics remain in the report for comparison

The calibrated validation score measures fit to that validation set and is
optimistic. Freeze the stored thresholds and evaluate once on the untouched
test split before making quality claims. The onset-head frame score is retained
only as `onset_frame_diagnostic_at_0_5`; it measures widened target frames and
is not an event detector quality metric.

## Commands

Run from the repository root.

Inspect a deterministic, diverse sample without writing output:

```bash
research/.venv_seed_vc/bin/python \
  research/gtsinger_alignment/build_manifest.py \
  --dataset-root data/GTSinger \
  --limit 24 \
  --dry-run
```

Build a small English smoke manifest:

```bash
research/.venv_seed_vc/bin/python \
  research/gtsinger_alignment/build_manifest.py \
  --dataset-root data/GTSinger \
  --language English \
  --output /tmp/gtsinger_alignment_english_smoke.jsonl \
  --limit 24
```

Validate baseline inputs without loading audio:

```bash
research/.venv_seed_vc/bin/python \
  research/gtsinger_alignment/train_baseline.py \
  --manifest /tmp/gtsinger_alignment_english_smoke.jsonl \
  --dataset-root data/GTSinger \
  --dry-run
```

Run a deliberately small training smoke test:

```bash
research/.venv_seed_vc/bin/python \
  research/gtsinger_alignment/train_baseline.py \
  --manifest /tmp/gtsinger_alignment_english_smoke.jsonl \
  --dataset-root data/GTSinger \
  --output /tmp/gtsinger_alignment_english_smoke.pt \
  --epochs 1 \
  --max-records 2 \
  --max-steps-per-epoch 2 \
  --batch-size 2
```

Run unit tests:

```bash
PYTHONPATH=research/gtsinger_alignment \
  research/.venv_seed_vc/bin/python -m unittest \
  research/gtsinger_alignment/test_pipeline.py
```

For a full pilot, omit `--limit`, build the manifest once, then train with
larger `--max-records` values before committing to a long run. The checkpoint
stores feature settings, event order, label and event tolerances, calibrated
thresholds, per-head output semantics, class weights, seed, and training
history. Model parameters and thresholds come from the epoch with the highest
validation macro-F1 across phoneme/syllable event F1 and breath/silence
segmentation F1; `best_epoch` and the final epoch are both recorded.

## Limitations

- Phoneme and syllable remain onset-only; their durations are not modeled.
- The syllable target is a cross-language vowel-nucleus proxy.
- Thresholds are calibrated on validation. Validation metrics must not be
  presented as held-out product quality; use frozen thresholds on the test
  split for that purpose.
- Breath/silence segmentation metrics are framewise and do not measure
  interval-level overlap, duration error, or fragmentation directly. Rising
  edge metrics cover onset timing only.
- Partial downloads are accepted. Malformed or incomplete pairs are skipped
  unless `--strict` is supplied, and skipped paths are written beside a
  generated manifest.
