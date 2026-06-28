# Журнал архитектурных решений — Synthetic Obsidian

## Формат записи
Каждое решение: дата, проблема, выбранный вариант, обоснование, последствия.

---

## ADR-001: Лицензия проекта

- **Дата:** 2026-04-11
- **Проблема:** RVC использует AGPL-3.0. Коммерческий закрытый продукт невозможен без переписывания.
- **Решение:** Плагин публикуется под AGPL-3.0
- **Обоснование:** Заказчик принял open-source модель
- **Последствия:** Весь код открыт. Монетизация через поддержку, облачные сервисы, или донейшн.

---

## ADR-002: ARA2 — Phase 2

- **Дата:** 2026-04-11
- **Проблема:** ARA2 добавляет сложность с первого дня разработки
- **Решение:** MVP без ARA2. ARA2 в Phase 2.
- **Обоснование:** Упрощает архитектуру MVP, ускоряет первый релиз
- **Последствия:** MVP = Standalone + VST3. Phase 2 добавляет ARA2 (только Cubase, Studio One, Reaper, Bitwig — не Logic Pro).

---

## ADR-003: RVC Training — локально

- **Дата:** 2026-04-11
- **Решение:** Обучение происходит локально на машине пользователя
- **Последствия:** UI для Training, прогресс-бар, background thread. CPU режим: 6-8 часов. GPU режим: 30-45 минут. Инсталлятор включает Python runtime + модели (~800MB-1GB).

---

## ADR-004: GPU рекомендована, CPU fallback

- **Дата:** 2026-04-11
- **Решение:** GPU ускорение через ONNX Runtime (CUDA/CoreML/DirectML). Обязательный CPU fallback.
- **Последствия:** Offline-режим для CPU (RVC inference ~200-500ms — только post-processing). Real-time pitch correction через DSP (pYIN/WORLD) без GPU.

---

## ADR-005: Порядок разработки

- **Дата:** 2026-04-11
- **Решение:** Standalone → VST3 → ARA2 (Phase 2)
- **Обоснование:** Позволяет тестировать без DAW, быстрый MVP

---

## ADR-006: WORLD vocoder как основной метод pitch correction; RVC только для voice conversion

- **Дата:** 2026-04-13 (уточнено 2026-04-13)
- **Проблема:** Традиционные алгоритмы pitch correction (phase vocoder, Rubberband) при сдвиге питча на несколько полутонов слышимо искажают вокал — форманты тянутся вместе с питчем. Изначально планировался RVC как замена, но это смешивает два принципиально разных понятия.

### Ключевое разделение задач

| Задача | Что нужно | Инструмент | Нужен пресет? |
|--------|-----------|------------|---------------|
| **Pitch correction** (тот же голос, другой питч) | Формантная структура из самого клипа | **WORLD vocoder** | Нет — берётся из клипа |
| **Voice conversion** (бэк-вокал другим тембром) | Обученная модель целевого голоса | **RVC** | Да — нужно обучение |

### Почему WORLD, а не RVC для коррекции питча

WORLD разделяет сигнал на три независимых слоя: **F0** (питч), **SpectralEnvelope** (тембр/форманты), **Aperiodicity** (дыхание). При коррекции меняется только F0 — формантная структура берётся напрямую из оригинального клипа поточечно, без обучения. Работает на клипе любой длины, без GPU.

RVC — это voice *conversion*: он заменяет голос на другой. Применять его для тюнинга собственного голоса избыточно и требует обучения.

### Pipeline offline pitch correction (финальный)

```
Clip audio
    │
    ├─ harvest(x, fs)      → F0[] (detected per-frame pitch)
    ├─ cheaptrick(x, f0)   → SpectralEnvelope[][] (формантная структура — НЕ меняется)
    └─ d4c(x, f0)          → Aperiodicity[][] (дыхание/шум — НЕ меняется)
                                      │
              F0_target[] = Piano Roll target pitch (per-frame, Hz)
                                      │
              pyworld.synthesis(F0_target, SpectralEnv, Aperiodicity, fs)
                                      │
                      Resynthesized vocal — новый питч, оригинальный тембр
```

