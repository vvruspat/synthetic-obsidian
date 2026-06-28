# DDSP Neural Pipeline — Synthetic Obsidian

> Замена WORLD vocoder на дифференцируемый source-filter pipeline с нейронным вокодером.
> ADR-008. Phase 0 validation: неделя 1. Production: Phase 4.

---

## Архитектура

```
┌─────────────────────────────────────────────────────────────┐
│                      ANALYSIS STAGE                          │
├─────────────────────────────────────────────────────────────┤
│  audio (mono, 24kHz)                                         │
│    ├──► F0 Encoder (torchcrepe)         → f0(t), voiced(t) │
│    ├──► Loudness Encoder (A-weighted)   → loudness(t)       │
│    ├──► Timbre Encoder (1D CNN + GRL)   → z_timbre(t) ∈ ℝ³²│
│    └──► Noise Encoder  (1D CNN)         → z_noise(t)  ∈ ℝ¹⁶│
└─────────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────────┐
│                       EDIT STAGE                             │
├─────────────────────────────────────────────────────────────┤
│  Piano Roll → note grid                                      │
│  f0' = apply_corrections(f0, note_grid)                     │
│  z_timbre, z_noise, loudness — не изменяются                │
└─────────────────────────────────────────────────────────────┘
                            │
┌─────────────────────────────────────────────────────────────┐
│                    SYNTHESIS STAGE                           │
├─────────────────────────────────────────────────────────────┤
│  (f0', loudness, z_timbre, z_noise)                          │
│    ├──► Harmonic Synth (DDSP additive)  → harmonic_audio    │
│    ├──► Noise Synth (filtered noise)    → noise_audio       │
│    └──► Vocos 24kHz                     → final audio       │
└─────────────────────────────────────────────────────────────┘
```

---

## Компоненты

### F0 Encoder
- `torchcrepe` (full variant) — точнее pyworld harvest на вокале
- Frame rate: 200Hz (5ms hop), совместим с текущим `world_bridge.py`
- Confidence threshold: 0.4 для маркировки unvoiced регионов

### Timbre Encoder
- Input: log-mel spectrogram (80 bins, FFT 1024, hop 256)
- Architecture: 1D CNN, 6 residual blocks, strided conv → F0 frame rate
- Output: `z_timbre(t) ∈ ℝ^32`
- Disentanglement: Gradient Reversal Layer + F0 predictor head
  - Loss: `L_disent = -λ · MSE(F0_pred, F0_true)` (gradient reversed)
  - `z_timbre` несёт информацию о форматах но НЕ о питче

### Noise Encoder
- Аналогичная архитектура, output `z_noise(t) ∈ ℝ^16`
- Кодирует сибиляты, дыхание, шум согласных
- Не меняется при питч-коррекции → сибиляты остаются без искажений

### DDSP Harmonic Synthesizer
- Аддитивный осциллятор, до 100 гармоник
- Амплитуды гармоник: MLP(z_timbre) → harmonic_amps[100]
- Fundamental = f0' (скорректированный питч из piano roll)
- Нет математической оценки форманты — всё learnable

### DDSP Noise Synthesizer
- Белый шум → learnable time-varying FIR filter
- Filter coefficients: MLP(z_noise) → fir_coeffs[64]
- Независим от F0 — критично для сохранения сибилянтов

### Vocos Vocoder
- Уже интегрирован (`world_bridge.py`)
- Input: mel(harmonic + noise) вместо mel(WORLD output)
- Убирает синтетичность DDSP — восстанавливает micro-texture голоса

---

## Функции потерь

```python
L_total = (
    1.0 * L_multiscale_spectral   # multi-scale STFT reconstruction
  + 0.1 * L_f0_consistency        # f0(output) ≈ f0_target
  + 0.01 * L_adversarial_disent   # z_timbre ⊥ f0 (GRL)
  + 0.1 * L_perceptual            # wav2vec2 feature matching (Phase 2+)
)
```

---

## Фазы обучения

| Фаза | Что включено | Цель |
|------|-------------|------|
| Phase 0 | DDSP autoencoder без диsentanglement и Vocos | Sanity check: reconstruction работает |
| Phase 1 | + GRL disentanglement | z_timbre F0-инвариантен |
| Phase 2 | + pre-trained Vocos fine-tune last layers | Качество как neural TTS |
| Phase 3 | Fine-tune на paired (original, shifted) примерах | Агрессивные сдвиги ±3 ст. |

---

## Данные

| Датасет | Размер | Цель | Лицензия |
|---------|--------|------|---------|
| VocalSet | ~10h, 20 певцов | Disentanglement (multi-pitch per singer) | CC BY-NC |
| NUS-48E | ~2h, 12 певцов | Чистый вокал, выровненный | Research |
| OpenSinger | ~85h, 93 певца | Масштаб + разнообразие | Research |

**Pre-processing:**
1. Resample → 24kHz mono
2. Loudness normalize → -23 LUFS
3. Trim silence (WebRTC VAD)
4. Pre-compute F0 (torchcrepe), log-mel, loudness → кэшируем `.pt`

---

## Deployment pipeline

```
PyTorch training
      │
      ▼
torch.onnx.export → model.onnx
      │
      ▼
onnx-simplifier   → model_simplified.onnx
      │
      ▼
ort (Rust) inference в плагине
```

---

## Текущий статус

- [ ] Phase 0 validation spike (benchmark_vs_world.py)
- [ ] VocalSet data prep (prepare_vocalset.py)
- [ ] TimbreEncoder + HarmonicSynth baseline training
- [ ] Disentanglement (Phase 1)
- [ ] Vocos integration в training pipeline (Phase 2)
- [ ] ONNX export (Phase 3)
- [ ] Rust inference в плагине (Phase 4)

*`world_bridge.py` остаётся рабочим fallback до завершения Phase 3.*
