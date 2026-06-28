# Спецификация дизайна: Synthetic Obsidian VST3/ARA2 Плагин

**Версия:** 1.0  
**Дата:** 2026-04-11  
**Язык:** Русский  
**Статус:** Финальная спецификация

---

## 1. Система дизайна

### 1.1. Цветовая палитра

#### Основные цвета

| Название | Hex код | RGB | Использование |
|----------|---------|-----|----------------|
| Background | #0a0a12 | 10, 10, 18 | Основной фон приложения |
| Surface | #0f0f1a | 15, 15, 26 | Панели, контейнеры |
| Accent Cyan | #00d4c8 | 0, 212, 200 | Активные элементы, акценты |
| Accent Purple | #8b5cf6 | 139, 92, 246 | Альтернативные элементы |
| Text Primary | #e2e8f0 | 226, 232, 240 | Основной текст |
| Text Secondary | #64748b | 100, 116, 139 | Вторичный текст, метки |
| Success Green | #22c55e | 34, 197, 94 | Индикаторы статуса, активные состояния |
| Border Default | rgba(255,255,255,0.08) | - | Границы элементов |
| Button Ghost BG | rgba(255,255,255,0.04) | - | Фон неактивных кнопок |

#### Расширенная палитра

| Элемент | Hex код | Примечание |
|---------|---------|-----------|
| Disabled Text | #475569 | Отключенные элементы |
| Success Highlight | #16a34a | Пульсирующие индикаторы |
| Warning | #f59e0b | Оповещения и предупреждения |
| Error | #ef4444 | Ошибки и критические события |
| Piano Roll Grid | rgba(255,255,255,0.02) | Сетка пианолы |
| Waveform | rgba(0,212,200,0.3) | Отображение волны |

### 1.2. Типография

#### Шрифтовое семейство
- **Основной:** Inter (Regular, Medium, Semi Bold)
- **Загрузка:** Встроена как BinaryData в JUCE проекте
- **Резервный:** Система шрифты (если Inter недоступен)

#### Размеры и стили

| Назначение | Размер | Вес | Межстрочный | Использование |
|-----------|--------|-----|-------------|----------------|
| Logo/Title | 20px | Semi Bold | 28px | Заголовок приложения |
| Header | 16px | Semi Bold | 24px | Заголовки секций |
| Body | 13px | Regular | 20px | Основной текст |
| Label | 11px | Regular | 16px | Метки, подписи |
| Small | 10px | Regular | 14px | Дополнительная информация |
| Code | 11px | Regular | 16px | Параметры, значения |

#### Интервалы букв
- Заголовки: 0px (стандарт)
- Основной текст: 0px
- Метки: 0.5px

### 1.3. Сетка и отступы (Grid: 8px)

#### Основная сетка
```
Базовая единица: 8px
Отступы контейнеров: 16px (2x8)
Отступы элементов: 8px (1x8)
Отступы между группами: 12px (1.5x8)
Радиусы скругления: 4px, 6px, 8px
```

#### Типичные размеры отступов

| Область | Значение | Комментарий |
|---------|----------|-----------|
| Container Padding | 16px | Вокруг основного контента |
| Panel Gap | 8px | Между элементами в панели |
| Component Gap | 4px | Между компонентами в группе |
| Section Gap | 12px | Между логическими секциями |
| Row Height | 32px | Высота строк списков |
| Small Gap | 4px | Микро отступы |

### 1.4. Теневые эффекты

```
Elevation 1 (тонкая тень):
  box-shadow: 0px 1px 2px rgba(0,0,0,0.3)

Elevation 2 (средняя тень):
  box-shadow: 0px 4px 8px rgba(0,0,0,0.4)

Elevation 3 (глубокая тень):
  box-shadow: 0px 8px 16px rgba(0,0,0,0.5)
```

---

## 2. UI Компоненты и состояния

### 2.1. TrackRow (Строка трека)

**Размеры:**
- Высота: 32px
- Ширина: 250px (левая панель)
- Отступы: 8px

**Состояния:**

#### Normal (Обычное)
- Background: #0f0f1a
- Text: #e2e8f0
- Border: rgba(255,255,255,0.04)

#### Hover (Наведение)
- Background: rgba(255,255,255,0.02)
- Text: #e2e8f0
- Border: rgba(255,255,255,0.08)
- Transition: 200ms ease-out

#### Selected (Выбрана)
- Background: rgba(0,212,200,0.15)
- Text: #00d4c8
- Border: 1px solid rgba(0,212,200,0.3)
- Left indicator bar: 3px solid #00d4c8

#### Disabled (Отключена)
- Background: #0a0a12
- Text: #475569
- Border: rgba(255,255,255,0.02)

**Компоненты строки:**
```
[Icon: 16x16] [Track Name: 14px] [Spacer] [Solo: 24x24] [Mute: 24x24]
|-- 8px --|-- 8px --|-- flex --|-- 4px --|-- 4px --|
```

**Типовые треки:**
- Guide (Гайд)
- Main Vox (Основной вокал)
- Back 1 (Бэк 1)
- Back 2 (Бэк 2)

### 2.2. S/M Toggle Buttons (Solo/Mute)

**Размеры:**
- Ширина: 24px
- Высота: 24px
- Иконка внутри: 14x14px

**Цвета иконок:**

#### Solo Button

| Состояние | Background | Icon Color | Border |
|-----------|-----------|-----------|--------|
| Off (Normal) | rgba(255,255,255,0.04) | #64748b | rgba(255,255,255,0.08) |
| Hover | rgba(255,255,255,0.08) | #e2e8f0 | rgba(255,255,255,0.12) |
| Active | #8b5cf6 | #ffffff | 1px solid #8b5cf6 |
| Disabled | rgba(255,255,255,0.02) | #475569 | rgba(255,255,255,0.04) |

#### Mute Button

| Состояние | Background | Icon Color | Border |
|-----------|-----------|-----------|--------|
| Off (Normal) | rgba(255,255,255,0.04) | #64748b | rgba(255,255,255,0.08) |
| Hover | rgba(255,255,255,0.08) | #e2e8f0 | rgba(255,255,255,0.12) |
| Active | #ef4444 | #ffffff | 1px solid #ef4444 |
| Disabled | rgba(255,255,255,0.02) | #475569 | rgba(255,255,255,0.04) |

**Переход:** 150ms ease-out