### Уровни качества (порядок предпочтения)

```
1. WORLD resynthesis   ← PRIMARY  (работает автоматически из клипа)
2. RVC resynthesis     ← Phase 2  (только для back vocals / voice conversion)
3. Phase vocoder       ← FALLBACK (если WORLD недоступен)
```

### Последствия для архитектуры

- Новый класс `WORLDResynthesizer` в `Source/dsp/` — обёртка вокруг `pyworld` через Python bridge.
- `applyOfflineCorrections` добавляет `WORLDResynthesizer*` как первый опциональный параметр.
- UI-баннер "No voice preset" убрать — WORLD не требует никакого пресета от пользователя.
- `RVCPythonBridge` и `loadVoicePreset` остаются в кодовой базе для back vocal generation (Phase 2).

### Риски

- `pyworld` — Python-зависимость. Требует embedded Python (уже в стеке). Производительность ~реалтайм для 1× на CPU.
- WORLD некорректно работает на сегментах короче ~100ms — минимальная длина сегмента должна проверяться до вызова.
- При очень большом сдвиге (>5 полутонов) WORLD может давать артефакты на согласных — нужна стратегия обработки транзиентов.

---

## ADR-008: DDSP + Neural Vocoder как основной synthesis engine (замена WORLD)

- **Дата:** 2026-04-14
- **Проблема:** WORLD vocoder при коррекции питча восстанавливает аудио через математическую модель source-filter, что даёт фазовую некогерентность и характерную «роботизированность». Даже с Vocos post-processing WORLD вносит артефакты в спектральную огибающую (cheaptrick estimation), которые Vocos затем честно воспроизводит.
- **Решение:** Заменить WORLD synthesis на полностью дифференцируемый DDSP pipeline с нейронным вокодером:
  1. **F0 Encoder** — `torchcrepe` для точного определения питча (точнее harvest на вокале)
  2. **Timbre Encoder** — 1D CNN, обучается быть F0-инвариантным через Gradient Reversal Layer
  3. **Noise Encoder** — отдельно кодирует шум/сибиляты (не зависит от F0 — критично для сохранения согласных)
  4. **DDSP Harmonic Synth** — аддитивный синтез из обученных амплитуд гармоник
  5. **DDSP Noise Synth** — белый шум через learnable time-varying FIR filter
  6. **Vocos vocoder** — финальный синтез из mel (уже интегрирован, остаётся)

- **Обоснование:**
  - DDSP разделяет голос на computable harmonic + stochastic noise части — сибиляты остаются на оригинальной частоте при питч-коррекции
  - Disentangled `z_timbre` → формантная структура сохраняется автоматически (не через математическую оценку как в WORLD)
  - Подход проверен на vovious и аналогичных инструментах — побеждает WORLD на ±3 полутона

- **Последствия:**
  - Добавляется `research/` директория с PyTorch training кодом
  - Phase 0: validation spike на VocalSet (неделя) — go/no-go decision
  - Trained модели экспортируются в ONNX → используются через `ort` в плагине (Phase 4)
  - `world_bridge.py` остаётся как fallback пока DDSP модели не обучены
  - `WORLDResynthesizer` переименовывается в `NeuralSynthBridge` в Phase 4 (не сейчас)

- **Риски:**
  - DDSP на вокале может звучать синтетически без Vocos — решается Phase 0 spike
  - Adversarial disentanglement нестабилен — стартуем с λ_disent=0.01, spectral norm
  - ONNX export custom DDSP ops — пишем ops ONNX-совместимо с первого дня

---

## ADR-009: Research directory — PyTorch training, ONNX export, Rust inference

- **Дата:** 2026-04-14
- **Решение:** Добавить `research/` в монорепозиторий Synthetic Obsidian для training кода
- **Структура:**
  - `research/nvt/` — Python пакет с моделями, лоссами, training loop
  - `research/scripts/` — data prep, benchmark vs WORLD, ONNX export
  - `research/configs/` — Hydra конфиги
