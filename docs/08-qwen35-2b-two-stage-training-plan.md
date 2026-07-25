# Qwen Small-Model Two-Stage Backing Vocal Training Plan

## Goal

Fine-tune a small Qwen model into a backing-vocal DSL model.

The model should learn in two stages:

1. Music-theory and voice-leading knowledge.
2. Synthetic Obsidian DSL input-to-output generation.

## Source Data

Stage 1 theory dataset:

`/Users/aleksandrkolesov/Downloads/vocal_harmony_theory_dataset_v0`

Stage 2 DSL dataset:

Full all-style dataset:

`/Users/aleksandrkolesov/Downloads/multitrack/dsl_backing_full`

Preferred Stage 2 one-style-per-file dataset:

`/Users/aleksandrkolesov/Downloads/multitrack/dsl_backing_by_style`

Compact Stage 2 smoke datasets:

`/Users/aleksandrkolesov/Downloads/qwen35_backing_compact_stage2_smoke_6x16`

`/Users/aleksandrkolesov/Downloads/qwen35_backing_mlx_data/stage2_compact_smoke_6x16_filtered`

Native-YAML vocal-window smoke datasets:

`/Users/aleksandrkolesov/Downloads/qwen3_backing_yaml_windows_smoke`

`/Users/aleksandrkolesov/Downloads/qwen3_backing_yaml_windows_smoke_filtered`

## Stage 1: Theory Tuning

Purpose:

- Teach interval naming, chord-degree analysis, motion types, consonance/dissonance, range judgement, voice-leading critique, preference selection, and repair behavior.
- Preserve structured answer discipline.

Input format:

- Existing chat-style JSONL rows from the theory dataset.
- Keep the original `messages` shape.

Training order:

1. `interval_tasks.jsonl`
2. `chord_degree_tasks.jsonl`
3. `motion_type_tasks.jsonl`
4. `voice_leading_analysis.jsonl`
5. `preference_pairs.jsonl`
6. `repair_tasks.jsonl`
7. `theory_mix_seed.jsonl`

Output:

- LoRA adapter: `models/qwen35_2b_stage1_theory_lora`

## Stage 2: DSL Tuning

Purpose:

- Teach mapping from source song DSL to `tracks.backing_vocals`.
- Teach stable structured output compatible with the DSL player pipeline.

Do not train the model to output all 35 backing sets in one response at first. The full YAML files are too large.

Use single-strategy examples from the split-by-style dataset:

- One training row = one song + one requested backing style.
- Input contains `meta`, `tracks.chords`, `tracks.lead_vocal`, and a requested style name/id.
- Output contains only one `tracks.backing_vocals` item for that requested style.

This turns 2,096 songs into 73,360 smaller supervised examples.

The split-by-style full-song YAML is still too verbose for practical local MLX training. The preferred Stage 2 format is native YAML vocal windows:

- one row = one requested backing style + one short region where `lead_vocal` actually has notes;
- input keeps ordinary YAML `meta`, `tracks.chords`, and `tracks.lead_vocal`;
- output keeps ordinary YAML `tracks.backing_vocals`;
- chords are included only around the vocal window, with small padding;
- backing notes are filtered by `source_lead_ref`.

Native YAML vocal-window smoke:

- script: `scripts/prepare_qwen_vocal_window_dsl_training_data.py`;
- source files read: 350 one-style YAML files;
- examples produced: 10,625;
- token filter `<= 3800`: 8,931 train / 465 valid;
- first 200 checked examples parse as YAML;
- 0.6B Stage 2 smoke, 100 iterations:
  - adapter: `/Users/aleksandrkolesov/Downloads/qwen35_backing_models/qwen3_0_6b_stage2_yaml_windows_smoke_lora/adapters.safetensors`;
  - val loss: `0.360 -> 0.051`;
  - peak memory: about `5.6 GB`;
  - inference produced valid `tracks.backing_vocals` YAML.

Known native-YAML smoke issue:

- after only 100 iterations on a small smoke slice, generated YAML can be structurally valid but musically weak;
- observed example repeated one pitch and had a bad duration;
- next step is a longer/full run plus invariant validation.

