# Vocal Annotation Tool — план и техническое задание

## Цель

Сделать отдельный инструмент для подготовки качественного датасета вокальной разметки. Инструмент должен помогать быстро создавать и править эталонные note blocks, syllable boundaries, pitch curves, slides, lyrics, BPM и key для обучения гибридной модели vocal note detection.

Главная идея: автоматический анализ дает стартовую разметку, человек быстро доводит ее до музыкально правильного результата.

## Почему нужен отдельный инструмент

Текущая модель плохо определяет границы нот, потому что MIDI/WAV пары дают шумные labels:

- MIDI не совпадает с реальной атакой вокала.
- Согласные, слайды, вибрато и мелизмы не укладываются в простые MIDI-ноты.
- Начало и конец ноты часто субъективны: музыкальная граница, voiced boundary и syllable boundary могут быть разными моментами.
- Lyrics/ASR и syllable heuristics пока дают ложные разрезы и не должны автоматически менять note blocks без ручной проверки.

Поэтому нужен gold dataset, размеченный так, как ноты должны выглядеть в редакторе.

## Scope MVP

MVP должен позволять за один проход открыть вокальный WAV, получить авторазметку, руками исправить ноты и экспортировать JSON.

### Обязательные функции

- Загрузка WAV/AIFF/FLAC.
- Автоматическая первичная детекция:
  - note blocks;
  - pitch curve;
  - possible syllable boundaries;
  - possible slides/drift/vibrato regions;
  - BPM/key estimate.
- Редактирование нот:
  - двигать note block по времени;
  - менять длительность start/end handles;
  - двигать по pitch;
  - разрезать note block;
  - склеивать соседние note blocks;
  - удалить note block;
  - создать note block вручную.
- Редактирование syllable boundaries:
  - добавить boundary marker;
  - удалить marker;
  - сдвинуть marker;
  - назначить тип: `syllable`, `rearticulation`, `breath`, `noise`, `ignore`.
- Lyrics:
  - ручной ввод текста по словам/слогам;
  - привязка текста к note block или boundary marker;
  - ASR может предлагать черновик, но не считается эталоном.
- Waveform background:
  - крупная waveform под нотами;
  - waveform должна быть хорошо видна при zoom;
  - note regions подсвечиваются разными цветами поверх waveform, чтобы визуально видеть границы.
- Навигация:
  - horizontal zoom/scroll;
  - vertical pitch zoom/scroll;
  - fit to full clip;
  - jump to next/previous suspicious region.
- BPM/key:
  - автоматическое определение;
  - ручное изменение BPM;
  - ручное изменение key/scale;
  - сохранение выбранных значений в annotation JSON.
- Экспорт:
  - JSON annotation рядом с аудио;
  - optional MIDI export для проверки;
  - project autosave.

### Не входит в MVP

- Полноценное обучение модели внутри annotator.
- Realtime audio plugin behavior.
- ARA2/DAW integration.
- Автоматически идеальный lyrics transcription.
- Многодорожечная аранжировка.

## Рекомендуемый формат разметки

Один JSON на один аудиофайл:

```json
{
  "version": 1,
  "audio": "Sample01.wav",
  "sample_rate": 48000,
  "duration": 7.82,
  "bpm": 120.0,
  "key": "C minor",
  "notes": [
    {
      "id": "n001",
      "start": 1.240,
      "end": 1.680,
      "pitch": 64,
      "gain_db": 0.0,
      "voiced_start": 1.290,
      "voiced_end": 1.650,
      "lyric": "love",
      "flags": ["main"],
      "curve": [
        { "time": 1.290, "midi": 63.8, "confidence": 0.91 },
        { "time": 1.315, "midi": 64.1, "confidence": 0.94 }
      ]
    }
  ],
  "boundaries": [
    {
      "id": "b001",
      "time": 1.240,
      "kind": "syllable",
      "text": "love",
      "confidence": 1.0
    }
  ],
  "regions": [
    {
      "start": 1.180,
      "end": 1.760,
      "kind": "slide_in",
      "note_id": "n001"
    }
  ]
}
```

## Семантика labels

### Note block

Note block должен отражать то, что пользователь хочет видеть и редактировать как музыкальную ноту.