### 2.3. GhostButton (Контурная кнопка)

**Примеры:** "REPLACE GUIDE", "CAPTURE", "VOICE", "STYLE"

**Размеры:**
- Высота: 32px
- Padding: 8px 12px
- Min-width: 80px
- Border radius: 6px

**Состояния:**

#### Normal
- Background: rgba(255,255,255,0.04)
- Text: #e2e8f0 (13px Semi Bold)
- Border: 1px solid rgba(255,255,255,0.12)
- Cursor: pointer

#### Hover
- Background: rgba(255,255,255,0.08)
- Text: #ffffff
- Border: 1px solid rgba(255,255,255,0.2)
- Box-shadow: 0px 0px 8px rgba(0,0,0,0.3)
- Transition: 150ms ease-out

#### Active
- Background: rgba(255,255,255,0.12)
- Text: #00d4c8
- Border: 1px solid #00d4c8
- Box-shadow: 0px 0px 12px rgba(0,212,200,0.2)

#### Disabled
- Background: rgba(255,255,255,0.02)
- Text: #475569
- Border: 1px solid rgba(255,255,255,0.04)
- Cursor: not-allowed
- Opacity: 0.5

### 2.4. PrimaryButton (Основная кнопка)

**Примеры:** "RENDER VOCAL", "CAPTURE" (когда активна)

**Размеры:**
- Высота: 36px
- Padding: 8px 16px
- Min-width: 120px
- Border radius: 6px

**Состояния:**

#### Normal
- Background: #00d4c8
- Text: #0a0a12 (13px Semi Bold)
- Border: none
- Cursor: pointer
- Box-shadow: 0px 2px 8px rgba(0,212,200,0.2)

#### Hover
- Background: #00e6d8
- Text: #0a0a12
- Box-shadow: 0px 4px 12px rgba(0,212,200,0.3)
- Transform: translateY(-2px)
- Transition: 150ms ease-out

#### Active
- Background: #00c4b8
- Text: #0a0a12
- Box-shadow: 0px 1px 4px rgba(0,212,200,0.2)
- Transform: translateY(0px)

#### Disabled
- Background: #64748b
- Text: #475569
- Border: none
- Cursor: not-allowed
- Box-shadow: none
- Opacity: 0.5

#### Secondary (Purple variant)
- Background: #8b5cf6
- Text: #ffffff
- Box-shadow: 0px 2px 8px rgba(139,92,246,0.2)

### 2.5. StatusBadge (Статус-бейдж)

**Пример:** "● ARA2 ACTIVE"

**Размеры:**
- Высота: 24px
- Padding: 4px 8px
- Border radius: 12px

**Компоненты:**
```
[● Indicator: 6px] [Text: 11px Regular] [Status Info]
```

#### ARA2 Active State
- Background: rgba(34,197,94,0.15)
- Text: #22c55e (11px Regular)
- Indicator: 6px solid circle #22c55e
- Border: 1px solid rgba(34,197,94,0.3)
- Animation: пульсирующий свет (0.5s, opacity 1→0.6)

#### Processing State
- Background: rgba(139,92,246,0.15)
- Text: #8b5cf6
- Indicator: 6px solid circle #8b5cf6 (вращается)
- Border: 1px solid rgba(139,92,246,0.3)

#### Error State
- Background: rgba(239,68,68,0.15)
- Text: #ef4444
- Indicator: 6px solid circle #ef4444
- Border: 1px solid rgba(239,68,68,0.3)

### 2.6. RotaryKnob (Круглый энкодер)

**Использование:** PITCH DRIFT, AI INFLUENCE (альтернативный вид)

**Размеры:**
- Диаметр: 60px
- Внешний ring: 54px
- Внутренний circle: 40px
- Указатель (tick): 2px width, 12px length

**Визуальное отображение:**

```
Угловой диапазон: 225° до -45° (по часовой стрелке)
Min angle: 225° (7:30 o'clock)
Max angle: -45° (3:00 o'clock)
Total span: 270°
```

**Состояния:**

#### Normal
- Background circle: #0f0f1a
- Ring stroke: rgba(255,255,255,0.12)
- Ring width: 2px
- Indicator: #00d4c8
- Text (center): 11px Regular #e2e8f0

#### Hover
- Ring stroke: rgba(255,255,255,0.2)
- Indicator: #00e6d8
- Cursor: pointer

#### Active (Dragging)
- Ring stroke: #00d4c8
- Ring width: 3px
- Indicator: #ffffff
- Box-shadow: 0px 0px 12px rgba(0,212,200,0.3)

#### Disabled
- Ring stroke: rgba(255,255,255,0.04)
- Indicator: #475569
- Opacity: 0.5

**Жесты:**
- Vertical drag: ±1-2px = 1 unit change
- Ctrl+Click (macOS Cmd+Click): Точное значение
- Double-click: Сброс к default значению

### 2.7. HorizontalSlider (Горизонтальный слайдер)

**Использование:** FORMANT SHIFT, VIBRATO SCALE (основной вид)

**Размеры:**
- Ширина: 200px (variable)
- Высота слайда: 4px
- Thumb размер: 16x24px
- Border radius (thumb): 4px

**Состояния:**

#### Normal
- Track (unfilled): rgba(255,255,255,0.08)
- Track (filled): #00d4c8
- Thumb: #00d4c8
- Text label: 11px Regular #64748b
- Value text: 13px Regular #e2e8f0

#### Hover
- Track (filled): #00e6d8
- Thumb: #00e6d8
- Thumb box-shadow: 0px 2px 8px rgba(0,212,200,0.2)
- Cursor: pointer

#### Active (Dragging)
- Track (filled): #00c4b8
- Thumb: #00c4b8
- Thumb box-shadow: 0px 4px 12px rgba(0,212,200,0.3)

#### Disabled
- Track (unfilled): rgba(255,255,255,0.04)
- Track (filled): #64748b
- Thumb: #64748b
- Opacity: 0.5

### 2.8. CircularMeter (Круглый индикатор)

**Использование:** AI INFLUENCE (первичный вид)

**Размеры:**
- Диаметр: 80px
- Внешний ring: 74px
- Текст: 20px Semi Bold center
- Label: 10px Regular below

**Визуальное отображение:**

```
Угловой диапазон: 180° до 180° (bottom arc)
Min angle: 180° (6:00 o'clock)
Max angle: 0° (12:00 o'clock)
Total span: 180°
```

