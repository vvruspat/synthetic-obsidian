# SVC Pitch / Backing Vocal Spike

This spike replaces the DDSP autoencoder path as the candidate base model for
natural pitch correction and future backing-vocal generation.

## Current Status

As of 2026-04-17, Seed-VC is the best-sounding backend in the local listening
tests and is wired into the Standalone UI through the `AI BACKS` button.

The current product-facing flow is intentionally offline:

1. Select a vocal track in the plugin.
2. Click `AI BACKS`.
3. The C++ processor launches Seed-VC from a background thread through
   `Source/ai/SeedVCBridge`.
4. Seed-VC renders four backing-vocal intervals: `+3`, `+4`, `+7`, `+12`.
5. The files are passed through `polish_outputs.py --mode headroom`.
6. The UI adds them back as `Back Vox` tracks.

The working local paths are:

- Seed-VC checkout: `third_party/seed-vc`
- Python environment: `research/.venv_seed_vc`
- single-file runner: `research/svc_pitch/run_seed_vc_file.py`
- output polish: `research/svc_pitch/polish_outputs.py`
- plugin output root: `~/Music/Synthetic Obsidian/SeedVC/`

Do not move Seed-VC into the realtime path. This spike is an offline/background
render path only.

## Why This Exists

The current DDSP model is editable, but it reconstructs vocals from a coarse
harmonic/noise representation. That makes it fragile: F0 dropouts, voicing
mistakes, and consonants can turn into synthetic pitched artifacts.

WORLD was also rejected by listening tests: it preserves more of the original
envelope, but the result is too vocoder-like for the target quality.

The next base model should be an SVC/RVC-style model:

```text
audio -> content encoder -> F0 contour -> speaker/timbre conditioning -> neural vocoder -> audio
```

This gives us the right control point: edit F0 for tuning/harmonies while the
model keeps phonetic content and vocal timbre.

## Candidate Backends

1. Seed-VC for the current zero-shot quality path and MVP backing vocals.
2. RVC-style model for a possible productized custom voice preset path.
3. DDSP-SVC only if the first two fail; it is closer to SVC than to our current
   DDSP autoencoder, but still carries DDSP coloration risk.

## Evaluation Set

Use the same four clips that exposed the DDSP problems:

- `female_straight`
- `female_vibrato`
- `male_belt`
- `male_breathy`

For each backend, generate:

- identity reconstruction: no pitch edit
- `+2` semitones
- `-2` semitones
- optional harmony intervals for backing-vocal checks: `+3`, `+4`, `+7`, `+12`

## Pass Criteria

Identity reconstruction must pass before pitch edits matter.

- No strong robotic/vocoder tone on `female_straight`
- No pitched consonants from breaths or sibilants
- No obvious clicks or single-sample jumps
- Pitch-shifted versions should preserve singer identity better than WORLD and
  the current DDSP model
- Backing-vocal intervals should sound like alternate takes, not synth layers

Current listening result:

- Seed-VC passed well enough for backing-vocal generation.
- The user's `test-sasha.wav` result was accepted as the current quality bar.
- `headroom` polish is preferred over stronger waveform repair; aggressive
  repair reduced peaks but sounded more distorted/clipped.

## Integration Notes

The plugin architecture must keep this model off the audio thread. Inference is
an offline/background render path, with a real-time DSP preview path remaining
separate.

The first implementation is a research adapter wired into the plugin via Python
subprocess. Productization can later replace this with an exported runtime, but
the current UI integration should stay simple and preserve the audio/AI thread
boundary.

Important implementation details:

- `juce::ChildProcess` must be started with `juce::StringArray` arguments. A
  manually quoted command string broke paths and made Python look for
  `//"/Users/.../run_seed_vc_file.py"`.
- `polish_outputs.py` uses `soundfile.read/write`, not `torchaudio.load`.
  In the current venv, `torchaudio.load` can route through `torchcodec` and
  fail when launched from the plugin.
- The UI consumes polished files from the `headroom_only/` output tree, not
  the raw Seed-VC render directory.
- If `AI BACKS` fails, first inspect the alert text. Path/venv issues usually
  show before model-quality issues.

## Useful Commands

Run Seed-VC for one source file from the repo root:

```bash
research/.venv_seed_vc/bin/python research/svc_pitch/run_seed_vc_file.py \
  --source /path/to/vocal.wav \
  --output-root data/svc_pitch_seed_vc/manual_test \
  --python-bin research/.venv_seed_vc/bin/python \
  --diffusion-steps 30 \
  --interval harmony_minor_third:3 \
  --interval harmony_major_third:4 \
  --interval harmony_fifth:7 \
  --interval harmony_octave:12
```

Apply the currently preferred polish:

```bash
research/.venv_seed_vc/bin/python research/svc_pitch/polish_outputs.py \
  --input-dir data/svc_pitch_seed_vc/manual_test \
  --output-dir data/svc_pitch_seed_vc/manual_test_headroom_only \
  --target-peak-db -3.0 \
  --mode headroom
```