- **Обоснование:** Держим training код рядом с плагином — легче синхронизировать изменения модели с inference кодом
- **Последствия:** `research/` не включается в плагин build. Модели идут в `models/` (gitignored).

---

## ADR-010: Seed-VC как текущий backend для offline backing vocals

- **Дата:** 2026-04-17
- **Статус:** принято для MVP-spike / Standalone integration

### Контекст

После нескольких итераций прослушивания WORLD, DDSP autoencoder и DDSP/STFT noise synth результаты остались недостаточно натуральными:

- WORLD звучал слишком vocoder-like даже после экспериментов с post-processing.
- DDSP стал лучше после исправлений кликов, но давал synthetic coloration, distortion и нестабильный `female_straight_recon`.
- Дополнительный тренинг DDSP снижал часть артефактов, но не вывел качество на уровень коммерческого reference.

Seed-VC zero-shot spike на пользовательском вокале дал лучший субъективный результат: натуральнее WORLD/DDSP и пригодно для генерации бэк-вокалов. Пользователь оценил результат как "огонь" и "не хуже чем у audimee"; мягкий `headroom` polish выбран вместо агрессивного repair, потому что repair усиливал ощущение clipping/distortion.

### Решение

Для текущего MVP использовать Seed-VC как offline/background backend для кнопки `AI BACKS`.

Текущий UI flow:

1. Пользователь выбирает трек с вокалом.
2. Нажимает `AI BACKS` в transport bar.
3. `SyntheticObsidianProcessor::renderSeedVCBackVocals()` запускает Seed-VC в background thread.
4. `SeedVCBridge` вызывает research runner через Python subprocess.
5. Генерируются интервалы `+3`, `+4`, `+7`, `+12`.
6. Результаты проходят `polish_outputs.py --mode headroom --target-peak-db -3.0`.
7. UI добавляет готовые WAV как `Back Vox` tracks с preset name `Seed-VC`.

### Реализация

- C++ bridge: `Source/ai/SeedVCBridge.*`
- UI entry point: `TransportBar` button `AI BACKS`
- Editor wiring: `PluginEditor.cpp`, callback `transportBar_.onRenderBacks`
- Track insertion: `TrackManagerPanel::addGeneratedAudioTrack`
- Python runner: `research/svc_pitch/run_seed_vc_file.py`
- Polish script: `research/svc_pitch/polish_outputs.py`
- Seed-VC checkout: `third_party/seed-vc`
- Python environment: `research/.venv_seed_vc`
- Output root: `~/Music/Synthetic Obsidian/SeedVC/<source>_<timestamp>/`
  - raw Seed-VC files: `raw/`
  - selected output used by UI: `headroom_only/`

### Important implementation notes

- Python/Seed-VC must never run on the realtime audio thread.
- `SeedVCBridge::render()` is blocking by design and must only be called from a background thread.
- `juce::ChildProcess` must be started with `juce::StringArray` arguments, not a manually quoted command string. Manual quoting caused Python to receive paths like `//"/Users/.../run_seed_vc_file.py"` and fail with `Errno 2`.
- `polish_outputs.py` must use `soundfile.read/write` for WAV I/O. `torchaudio.load()` in the current environment can route through `torchcodec` and fail inside the plugin-launched subprocess.
- The selected polish mode is `headroom`: DC removal, short edge fades, and peak headroom only. The stronger `repair` mode exists for experiments but sounded more clipped/distorted on the current vocal tests.

### Consequences

- MVP can generate useful backing vocals now, before a productized ONNX inference stack exists.
- Current integration is research-backed and local-machine specific; packaging still needs a production plan for model weights, Python runtime, and cross-platform paths.
- The audio/AI boundary remains preserved: generated audio is rendered offline and loaded back into the normal track playback path.
- Future product path can either keep Seed-VC as a bundled subprocess backend or replace it with an exported model/runtime once quality and licensing are settled.