**Компоненты:**
- Background ring: rgba(255,255,255,0.08), width 3px
- Filled arc: #00d4c8 → #8b5cf6 (gradient), width 3px
- Center text: percentage value (0-100%)
- Label below: "AI INFLUENCE"

**Анимация:**
- Пульсирующее свечение при изменении (150ms ease-out)
- Smooth arc fill (300ms ease-out)
- Пульсирующий ореол при AI processing (1s infinite)

### 2.9. SegmentedSlider (Сегментированный слайдер)

**Использование:** VIBRATO SCALE, режимные селекторы

**Размеры:**
- Высота: 32px
- Ширина: variable (flex)
- Segment padding: 4px
- Border radius container: 6px

**Структура:**

```
[Segment 1] [Segment 2] [Segment 3] [...]
```

**Состояния сегмента:**

#### Unselected
- Background: rgba(255,255,255,0.04)
- Text: #64748b (11px Regular)
- Border: none
- Cursor: pointer

#### Hover (Unselected)
- Background: rgba(255,255,255,0.08)
- Text: #e2e8f0

#### Selected
- Background: #00d4c8
- Text: #0a0a12 (11px Semi Bold)
- Box-shadow: 0px 2px 8px rgba(0,212,200,0.2)

#### Disabled
- Background: rgba(255,255,255,0.02)
- Text: #475569
- Opacity: 0.5

### 2.10. TextInput (Текстовое поле)

**Использование:** Названия треков, пресеты

**Размеры:**
- Высота: 32px
- Padding: 8px 12px
- Border radius: 4px
- Font: 13px Regular

**Состояния:**

#### Empty
- Background: rgba(255,255,255,0.04)
- Border: 1px solid rgba(255,255,255,0.08)
- Text: #64748b (placeholder)
- Cursor: text

#### Focused
- Background: rgba(255,255,255,0.06)
- Border: 1px solid #00d4c8
- Box-shadow: 0px 0px 8px rgba(0,212,200,0.15)
- Text: #e2e8f0

#### Filled
- Background: rgba(255,255,255,0.06)
- Border: 1px solid rgba(255,255,255,0.12)
- Text: #e2e8f0

#### Disabled
- Background: rgba(255,255,255,0.02)
- Border: 1px solid rgba(255,255,255,0.04)
- Text: #475569
- Opacity: 0.5

---

## 3. Макет главного окна (Main Layout)

### 3.1. Общая структура

**Размер окна:** 1024px × 800px (фиксированный или ограниченный resize)

