# Compact Backing-Vocal Transformer

This experiment replaces autoregressive YAML generation for one-to-one backing
styles with a small encoder-only Transformer. It predicts one semitone offset
for every lead note in parallel. Timing and note identity remain deterministic.

The first model intentionally excludes pedal, multipart harmony, and multipart
genre styles. Those need a second duration/action or multi-voice output head.

## Smoke dataset

```bash
.venv-llm/bin/python scripts/backing_transformer/prepare_dataset.py \
  --input ~/Downloads/qwen3_backing_yaml_phrase_windows_full_min4_filtered_3800/train.jsonl \
  --output-root ~/Downloads/backing_transformer_smoke \
  --max-per-style 300
```

## Train

```bash
.venv-llm/bin/python scripts/backing_transformer/train.py \
  --data-root ~/Downloads/backing_transformer_smoke \
  --output ~/.synthetic_obsidian/models/backing_vocals/backing_transformer_v1 \
  --epochs 12
```
