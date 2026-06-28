# DALI Zenodo Access Request Draft

Record:

- v1: https://zenodo.org/records/2577915
- v2/latest: https://zenodo.org/records/3576083

Suggested justification:

```text
I am requesting access to DALI for non-commercial research and prototyping of a
singing-vocal note and syllable detector. The intended use is to study aligned
lyrics, vocal-note timing, and phrase segmentation for an offline music
information retrieval model. The dataset will be used locally for research only
and will not be redistributed. I understand that DALI is licensed for
non-commercial use under CC BY-NC-SA 4.0 and that any commercial model or product
would require separate licensing review or replacement with proprietary training
data.
```

After approval:

```bash
cd /Users/aleksandrkolesov/synthetic-obsidian
export ZENODO_TOKEN=...
/opt/anaconda3/bin/python3 research/note_detector/download_dali_assets.py \
  --output-dir data/raw/dali \
  --record-id 2577915
```