```
┌─────────────────────────────────────────────────────────────────┐
│ Header (50px)                                                   │
├────────────────┬──────────────────────────────────────────────┤
│                │                                              │
│  Left Panel    │            Center Area                       │
│  Track Manager │            (Piano Roll)                      │
│  (250px)       │                                              │
│                │                                              │
├────────────────┼──────────────────────────────────────────────┤
│ Bottom Parameters Panel (100px)                               │
├─────────────────────────────────────────────────────────────────┤
│ Transport Bar (60px)                                          │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2. Header (50px)

**Компоненты слева направо:**

```
[Logo & Title: 20px] [Spacer: flex] [Status Badge] [Settings Icon]
|-- 16px --|-- variable --|-- 8px --|-- 8px --|-- 16px --|
```

**Состояние:**
- Background: #0a0a12
- Border-bottom: 1px solid rgba(255,255,255,0.08)
- Vertical align: center

**Элементы:**
1. Logo text "Synthetic Obsidian" (20px Semi Bold)
2. Status badge "● ARA2 ACTIVE" (22px)
3. Settings icon button (24x24px)

### 3.3. Left Panel - Track Manager (250px width)

**Структура:**

```
┌──────────────────────────┐
│ Tracks List              │ (header: 32px, label)
├──────────────────────────┤
│ ▼ Guide                  │ (32px height)
│ ▼ Main Vox               │ (32px height)
│ ▼ Back 1                 │ (32px height)
│ ▼ Back 2                 │ (32px height)
├──────────────────────────┤
│ [+ Add Track Button]     │ (32px height)
└──────────────────────────┘
```

**Стили:**

- Background: #0f0f1a
- Border-right: 1px solid rgba(255,255,255,0.08)
- Panel padding: 12px
- Track row height: 32px
- Gap between rows: 4px

**Взаимодействие:**
- Click track: выбрать трек
- Right-click: контекстное меню (удалить, дублировать, изменить имя)
- Drag: переупорядочение треков
- Solo/Mute buttons: как описано в 2.2

### 3.4. Center Area - Piano Roll

**Верхняя часть:** Временная шкала (Timeline, 32px)

```
[Grid ruler with time markers: 0.0s, 0.5s, 1.0s, ...]
```

- Background: #0f0f1a
- Text: 10px Regular #64748b
- Markers: каждые 0.5s
- Grid lines: rgba(255,255,255,0.04)

**Основная зона:**

```
┌─────────────────────────────────────────────┐
│  Piano Roll Container                       │
│  ┌───────────────────────────────────────┐  │
│  │ [Waveform Background & Grid]          │  │
│  │ [Note Blocks: cyan & purple]          │  │
│  │ [Playhead: red/cyan line]             │  │
│  │ [Cursor indicators]                   │  │
│  └───────────────────────────────────────┘  │
│                                             │
│  [Vertical Scrollbar: right side, 12px]    │
│  [Horizontal Scrollbar: bottom, 12px]      │
└─────────────────────────────────────────────┘
```

**Размеры:**
- Available width: 1024 - 250 - 16 = 758px
- Available height: 800 - 50 - 100 - 60 = 590px
- Scrollbar width/height: 12px

**Визуальные элементы:**

#### Grid & Background
- Grid lines: rgba(255,255,255,0.02), vertical every 1/4 note
- Horizontal lines: rgba(255,255,255,0.02), every semitone
- Background: #0a0a12 with subtle pattern

#### Waveform Display
- Color: rgba(0,212,200,0.3)
- Stroke width: 1px
- Opacity when not playing: 0.5
- Opacity when playing: 0.8

#### Note Blocks

**Main Vocal Notes (Guide):**
- Fill: #00d4c8 (cyan)
- Border: 1px solid #00b8ae
- Height: variable (per semitone)
- Min width: 8px
- Corner radius: 3px
- Padding inside: 2px

**Backing Vocal Notes:**
- Fill: #8b5cf6 (purple)
- Border: 1px solid #7c3aed
- Height: variable
- Min width: 6px
- Corner radius: 3px

**Note Hover State:**
- Brightness: +10%
- Cursor: pointer
- Outline: 1px solid rgba(255,255,255,0.2)

**Note Selected State:**
- Outline: 2px solid #00d4c8
- Box-shadow: 0px 0px 8px rgba(0,212,200,0.3)

#### Playhead (Current Position Indicator)
- Width: 2px
- Color: #ef4444 (red) or #00d4c8 (cyan when paused)
- Height: full piano roll height
- Animation: smooth movement during playback

#### Cursor Tooltip
- Background: rgba(0,0,0,0.8)
- Text: 10px Regular #e2e8f0
- Padding: 4px 8px
- Border radius: 3px
- Appears on hover above playhead

### 3.5. Bottom Parameters Panel (100px)

**Раскладка 2x2 сетка:**

```
┌──────────────────────────────────────────────────────────────┐
│ [PITCH DRIFT]        [FORMANT SHIFT]                        │
│ [Knob: 60px]        [Slider: 200px]                         │
│ Label + Value       Label + Value                            │
├──────────────────────────────────────────────────────────────┤
│ [AI INFLUENCE]      [VIBRATO SCALE]                         │
│ [Meter: 80px]       [Segmented: 180px]                      │
│ Label + Percentage  Label + Segments                         │
└──────────────────────────────────────────────────────────────┘
```

**Стили:**
- Background: #0f0f1a
- Border-top: 1px solid rgba(255,255,255,0.08)
- Padding: 12px 16px
- Grid gap: 16px

**Элементы (слева направо):**

1. **PITCH DRIFT** (левый верх)
   - Label: 11px Regular #64748b
   - Component: RotaryKnob 60px
   - Value display: 13px Regular #e2e8f0 (e.g., "+2.5 semitones")
   - Range: -12 to +12 semitones
   - Default: 0

2. **FORMANT SHIFT** (правый верх)
   - Label: 11px Regular #64748b
   - Component: HorizontalSlider 200px
   - Value display: 13px Regular #e2e8f0 (e.g., "-150 cents")
   - Range: -500 to +500 cents
   - Default: 0

3. **AI INFLUENCE** (левый низ)
   - Label: 11px Regular #64748b
   - Component: CircularMeter 80px
   - Percentage display: 20px Semi Bold #00d4c8
   - Range: 0 to 100%
   - Default: 50%

4. **VIBRATO SCALE** (правый низ)
   - Label: 11px Regular #64748b
   - Component: SegmentedSlider with 4 segments
   - Options: "OFF", "LIGHT", "MEDIUM", "HEAVY"
   - Default: "MEDIUM"

### 3.6. Transport Bar (60px)

**Компоненты слева направо:**

```
[RESET] [PLAY] [STOP] [Spacer] [Tempo Display] [Spacer] [Help]
```

**Детали:**

```
┌─────────────────────────────────────────────────────────────┐
│ [Button: RESET]  [Button: PLAY]  [Button: STOP]             │
│    32x32px          36x36px          32x32px                │
│                                                             │
│                [Tempo: 120.0 BPM] [Time: 0:00.0]           │
│                        16px Regular                         │
└─────────────────────────────────────────────────────────────┘
```

**Кнопки:**

#### RESET Button
- Style: GhostButton 32x32px
- Icon: ↻ (rotate icon)
- Function: Вернуть playhead в начало, очистить undo/redo

#### PLAY Button
- Style: PrimaryButton 36x36px (highlighted when active)
- Icon: ▶ (play triangle)
- Active state: Background #00d4c8, pulsing glow
- Function: Начать воспроизведение

#### STOP Button
- Style: GhostButton 32x32px
- Icon: ■ (stop square)
- Function: Остановить воспроизведение

**Информационные элементы:**

- **Tempo Display:** "120.0 BPM" (16px Regular #e2e8f0)
  - Click to edit
  - Range: 40-200 BPM
  - Step: 0.1 BPM

- **Time Display:** "0:00.0" (16px Regular #e2e8f0)
  - Format: MM:SS.d (minutes:seconds.decisecond)
  - Read-only during playback

**Стили:**
- Background: #0a0a12
- Border-top: 1px solid rgba(255,255,255,0.08)
- Padding: 8px 16px
- Vertical align: center

---

## 4. Дополнительные экраны и диалоги

### 4.1. Voice Preset Creator Window (RVC Training)

**Размер окна:** 600px × 700px (resizable)

**Структура:**

```
┌────────────────────────────────────────────┐
│ Header: "Voice Preset Creator"             │
├────────────────────────────────────────────┤
│                                            │
│ Step 1: Upload Audio Files                │
│ ┌──────────────────────────────────────┐  │
│ │ [✓] Drag & drop audio files here    │  │
│ │ Supported: WAV, MP3, FLAC (max 10MB)│  │
│ │ [Browse Files Button]                │  │
│ └──────────────────────────────────────┘  │
│                                            │
│ Step 2: Configure Settings                │
│ ┌──────────────────────────────────────┐  │
│ │ Preset Name: [Text Input: ...]       │  │
│ │ Voice Type: [Dropdown: Singer/Vocal] │  │
│ │ Quality:    [Slider: Low→High]       │  │
│ └──────────────────────────────────────┘  │
│                                            │
│ Step 3: Training Progress                 │
│ ┌──────────────────────────────────────┐  │
│ │ Processing: ████████░░░░░░░░  75%   │  │
│ │ Time remaining: ~2 minutes 30 seconds│  │
│ │ [Cancel Training Button]              │  │
│ └──────────────────────────────────────┘  │
│                                            │
│ [Save & Close]  [Cancel]  [Help]         │
└────────────────────────────────────────────┘
```

**Цвета и стили:**

- Title: 16px Semi Bold #e2e8f0
- Labels: 11px Regular #64748b
- Separators: 1px solid rgba(255,255,255,0.08)
- Drop zone background: rgba(0,212,200,0.08)
- Drop zone border: 2px dashed #00d4c8

**Элементы:**

#### File Upload Zone
- Высота: 120px
- Border: 2px dashed rgba(0,212,200,0.4)
- Border radius: 8px
- Padding: 16px
- On hover: background rgba(0,212,200,0.12)
- Icon: ↓ (24px, #00d4c8)
- Text: "Drag & drop audio files here" (13px)

#### Text Inputs
- Высота: 36px
- Padding: 8px 12px
- Border radius: 4px
- Font: 13px Regular
- Placeholder: 11px #64748b

#### Dropdown Select
- Высота: 36px
- Padding: 8px 12px
- Background: rgba(255,255,255,0.04)
- Border: 1px solid rgba(255,255,255,0.08)
- Border radius: 4px
- Options: dropdown list with 200px width

#### Progress Bar
- Высота: 8px
- Background: rgba(255,255,255,0.08)
- Fill: linear gradient #00d4c8 → #8b5cf6
- Border radius: 4px
- Animation: smooth fill (no flickering)

#### Training Status Text
- Font: 11px Regular #64748b
- Margin: 8px 0 0 0

### 4.2. Style Picker Dialog (Back Vocals)

**Размер окна:** 500px × 400px (modal, center-aligned)

**Структура:**

```
┌──────────────────────────────────────────┐
│ "Select Back Vocal Style"                │
├──────────────────────────────────────────┤
│                                          │
│ Choose harmony style for [Back 1]        │
│                                          │
│ [O] Drone         [O] Thirds             │
│     (single note)      (3rd intervals)   │
│                                          │
│ [O] Fifths        [O] Octave             │
│     (5th intervals)    (1 octave up)     │
│                                          │
│ [O] Double Up     [O] Double Down        │
│     (2x octave up)    (2x octave down)   │
│                                          │
│ [Radio button selected]                 │
│ Description of selected style...        │
│                                          │
│ [Apply] [Cancel]                        │
└──────────────────────────────────────────┘
```

**Стили:**

- Dialog background: #0f0f1a
- Title: 16px Semi Bold #e2e8f0
- Instruction: 13px Regular #64748b
- Options grid: 2 columns, 16px gap

#### Radio Button Option

**Размеры:**
- Radio circle: 16px diameter
- Label text: 13px Regular
- Padding: 12px

**Состояния:**

| State | Circle | Label | Background |
|-------|--------|-------|-----------|
| Normal | circle #00d4c8, border rgba(255,255,255,0.2) | #e2e8f0 | transparent |
| Hover | circle #00d4c8, border #00d4c8 | #ffffff | rgba(0,212,200,0.08) |
| Selected | circle filled #00d4c8, inner dot | #ffffff | rgba(0,212,200,0.12) |

#### Description Box
- Background: rgba(255,255,255,0.04)
- Border: 1px solid rgba(255,255,255,0.08)
- Border radius: 6px
- Padding: 12px
- Font: 11px Regular #64748b
- Min-height: 40px

#### Buttons
- Apply: PrimaryButton (cyan, 36px height)
- Cancel: GhostButton (outline, 32px height)
- Gap: 8px

### 4.3. Settings Panel

**Размер окна:** 550px × 650px (resizable modal)

**Структура (Tab-based):**

```
┌──────────────────────────────────────────┐
│ "Settings"                               │
├──────────────────────────────────────────┤
│ [Audio] [Display] [Advanced]             │ (tab buttons, 80px each)
├──────────────────────────────────────────┤
│                                          │
│ === AUDIO TAB ===                        │
│                                          │
│ Audio Device:                            │
│ [Dropdown: Current Device ▼]             │
│                                          │
│ Buffer Size:                             │
│ [Dropdown: 256 samples ▼]                │
│ (values: 64, 128, 256, 512, 1024)       │
│                                          │
│ Sample Rate:                             │
│ [Display: 44.1 kHz (read-only)]         │
│                                          │
│ ☐ Enable Audio In                       │
│ ☐ Auto Gain Control                     │
│ ☐ Noise Gate (Threshold: [Slider])      │
│                                          │
├──────────────────────────────────────────┤
│ === DISPLAY TAB ===                      │
│                                          │
│ Theme:                                   │
│ ○ Dark (selected)  ○ Light               │
│                                          │
│ UI Scale:                                │
│ [Slider: 75% ←|→ 150%]                  │
│                                          │
│ ☐ Show Tooltips                         │
│ ☐ High Contrast Mode                    │
│                                          │
├──────────────────────────────────────────┤
│ === ADVANCED TAB ===                     │
│                                          │
│ GPU Acceleration:                        │
│ ☐ Enable (if available)                 │
│                                          │
│ Thread Pool Size:                        │
│ [Dropdown: Auto ▼] (2, 4, 8, Auto)      │
│                                          │
│ Debug Mode:                              │
│ ☐ Log to File                           │
│ ☐ Verbose Output                        │
│                                          │
├──────────────────────────────────────────┤
│ [Reset to Defaults]  [OK]  [Cancel]    │
└──────────────────────────────────────────┘
```

**Стили:**

#### Tab Buttons
- Height: 40px
- Width: auto, min 80px
- Padding: 8px 16px
- Font: 13px Regular
- Border-bottom: 2px solid transparent
- Border-bottom on active: 2px solid #00d4c8
- Background: transparent
- Color on active: #00d4c8

#### Checkbox Elements
- Size: 18x18px
- Corner radius: 3px
- Border: 1px solid rgba(255,255,255,0.12)
- When checked: background #00d4c8, inner checkmark white
- Label margin: 8px left

#### Dropdowns (Settings)
- Height: 36px
- Padding: 8px 12px
- Background: rgba(255,255,255,0.04)
- Border: 1px solid rgba(255,255,255,0.08)
- Border radius: 4px
- Font: 13px Regular

#### Section Dividers
- Margin: 16px 0
- Border-top: 1px solid rgba(255,255,255,0.08)

---

## 5. JUCE 8 реализация

### 5.1. Архитектура LookAndFeel

**Класс:** `SyntheticObsidianLookAndFeel : public juce::LookAndFeel_V4`

```cpp
class SyntheticObsidianLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SyntheticObsidianLookAndFeel();
    
    // === Общие методы ===
    void drawButtonBackground(
        juce::Graphics& g,
        juce::Button& button,
        const juce::Colour& backgroundColour,
        bool isMouseOverButton,
        bool isButtonDown) override;
    
    void drawToggleButton(
        juce::Graphics& g,
        juce::ToggleButton& button,
        bool isMouseOverButton,
        bool isButtonDown) override;
    
    void drawLabel(
        juce::Graphics& g,
        juce::Label& label) override;
    
    void drawLinearSlider(
        juce::Graphics& g,
        int x, int y, int width, int height,
        float sliderPos,
        float minSliderPos,
        float maxSliderPos,
        const juce::Slider::SliderStyle style,
        juce::Slider& slider) override;
    
    void drawRotarySlider(
        juce::Graphics& g,
        int x, int y, int width, int height,
        float sliderPosProportional,
        float rotaryStartAngle,
        float rotaryEndAngle,
        juce::Slider& slider) override;
    
    // === Дополнительные методы ===
    void drawCustomMeter(
        juce::Graphics& g,
        int x, int y, int width, int height,
        float value, // 0.0-1.0
        const juce::String& label);
    
    void drawPianoRollGrid(
        juce::Graphics& g,
        int x, int y, int width, int height);
    
    void drawNoteBlock(
        juce::Graphics& g,
        int x, int y, int width, int height,
        bool isSelected,
        const juce::Colour& noteColour);
    
    void drawPlayhead(
        juce::Graphics& g,
        int x,
        int y,
        int height);