Compact JSON rows remain useful as a lower-token fallback:

- one row = one song window + one requested backing style;
- compact arrays are used with explicit `*_schema` fields;
- assistant output is `{"backing_vocals_compact":[...]}`;
- a postprocessor can expand compact JSON back to full YAML DSL.

Current practical smoke profile:

- `max_lead_notes`: 6
- `max_chords`: 16
- token filter: `<= 1900` Qwen chat-template tokens
- MLX `max-seq-length`: 2048
- retained examples from first 7,000 split-style files: 5,508 train / 290 valid

Larger attempted profiles:

- `32 lead / 96 chords`, filtered to 7,600 tokens: valid but too slow for interactive work.
- `12 lead / 32 chords`, filtered to 3,800 tokens: enough data, but hit Metal memory pressure at `max-seq-length 4096`.
- `8 lead / 24 chords`, filtered to 2,800 tokens: enough data, but still too slow at `max-seq-length 3072`.

Conclusion: train Stage 2 as vocal windows first. Prefer native YAML windows for final behavior; keep compact JSON as a lower-token fallback. Full-track generation should be assembled from overlapping windows.

Dataset shape:

```text
dsl_backing_by_style/
  artist/song/
    01_UNISON.yaml
    02_OCT_UP.yaml
    ...
    35_TENSION_RES.yaml
```

Recommended Stage 2 curriculum:

1. Simple one-part strategies: unison, octave, thirds, sixths, fourths, fifths.
2. Drone and pedal strategies.
3. Motion strategies: contrary, oblique, parallel thirds/sixths.
4. Choir voice strategies.
5. Multi-part styles: two-part, three-part, four-part, pop, folk, gospel, classical, barbershop.
6. Ornament/tension styles: suspension, passing, tension-resolution.

Output:

- LoRA adapter: `models/qwen35_2b_stage2_dsl_lora`

## Model Choice

Primary local model:

`Qwen/Qwen3-0.6B`

Reason:

- Much more practical on a 48 GB unified-memory Mac.
- Stage 1 and Stage 2 LoRA runs complete locally without Metal memory pressure.
- Stage 2 compact smoke reached low validation loss quickly.
- Adapter saving works reliably.
- Good candidate for fast iteration and local product experiments.

Observed local 0.6B results:

- Stage 1 theory, 200 iterations:
  - adapter: `/Users/aleksandrkolesov/Downloads/qwen35_backing_models/qwen3_0_6b_stage1_theory_lora/adapters.safetensors`
  - final observed val loss: about `0.190`
  - peak memory: about `2.4 GB`
- Stage 2 compact `6 lead / 16 chords`, 500 iterations:
  - adapter: `/Users/aleksandrkolesov/Downloads/qwen35_backing_models/qwen3_0_6b_stage2_compact_6x16_lora/adapters.safetensors`
  - final observed val loss: about `0.039`
  - peak memory: about `3.9 GB`
  - inference returns valid compact JSON after disabling Qwen thinking mode.

Known 0.6B issue:

- Generated `pitch` values can be musically correct while generated `midi_note` values are inconsistent.
- Treat `pitch` as authoritative and repair `midi_note` deterministically in postprocessing.

Larger local/backup model:

`Qwen/Qwen3.5-2B`

Reason:

- Apache-2.0.
- Qwen family is strong at structured/code-like output.
- Stage 1 theory LoRA completed successfully.
- Higher capacity than 0.6B if musical reasoning quality becomes the limiting factor.

Observed 2B limitation:

- Stage 2 compact runs at 2048 tokens reported about `31.9 GB` peak memory.
- Larger windows were slow or hit Metal memory pressure.
- Use 2B primarily for cloud training or later local experiments, not for the first fast loop.

Larger backup candidate:

`Qwen/Qwen3.5-4B-Base`

Use only in cloud if 0.6B/2B cannot learn YAML fidelity or harmony style separation.

## Long Track Strategy

The real product may receive a whole song as input. A larger model helps, but it does not remove sequence-length and output-length limits.

Therefore Stage 2 should teach chunked generation:

- Input contains whole-song metadata where useful and a bounded lead/chord window.
- Each example specifies a requested style id/name.
- Output contains only notes whose `source_lead_ref` falls inside that window.
- In production, generate backing vocals window-by-window and stitch parts by style/role.
- For short songs or sparse strategies, full-song generation can still be used.

Do not train the first DSL model to emit all 35 strategies for a whole long song in one response.

Recommended production windowing:

- Train on 6-12 lead-note vocal windows first.
- Add overlapping windows during dataset generation after the smoke is stable.
- Keep a deterministic postpass for stitching, note-id renumbering, YAML expansion, and invariant validation.
- Let the model solve the musical decision, not final file assembly.

## Training Backend

Preferred local backend:

- MLX / `mlx-lm` on Apple Silicon.

Fallback:

- Hugging Face Transformers + PEFT if MLX tooling blocks on Python/version support.

## Validation

After Stage 1:

- Run theory held-out prompts and check structured labels.

After Stage 2:

- Generate held-out single-strategy outputs.
- Validate YAML parseability.
- Validate DSL invariants:
  - one `tracks.backing_vocals` item for requested style;
  - non-empty `parts`;
  - non-empty `notes`;
  - required note fields;
  - `source_lead_ref` present;
  - no `analysis` in output.

## First Smoke Run

Completed:

1. Stage 1 JSONL train/valid files prepared.
2. Stage 1 LoRA trained on `Qwen/Qwen3.5-2B`.
   - adapter: `/Users/aleksandrkolesov/Downloads/qwen35_backing_models/qwen35_2b_stage1_theory_lora/adapters.safetensors`
   - final observed val loss: about `0.129`
3. Split-by-style Stage 2 dataset prepared:
   - source: `/Users/aleksandrkolesov/Downloads/multitrack/dsl_backing_by_style`
   - count: `73,360` YAML files
4. Compact Stage 2 smoke data prepared:
   - source: `/Users/aleksandrkolesov/Downloads/qwen35_backing_compact_stage2_smoke_6x16`
   - filtered MLX data: `/Users/aleksandrkolesov/Downloads/qwen35_backing_mlx_data/stage2_compact_smoke_6x16_filtered`

Observed Stage 2 smoke:

- `Qwen/Qwen3.5-2B` can load the Stage 1 adapter and train on compact rows.
- No `nan` and no truncation warnings in the 2048-token compact run.
- Loss moved from val `0.673` to val `0.371` by iteration 3.
- Peak reported memory was about `31.9 GB`.
- The short MLX run appeared to stall after validation/save, so the next run should avoid frequent saves and use a longer unattended run.

Completed 0.6B smoke:

- `Qwen/Qwen3.5-0.6B` does not exist as an accessible Hugging Face repo.
- Used `Qwen/Qwen3-0.6B`.
- Stage 1 theory LoRA completed in seconds/minutes with low memory.
- Stage 2 compact LoRA completed 500 iterations locally.
- Inference produced structurally valid compact JSON for an `OCT_UP` validation prompt.
- Postprocessing must validate/repair `midi_note` from `pitch`.
- Stage 2 native-YAML vocal-window smoke completed 100 iterations locally.
- Inference produced parseable native YAML with one `tracks.backing_vocals` item.

Next:

1. Add validation for generated native YAML windows:
   - YAML parse;
   - exactly one `tracks.backing_vocals` item;
   - non-empty `parts` and `notes`;
   - all `source_lead_ref` values exist in input lead;
   - note starts/durations stay close to referenced lead notes unless the style is pedal/drone;
   - `pitch` and `midi_note` agree.
2. Add deterministic `pitch`/`midi_note` repair in postprocessing.
3. Add overlapping window generation to `scripts/prepare_qwen_vocal_window_dsl_training_data.py`.
4. Run a longer 0.6B Stage 2 YAML-window job on more of the split-by-style dataset.
5. Test inference on unseen DSL files and several requested styles/windows.
6. Only then decide whether 2B/cloud is worth it.

Optional 2B overnight job:

   - `6 lead / 16 chords`
   - `max-seq-length 2048`
   - no frequent intermediate saves
   - resume from the Stage 1 theory adapter
