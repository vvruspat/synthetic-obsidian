#!/bin/bash
# Download VocalSet and prepare it for NVT training.
# Run from the repo root: bash research/scripts/download_and_prepare.sh

set -e

DATA_DIR="${DATA_DIR:-/Users/aleksandrkolesov/synthetic-obsidian/data}"
DOWNLOADS="$DATA_DIR/downloads"
RAW_DIR="$DATA_DIR/vocalset_raw"
PROCESSED_DIR="$DATA_DIR/vocalset_processed"
PYTHON=/opt/anaconda3/bin/python3

mkdir -p "$DOWNLOADS" "$RAW_DIR" "$PROCESSED_DIR"

# ── 1. Check if already downloaded ───────────────────────────────────────────
VOCALSET_ZIP="$DOWNLOADS/VocalSet.zip"

if [ ! -f "$VOCALSET_ZIP" ]; then
    echo "Downloading VocalSet (2.3 GB)..."
    wget -q --show-progress \
        "https://zenodo.org/api/records/10200775/files/VocalSet.zip/content" \
        -O "$VOCALSET_ZIP"
else
    echo "VocalSet.zip already exists ($(du -sh "$VOCALSET_ZIP" | cut -f1))"
fi

# ── 2. Unzip ─────────────────────────────────────────────────────────────────
if [ ! -d "$RAW_DIR/FULL" ] && [ ! -d "$RAW_DIR/VocalSet" ]; then
    echo "Extracting..."
    unzip -q "$VOCALSET_ZIP" -d "$RAW_DIR"
    echo "Extracted to $RAW_DIR"
else
    echo "Already extracted."
fi

# Find the actual audio root (VocalSet has nested dirs)
AUDIO_ROOT=$(find "$RAW_DIR" -name "*.wav" | head -1 | xargs dirname | xargs dirname | xargs dirname 2>/dev/null || echo "$RAW_DIR")
echo "Audio root: $AUDIO_ROOT"
WAV_COUNT=$(find "$AUDIO_ROOT" -name "*.wav" | wc -l | tr -d ' ')
echo "Found $WAV_COUNT .wav files"

# ── 3. Prepare ────────────────────────────────────────────────────────────────
echo ""
echo "Running prepare_vocalset.py (crepe tiny, 4 workers)..."
cd "$(dirname "$0")/../.."

$PYTHON research/scripts/prepare_vocalset.py \
    --input_dir "$AUDIO_ROOT" \
    --output_dir "$PROCESSED_DIR" \
    --sample_rate 24000 \
    --hop_length 256 \
    --workers 4 \
    --crepe_model tiny \
    --extensions .wav

echo ""
echo "Done! Manifest: $PROCESSED_DIR/manifest.json"
echo ""
echo "To train:"
echo "  cd research && $PYTHON -m nvt.train data.manifest=$PROCESSED_DIR/manifest.json"