private:
    // === Цветовые константы ===
    const juce::Colour colourBackground = juce::Colour(0xFF0a0a12);
    const juce::Colour colourSurface = juce::Colour(0xFF0f0f1a);
    const juce::Colour colourAccentCyan = juce::Colour(0xFF00d4c8);
    const juce::Colour colourAccentPurple = juce::Colour(0xFF8b5cf6);
    const juce::Colour colourTextPrimary = juce::Colour(0xFFe2e8f0);
    const juce::Colour colourTextSecondary = juce::Colour(0xFF64748b);
    const juce::Colour colourGreen = juce::Colour(0xFF22c55e);
    
    // === Шрифты ===
    juce::Font fontLogo{20.0f, juce::Font::semiBold};
    juce::Font fontHeader{16.0f, juce::Font::semiBold};
    juce::Font fontBody{13.0f};
    juce::Font fontLabel{11.0f};
};
```

### 5.2. JUCE Components Map

| UI Element | JUCE Component | Custom Paint | Notes |
|-----------|----------------|--------------|-------|
| GhostButton | juce::TextButton | Yes | Контурный стиль |
| PrimaryButton | juce::TextButton | Yes | Заполненный циан стиль |
| Toggle (Solo/Mute) | juce::ToggleButton | Yes | Квадратные иконки |
| HorizontalSlider | juce::Slider (Linear) | Yes | Custom thumb & track |
| RotaryKnob | juce::Slider (Rotary) | Yes | 270° диапазон, иконка стрелка |
| SegmentedSlider | Кастом компонент | Yes | Grid-based segments |
| CircularMeter | Кастом компонент | Yes | Круговой прогресс arc |
| TrackRow | Кастом компонент (juce::Component) | Yes | Контейнер с иконками и кнопками |
| TextInput | juce::TextEditor | Yes | Custom border & focus |
| Dropdown | juce::ComboBox | Yes | Custom popup style |
| StatusBadge | Кастом компонент | Yes | Иконка + текст + анимация |
| Checkbox | juce::ToggleButton | Yes | Квадратный чекбокс |

### 5.3. Пример реализации: RotaryKnob (PITCH DRIFT)

```cpp
class RotaryKnobComponent : public juce::Component,
                            public juce::Slider::Listener
{
public:
    RotaryKnobComponent(const juce::String& labelText,
                        double minValue,
                        double maxValue,
                        double defaultValue)
        : label(labelText)
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, 
                               false, 60, 20);
        slider.setRange(minValue, maxValue, 1.0);
        slider.setValue(defaultValue);
        slider.setLookAndFeel(&lookAndFeel);
        slider.addListener(this);
        
        addAndMakeVisible(slider);
    }
    
    void resized() override
    {
        slider.setBounds(0, 0, getWidth(), getHeight());
    }
    
    void sliderValueChanged(juce::Slider* slider) override
    {
        // Обновить значение параметра
        sendChangeMessage();
    }