- `start` / `end`: музыкальные границы блока.
- `pitch`: основная нота блока.
- `gain_db`: локальная громкость note block в диапазоне `-24..+12 dB`; для старых файлов по умолчанию `0 dB`.
- `voiced_start` / `voiced_end`: участок, где реально звучит voiced вокал. Может быть уже, чем note block.
- `curve`: реальная pitch curve внутри или рядом с note block.

### Syllable boundary

Boundary marker не обязан автоматически разрезать note block. Он говорит модели: "в этой точке есть артикуляционная/ритмическая граница".

Типы:

- `syllable`: новый слог.
- `rearticulation`: повторная атака на той же гласной или той же ноте.
- `breath`: вдох.
- `noise`: шум/согласная без полезного pitch.
- `ignore`: место не использовать для обучения boundary.

### Slides, vibrato, drift

Slides и vibrato не должны автоматически превращаться в отдельные note blocks.

- `slide_in`: вход в основную ноту.
- `slide_out`: выход из основной ноты.
- `drift`: медленное отклонение pitch.
- `vibrato`: периодическое отклонение вокруг основной ноты.
- `melisma`: намеренная быстрая вокальная фигура, которую можно оставить как curve или отдельные note blocks по решению разметчика.

## UI

### Основная рабочая область

- Сверху timeline с bars/beats и seconds.
- Основной слой: крупная waveform.
- Поверх waveform: note blocks.
- Поверх note blocks: pitch curve.
- Вертикальные markers для syllable/rearticulation.
- Lyrics lane снизу или прямо внутри note blocks, если хватает места.
- Piano keyboard слева.

### Pitch Editor tools

- `Pointer`: выбор, Shift-мультивыбор и перемещение note blocks по времени и pitch.
- `Pencil`: создание note block; drag задаёт длительность.
- `Eraser`: удаление note block.
- `Scissors`: разрез note block в позиции курсора.
- `Join`: объединение выбранных note blocks или текущего блока со следующим.
- `Flex`: изменение длительности note block с пропорциональным растяжением pitch curve.
- `Vibrato`: масштабирование отклонений pitch curve относительно основной ноты.
- `Gain`: локальная громкость note block (`-24..+12 dB`).
- `Zoom`: увеличение по позиции курсора; right-click или Alt-click уменьшает масштаб.

Для левой и правой кнопки мыши инструмент выбирается независимо. Pitch Editor ведёт отдельные undo/redo stacks для `Voice Main` и каждой backing vocal дорожки.

После правки backing vocal с уже готовым аудио Pitch Editor показывает `Re-render` и в фоне создаёт временный алгоритмический pitch-preview из последнего полноценного рендера. Если аудио ещё нет, показывается `Render`. Последнее состояние нот, использованное для полноценного рендера, сохраняется в `rendered_notes`, поэтому preview остаётся корректным после повторного открытия проекта. Финальный render по-прежнему выполняется вне audio thread.

### Визуальные правила

- Note blocks разных pitch или соседние regions подсвечиваются разными цветами.
- Активная нота имеет яркую рамку.
- `voiced_start` / `voiced_end` можно показывать более темной внутренней полосой внутри note block.
- Pitch curve рисуется белой или светлой линией поверх блока.
- Syllable markers рисуются вертикальными линиями, но не должны выглядеть как окончательная граница ноты, если note не разрезана.
- Suspicious regions подсвечиваются отдельным цветом:
  - низкая confidence;
  - слишком длинная нота с несколькими energy peaks;
  - note boundary далеко от waveform onset;
  - резкий pitch jump без note split.

## Инструменты редактирования

### Mouse

- Drag body: двигать note block по pitch.
- Shift + drag body: двигать note block по времени.
- Drag left/right edge: менять start/end.
- Double click empty area: создать note.
- Double click note: открыть быстрый editor.
- Alt/Option + click note: split at cursor.
- Select two neighboring notes + merge command: склеить.
- Drag boundary marker: сдвинуть marker.
- Double click waveform: добавить boundary marker.

### Keyboard