private:
    juce::Slider slider;
    juce::String label;
    SyntheticObsidianLookAndFeel lookAndFeel;
};
```

### 5.4. Пример реализации: Piano Roll Component

```cpp
class PianoRollComponent : public juce::Component
{
public:
    PianoRollComponent()
    {
        setOpaque(true);
    }
    
    void paint(juce::Graphics& g) override
    {
        // === Фон ===
        g.fillAll(juce::Colour(0xFF0a0a12));
        
        // === Сетка ===
        drawGrid(g);
        
        // === Волна ===
        drawWaveform(g);
        
        // === Note блоки ===
        for (const auto& note : notes)
            drawNoteBlock(g, note);
        
        // === Playhead ===
        drawPlayhead(g);
    }
    
    void resized() override
    {
        // Обновить размеры
    }

private:
    void drawGrid(juce::Graphics& g)
    {
        // Вертикальные линии (time)
        for (int x = 0; x < getWidth(); x += 32)
        {
            g.setColour(juce::Colour(0x0affffff));
            g.drawVerticalLine(x, 0.0f, (float)getHeight());
        }
        
        // Горизонтальные линии (notes)
        const int noteHeight = 8;
        for (int y = 0; y < getHeight(); y += noteHeight)
        {
            g.setColour(juce::Colour(0x0affffff));
            g.drawHorizontalLine(y, 0.0f, (float)getWidth());
        }
    }
    
    void drawWaveform(juce::Graphics& g)
    {
        g.setColour(juce::Colour(0xFF00d4c8).withAlpha(0.3f));
        
        // Данные волны из аудио буффера
        juce::Path waveformPath;
        // ... заполнить путь точками волны
        g.strokePath(waveformPath, juce::PathStrokeType(1.0f));
    }
    
    void drawNoteBlock(juce::Graphics& g, const NoteBlock& note)
    {
        juce::Rectangle<int> bounds = note.getBounds();
        
        // Выбрать цвет в зависимости от трека
        juce::Colour noteColour = 
            (note.trackType == TrackType::Guide) 
                ? juce::Colour(0xFF00d4c8) 
                : juce::Colour(0xFF8b5cf6);
        
        // Фон
        g.setColour(noteColour);
        g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
        
        // Граница
        g.setColour(noteColour.darker(0.3f));
        g.drawRoundedRectangle(bounds.toFloat(), 3.0f, 1.0f);
        
        // Если выбран
        if (note.isSelected)
        {
            g.setColour(juce::Colour(0xFF00d4c8));
            g.drawRoundedRectangle(bounds.toFloat(), 3.0f, 2.0f);
            
            g.setColour(juce::Colour(0xFF00d4c8).withAlpha(0.2f));
            g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
        }
    }
    