- Space: play/stop.
- Backspace/Delete: удалить выделенное.
- Cmd+Z / Cmd+Shift+Z: undo/redo.
- Cmd+S: save annotation.
- S: split selected note at playhead.
- M: merge selected notes.
- Arrow Up/Down: move pitch by semitone.
- Shift + Arrow Up/Down: move pitch by octave.
- Arrow Left/Right: nudge time.
- Tab / Shift+Tab: next/previous suspicious region.

## Автоматический анализ

### Baseline

Использовать текущий рабочий путь:

- `librosa.pyin` для F0.
- note merge heuristics.
- pitch curve export.
- confidence per note.

### Дополнительные черновые слои

Эти слои должны быть подсказками, а не истиной:

- onset candidates from RMS/spectral flux;
- syllable boundary candidates;
- slide/vibrato candidate regions;
- BPM estimate;
- key estimate;
- ASR word candidates.

Все candidate layers должны быть отключаемыми в UI.

## Data quality workflow

1. Annotator открывает аудио.
2. Программа строит авторазметку.
3. Annotator правит note blocks.
4. Annotator проверяет pitch curve и основной pitch.
5. Annotator расставляет или исправляет syllable boundaries.
6. Annotator вписывает lyrics только там, где текст известен.
7. Annotator проходит suspicious regions.
8. Annotator сохраняет JSON.
9. Validation script проверяет:
   - нет overlapping note blocks без явного флага;
   - `end > start`;
   - pitch в допустимом диапазоне;
   - boundaries внутри duration;
   - curve points отсортированы;
   - audio path существует.

## Техническая архитектура

Рекомендуемый путь для MVP: отдельный offline annotator mode в standalone-приложении или отдельный JUCE standalone target, который переиспользует:

- waveform rendering;
- piano roll rendering;
- текущий Python analysis subprocess;
- JSON import/export.

Важное ограничение: annotator не должен попадать в realtime audio path плагина. Python, ASR, BPM/key analysis и model inference выполняются только offline/background.

### Модули

```text
Source/
  annotation/
    AnnotationDocument.h/.cpp
    AnnotationJson.h/.cpp
    AnnotationValidator.h/.cpp
    AnnotationAnalysisBridge.h/.cpp
  ui/
    AnnotationEditor.h/.cpp
    WaveformLane.h/.cpp
    NoteBlockLayer.h/.cpp
    BoundaryMarkerLayer.h/.cpp
    LyricsLane.h/.cpp
```

### Python scripts

```text
research/svc_pitch/
  analyze_notes_pyin.py        # existing baseline
  analyze_boundaries.py        # future candidate boundary layer
  analyze_bpm_key.py           # future BPM/key estimate
```

## Milestones

### Milestone 1 — JSON + import/export

- Define `AnnotationDocument`.
- Load/save JSON.
- Add validation script.
- Convert current auto notes to annotation JSON.

### Milestone 2 — Viewer

- Load audio.
- Draw large waveform.
- Draw note blocks.
- Draw pitch curve.
- Horizontal zoom/scroll.
- Vertical pitch zoom/scroll.

### Milestone 3 — Editing

- Move notes by time/pitch.
- Resize start/end.
- Split/merge/delete/create.
- Undo/redo.
- Save.

### Milestone 4 — Boundaries + lyrics

- Draw boundary markers.
- Add/move/delete markers.
- Add lyrics lane.
- Manual lyrics editing.

### Milestone 5 — Analysis assists

- Run auto detection.
- Show candidate boundaries/slides/vibrato.
- Show suspicious regions.
- Manual BPM/key override.

### Milestone 6 — Dataset export

- Batch export annotations.
- Export training manifests.
- Generate frame-level targets:
  - voiced;
  - pitch;
  - onset heatmap;
  - offset heatmap;
  - note-change heatmap;
  - syllable-boundary heatmap.

## Definition of Done

- Annotator can create a corrected JSON for one vocal file without editing raw files manually.
- Notes can be visually aligned to waveform.
- Pitch curve remains visible inside note blocks.
- Syllable boundaries and lyrics can be manually corrected.
- BPM/key can be auto-estimated and manually overridden.
- Output JSON passes validation.
- The tool does not introduce Python/ML work into the plugin audio thread.