    void drawPlayhead(juce::Graphics& g)
    {
        int playheadX = (int)(playheadPosition * getWidth());
        
        g.setColour(isPlaying ? 
            juce::Colour(0xFFef4444) : 
            juce::Colour(0xFF00d4c8));
        
        g.drawVerticalLine(playheadX, 0.0f, (float)getHeight());
    }
    
    struct NoteBlock
    {
        int startTime; // samples
        int duration;  // samples
        int pitch;     // MIDI note (0-127)
        TrackType trackType;
        bool isSelected = false;
        
        juce::Rectangle<int> getBounds() const
        {
            // Вычислить позицию и размер в пикселях
            return juce::Rectangle<int>(0, 0, 0, 0);
        }
    };
    
    std::vector<NoteBlock> notes;
    double playheadPosition = 0.0;
    bool isPlaying = false;
};
```

### 5.5. Пример реализации: SegmentedSlider

```cpp
class SegmentedSliderComponent : public juce::Component,
                                 public juce::Button::Listener
{
public:
    SegmentedSliderComponent(const juce::StringArray& segmentLabels)
        : labels(segmentLabels)
    {
        for (size_t i = 0; i < labels.size(); ++i)
        {
            auto button = std::make_unique<juce::ToggleButton>();
            button->setButtonText(labels[i]);
            button->addListener(this);
            buttons.push_back(button.get());
            addAndMakeVisible(button.get());
        }
    }
    
    void resized() override
    {
        int segmentWidth = getWidth() / buttons.size();
        
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            buttons[i]->setBounds(
                i * segmentWidth, 0,
                segmentWidth, getHeight()
            );
        }
    }
    
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xFF0f0f1a));
        
        // Рисовать границы между сегментами
        int segmentWidth = getWidth() / buttons.size();
        g.setColour(juce::Colour(0x14ffffff));
        
        for (int i = 1; i < (int)buttons.size(); ++i)
        {
            g.drawVerticalLine(i * segmentWidth, 0.0f, (float)getHeight());
        }
    }
    
    void buttonClicked(juce::Button* button) override
    {
        // Отключить все остальные кнопки
        for (auto b : buttons)
            b->setToggleState(b == button, juce::dontSendNotification);
        
        // Уведомить слушателей
        sendChangeMessage();
    }

private:
    juce::StringArray labels;
    std::vector<juce::ToggleButton*> buttons;
};
```

### 5.6. Эффекты и анимация

#### Анимация Playhead

```cpp
class PlayheadAnimator : public juce::Timer
{
public:
    PlayheadAnimator(PianoRollComponent& pianoRoll)
        : component(pianoRoll)
    {
        startTimer(16); // ~60fps (16.67ms per frame)
    }
    
    void timerCallback() override
    {
        if (isPlaying)
        {
            currentPosition += incrementPerFrame;
            if (currentPosition >= 1.0)
                currentPosition = 0.0;
            
            component.setPlayheadPosition(currentPosition);
            component.repaint();
        }
    }
    
    void startPlayback() { isPlaying = true; }
    void stopPlayback() { isPlaying = false; }

private:
    PianoRollComponent& component;
    double currentPosition = 0.0;
    double incrementPerFrame = 0.001; // 0.1% per 16ms
    bool isPlaying = false;
};
```

#### Пульсирующий AI Influence Meter

```cpp
class CircularMeterComponent : public juce::Component,
                               public juce::Timer
{
public:
    CircularMeterComponent()
    {
        startTimer(30);
    }
    
    void paint(juce::Graphics& g) override
    {
        // === Background arc ===
        g.setColour(juce::Colour(0x14ffffff));
        drawArc(g, 0.0f, 1.0f, 3.0f);
        
        // === Filled arc (gradient) ===
        juce::ColourGradient gradient(
            juce::Colour(0xFF00d4c8), 0.0f, 0.0f,
            juce::Colour(0xFF8b5cf6), (float)getWidth(), 0.0f,
            false
        );
        g.setGradientFill(gradient);
        drawArc(g, 0.0f, value, 3.0f);
        
        // === Center text (percentage) ===
        juce::String percentageText = 
            juce::String((int)(value * 100.0f)) + "%";
        
        g.setColour(juce::Colour(0xFF00d4c8));
        g.setFont(juce::Font(20.0f, juce::Font::semiBold));
        g.drawFittedText(percentageText,
            getLocalBounds(), juce::Justification::centred, 1);
        
        // === Pulsing glow (когда AI активен) ===
        if (isProcessing)
        {
            float glowAlpha = std::sin(pulsePhase * 3.14159f) * 0.3f + 0.1f;
            g.setColour(juce::Colour(0xFF00d4c8).withAlpha(glowAlpha));
            g.drawEllipse(
                getLocalBounds().toFloat().reduced(5),
                3.0f
            );
        }
    }
    
    void timerCallback() override
    {
        pulsePhase += 0.05f;
        if (pulsePhase >= 1.0f)
            pulsePhase = 0.0f;
        
        repaint();
    }
    
    void setValue(float newValue)
    {
        value = juce::jlimit(0.0f, 1.0f, newValue);
    }
    
    void setProcessing(bool processing)
    {
        isProcessing = processing;
    }

private:
    float value = 0.5f;
    float pulsePhase = 0.0f;
    bool isProcessing = false;
    
    void drawArc(juce::Graphics& g,
                 float startValue,
                 float endValue,
                 float lineWidth)
    {
        const float centerX = getWidth() * 0.5f;
        const float centerY = getHeight() * 0.5f;
        const float radius = (getWidth() - 10) * 0.5f;
        
        const float startAngle = 3.14159f; // 180° (bottom)
        const float arcSpan = 3.14159f;    // 180° total span
        
        float angle1 = startAngle + (startValue * arcSpan);
        float angle2 = startAngle + (endValue * arcSpan);
        
        juce::Path arc;
        arc.addCentredArc(centerX, centerY, radius, radius,
                         0.0f, angle1, angle2, true);
        
        g.strokePath(arc, juce::PathStrokeType(lineWidth));
    }
};
```

#### Анимация волны при воспроизведении

```cpp
class WaveformAnimator
{
public:
    void updateWaveformAlpha(bool isPlaying)
    {
        targetAlpha = isPlaying ? 0.8f : 0.5f;
    }
    
    void timerCallback()
    {
        if (std::abs(currentAlpha - targetAlpha) > 0.01f)
        {
            currentAlpha += (targetAlpha - currentAlpha) * 0.1f;
        }
    }
    
    float getCurrentAlpha() const { return currentAlpha; }

private:
    float currentAlpha = 0.5f;
    float targetAlpha = 0.5f;
};
```

---

## 6. Анимационные спецификации

### 6.1. Переходы и timing

| Элемент | Transition | Duration | Easing |
|---------|-----------|----------|--------|
| Button hover | Background + shadow | 150ms | ease-out |
| Slider drag | Smooth value update | - | linear |
| Playhead move | Continuous smooth | - | linear |
| Status badge pulse | Opacity | 500ms | infinite |
| AI meter arc | Value fill | 300ms | ease-out |
| Note selection | Outline + glow | 200ms | ease-out |
| Track hover | Background | 200ms | ease-out |

### 6.2. Специальные анимации

#### Waveform Display
- При паузе: opacity = 0.5, статическое отображение
- Во время воспроизведения: opacity = 0.8, плавное изменение яркости
- Transition time: 300ms ease-in-out

#### Playhead
- Smooth horizontal movement synchronized с audio playback
- Цвет: #ef4444 (красный) во время воспроизведения, #00d4c8 (циан) при паузе
- Fade transition between colors: 200ms

#### AI Processing Indicator (CircularMeter)
- Пульсирующий ореол: 1000ms infinite sine wave
- Opacity range: 0.1 → 0.4 → 0.1
- Arc fill animation: 300ms ease-out при изменении значения

#### Note Block Selection
- Outline появляется: 150ms ease-out
- Inner glow fade in: 200ms ease-out
- Combination effect: 2px outline + 0.2 alpha fill

#### Button Press Feedback
- Transform: translateY(-2px) on hover
- Transform: translateY(0px) on click/release
- Duration: 100ms ease-out

---

## 7. Доступность (Accessibility)

### 7.1. Контрастность

| Элемент | Foreground | Background | Контраст (WCAG) |
|---------|-----------|-----------|-----------------|
| Text Primary | #e2e8f0 | #0a0a12 | 16.8:1 ✓ AAA |
| Text Secondary | #64748b | #0a0a12 | 6.1:1 ✓ AA |
| Active Button | #0a0a12 | #00d4c8 | 10.5:1 ✓ AAA |
| Success Badge | #22c55e | #0a0a12 | 8.2:1 ✓ AAA |

### 7.2. Клавиатурная навигация

- Tab: фокус на следующий элемент
- Shift+Tab: фокус на предыдущий элемент
- Enter/Space: активировать кнопку
- Arrow Keys (на слайдерах): +/- значение
- Escape: закрыть диалог/отменить

### 7.3. Screen Reader поддержка

- Все кнопки имеют accessible names
- Слайдеры: текущее значение объявляется
- Статус badges: статус объявляется
- Пиано roll: навигация по нотам с объявлением pitch

---

## 8. Производительность и оптимизация

### 8.1. Рендеринг

- Использовать `setOpaque(true)` для компонентов без прозрачности
- Кэшировать сложные paths (сетка, волнообразные линии)
- Redraw только измененные области (repaint bounds)
- Использовать `juce::CachedComponentImage` для статических элементов

### 8.2. Memory Management

- Piano Roll: использовать single buffer для всех note blocks
- Слайдеры: переиспользовать paint методы
- Анимации: использовать `juce::Timer` вместо `juce::Thread`

### 8.3. Platform-specific

- macOS: использовать Metal rendering если доступно
- Windows: проверить CPU usage при animation
- Linux: убедиться в совместимости с разными дистрибутивами

---

## 9. Примечания по реализации

### 9.1. Загрузка шрифтов

```cpp
// В PluginProcessor.cpp
juce::FontOptions fontOptions;
fontOptions.withHeight(20.0f);
fontOptions.withTypefaceName("Inter");

// Если Inter не установлен, использовать системный шрифт
auto typeface = juce::Typeface::createSystemTypefaceFor(fontOptions);
```

### 9.2. BinaryData для шрифтов

```cpp
// В Projucer: добавить Inter.ttf в BinaryData
// В коде:
auto fontData = BinaryData::Interttf;
auto fontDataSize = BinaryData::InterttfSize;

auto typeface = juce::Typeface::createSystemTypefaceFor(
    juce::MemoryInputStream(fontData, fontDataSize, false)
);
```

### 9.3. Color Constants

```cpp
namespace SynthObsidianColours
{
    constexpr uint32_t Background = 0xFF0a0a12;
    constexpr uint32_t Surface = 0xFF0f0f1a;
    constexpr uint32_t AccentCyan = 0xFF00d4c8;
    constexpr uint32_t AccentPurple = 0xFF8b5cf6;
    constexpr uint32_t TextPrimary = 0xFFe2e8f0;
    constexpr uint32_t TextSecondary = 0xFF64748b;
    constexpr uint32_t SuccessGreen = 0xFF22c55e;
}
```

---

## 10. Тестирование UI

### 10.1. Визуальная проверка

- [ ] Все цвета совпадают с палеттой
- [ ] Отступы соответствуют 8px сетке
- [ ] Текст читаемый и правильно выровнен
- [ ] Состояния компонентов (hover, active, disabled) отображаются корректно

### 10.2. Интерактивное тестирование

- [ ] Слайдеры плавно двигаются
- [ ] Кнопки отзываются на нажатие
- [ ] Playhead синхронизирован с audio
- [ ] Анимации не заикаются (>30fps)

### 10.3. Кроссплатформное тестирование

- [ ] macOS (Silicon + Intel)
- [ ] Windows 10/11
- [ ] Linux (Ubuntu 20.04+)

---

**Конец спецификации**

**Версия документа:** 1.0  
**Последнее обновление:** 2026-04-11  
**Статус:** Готово к разработке
