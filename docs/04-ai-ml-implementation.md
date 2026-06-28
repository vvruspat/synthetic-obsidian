# AI/ML Реализация — Synthetic Obsidian

## 1. Обзор AI компонентов

### 1.1 Разделение ответственности: AI vs DSP

Синтетический Obsidian реализует **гибридную архитектуру** с разделением задач:

| Компонент | Технология | Назначение | Режим |
|-----------|-----------|-----------|-------|
| **C++ DSP** | JUCE, Eigen | Фильтрация, микширование, real-time обработка | Синхронный, <1ms latency |
| **ONNX Runtime** | ONNX, TensorRT | Inference моделей, pitch shift, voice conversion | Async, <50ms latency |
| **Python** | librosa, madmom, Essentia, Seed-VC | Анализ гайда, обучение/исследования, offline backing vocals | Batch/background обработка |

### 1.2 Трёхуровневая архитектура

```
┌─────────────────────────────────────────────────────┐
│             VST Plugin UI (JUCE)                    │
└────────────┬────────────────────────────────────────┘
             │
┌────────────▼────────────────────────────────────────┐
│  C++ Real-Time DSP Layer (Audio I/O, <1ms)         │
│  • Audio buffering (JACK/ASIO/CoreAudio)            │
│  • Wave table synthesis                             │
│  • Convolution, filtering                           │
│  • Phase vocoder (fallback)                         │
└────────────┬────────────────────────────────────────┘
             │ (async queue, non-blocking)
┌────────────▼────────────────────────────────────────┐
│  ONNX Runtime Inference Layer (~10-50ms)            │
│  • HuBERT feature extraction (CPU)                   │
│  • VITS vocoder (GPU/CPU with fallback)             │
│  • Pitch detection (CREPE quantized)                │
│  • Voice conversion (FAISS + synthesis)             │
└────────────┬────────────────────────────────────────┘
             │ (subprocess IPC or pybind11)
┌────────────▼────────────────────────────────────────┐
│  Python Analysis/Training Layer (offline)            │
│  • librosa, madmom, Essentia                        │
│  • RVC model training                               │
│  • Seed-VC offline backing-vocal rendering          │
│  • Batch processing, dataset preparation            │
└─────────────────────────────────────────────────────┘
```

### 1.3 Текущий MVP-spike: Seed-VC для `AI BACKS`

На 2026-04-17 лучший прослушанный backend для натуральных бэк-вокалов — Seed-VC,
запущенный как offline Python subprocess. Это не realtime inference и не часть
`processBlock()`.

Текущая интеграция:

- C++ bridge: `Source/ai/SeedVCBridge.*`
- UI entry point: кнопка `AI BACKS` в `TransportBar`
- Processor API: `SyntheticObsidianProcessor::renderSeedVCBackVocals`
- Python runner: `research/svc_pitch/run_seed_vc_file.py`
- Polish: `research/svc_pitch/polish_outputs.py --mode headroom --target-peak-db -3.0`
- Local venv: `research/.venv_seed_vc`
- Output: `~/Music/Synthetic Obsidian/SeedVC/<source>_<timestamp>/`

Ключевое правило: Seed-VC вызывается только из background thread. Готовые WAV
загружаются обратно как обычные `Back Vox` tracks, поэтому аудио-поток остаётся
детерминированным и не вызывает Python/ONNX.

### 1.4 Research: DALI для обучения детектора нот/слогов

Для улучшения Piano Roll analysis выбран двухэтапный путь:

1. DALI используется как pretraining/weak-label источник: aligned lyrics + vocal
   notes дают стартовую разметку нот, слов и строк, но не считаются абсолютной
   истиной из-за известных ошибок автоматического выравнивания.
2. Локальные пары vocal audio + MIDI/ручная правка используются для fine-tune,
   чтобы финальная модель научилась резать длинные ноты по ритмике фразы и не
   дробить вибрато на ложные ноты.

Рабочая папка: `research/note_detector/`. Первый инструмент:
`research/note_detector/import_dali.py`, который конвертирует DALI `.gz`
аннотации в нормализованные `labels/<dali_id>.json` и `manifest.jsonl`.

DALI лицензирован как CC BY-NC-SA 4.0, поэтому пока это research/prototype data.
Перед коммерческим распространением модели, обученной на DALI, нужен отдельный
лицензионный разбор или замена финального train set на собственные данные.

---

## 2. Анализ Guide трека (оффлайн)

### 2.1 Определение тональности: Essentia HPCP

**HPCP** (Harmonic Pitch Class Profile) — спектрально-гармонический анализ для определения тональности.

**Алгоритм:**
1. Вычисление HPCP профиля из спектрограммы
2. Сравнение с Krumhansl-Schmuckler профилями 24 тональностей
3. Динамическое отслеживание смен тональности

**Python реализация:**

```python
import essentia.standard as es
import numpy as np
from librosa import hz_to_midi

def analyze_tonality(audio_path: str, sr: int = 44100):
    """Определение тональности трека с высокой точностью."""
    
    # Загрузка аудио
    loader = es.MonoLoader(filename=audio_path, sampleRate=sr)
    audio = loader()
    
    # Спектральный анализ
    spectrum = es.Spectrum(size=4096)
    windowing = es.Windowing(type='hann')
    
    # HPCP профиль
    hpcp = es.HPCP(size=12)
    hpcp_buffer = []
    
    hop_size = 2048
    for i in range(0, len(audio) - 4096, hop_size):
        frame = audio[i:i+4096]
        windowed = windowing(frame)
        spec = spectrum(windowed)
        h = hpcp(spec)
        hpcp_buffer.append(h)
    
    # Усредняем HPCP за весь трек
    avg_hpcp = np.mean(hpcp_buffer, axis=0)
    
    # Krumhansl-Schmuckler профили для всех тональностей
    # Major и minor профили (standard music theory)
    major_profiles = [
        [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88],  # C major
    ]
    minor_profiles = [
        [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17],  # A minor
    ]
    
    # Ротируем профили на все 12 полутонов
    def rotate_profile(p):
        return [p[(i - j) % 12] for i in range(12) for j in range(1)]
    
    # Ищем лучшее совпадение
    best_key = None
    best_score = -float('inf')
    
    for shift in range(12):
        rotated_major = np.roll(major_profiles[0], shift)
        correlation = np.dot(avg_hpcp, rotated_major)
        
        if correlation > best_score:
            best_score = correlation
            best_key = (shift, 'major')
    
    return {
        'key': ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'][best_key[0]],
        'mode': best_key[1],
        'confidence': best_score / np.sum(major_profiles[0]),
        'hpcp_vector': avg_hpcp.tolist()
    }
```

### 2.2 Определение BPM: Ensemble madmom + librosa

**Методология:**
- **madmom RNNBeatProcessor**: глубокая нейросеть, обучена на 600+ часах музыки
- **librosa tempogram**: спектрограмма пульсации для валидации
- Ensemble voting для повышения точности

**Python реализация:**

```python
import madmom
import librosa
import numpy as np
from scipy.signal import find_peaks

def analyze_bpm(audio_path: str, sr: int = 44100):
    """Определение темпо/BPM с помощью RNN + tempogram."""
    
    # Метод 1: madmom RNNBeatProcessor (high-level)
    proc = madmom.features.beats.RNNBeatProcessor()
    act = madmom.features.beats.ActivationProcessor()(audio_path)
    beat_times = proc(act)
    
    if len(beat_times) > 1:
        beat_intervals = np.diff(beat_times)
        madmom_bpm = np.median(60.0 / beat_intervals)
    else:
        madmom_bpm = None
    
    # Метод 2: librosa tempogram (low-level)
    y, sr = librosa.load(audio_path, sr=sr)
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    
    # Tempogram: strength vs BPM across time
    tempogram = librosa.feature.tempogram(onset_env=onset_env, sr=sr)
    
    # Глобальный autocorrelation для BPM
    bpm_values = librosa.tempo(onset_env=onset_env, sr=sr, aggregate=None)
    
    # Поиск пиков в 50-180 BPM диапазоне
    freqs = librosa.tempo_frequencies(n_fft=len(tempogram), sr=sr)
    valid_idx = np.where((freqs >= 50/60) & (freqs <= 180/60))[0]
    
    if len(valid_idx) > 0:
        max_idx = valid_idx[np.argmax(tempogram.mean(axis=1)[valid_idx])]
        librosa_bpm = freqs[max_idx] * 60
    else:
        librosa_bpm = None
    
    # Ensemble: выбираем медиану из двух методов
    bpm_candidates = [b for b in [madmom_bpm, librosa_bpm] if b is not None]
    final_bpm = np.median(bpm_candidates) if bpm_candidates else 120.0
    
    return {
        'bpm': float(final_bpm),
        'madmom_bpm': float(madmom_bpm) if madmom_bpm else None,
        'librosa_bpm': float(librosa_bpm) if librosa_bpm else None,
        'confidence': min(1.0, len(bpm_candidates) / 2.0),
        'beat_times': beat_times.tolist() if madmom_bpm else []
    }
```

### 2.3 Определение размера: Вероятностный анализ

**Алгоритм:** анализируем интервалы между beat'ами для определения 4/4, 3/4, 6/8 и т.д.

```python
def analyze_meter(beat_times: np.ndarray, n_candidates: int = 3):
    """Определение размера (meter) из beat times."""
    
    if len(beat_times) < 8:
        return {'meter': '4/4', 'confidence': 0.5}
    
    # Интервалы между last beat'ами (в долях)
    intervals = np.diff(beat_times)
    
    # Нормализуем к первому интервалу
    normalized = intervals / intervals[0]
    
    # Ищем мультипликативные отношения
    meter_patterns = {
        '4/4': [1, 1, 1, 1],
        '3/4': [1, 1, 1],
        '6/8': [1, 1],
        '2/4': [1, 1],
    }
    
    scores = {}
    for meter, pattern in meter_patterns.items():
        # Повторяем паттерн столько раз, сколько нужно
        repeated = pattern * (len(normalized) // len(pattern))
        score = 1.0 - np.mean(np.abs(normalized[:len(repeated)] - repeated))
        scores[meter] = score
    
    best_meter = max(scores.items(), key=lambda x: x[1])
    
    return {
        'meter': best_meter[0],
        'confidence': float(best_meter[1]),
        'all_scores': scores
    }
```

### 2.4 Детекция модуляций: Frame-by-frame Chord Analysis

```python
def detect_modulations(audio_path: str, sr: int = 44100, hop_length: int = 2048):
    """Детекция смен тональности (модуляций) в реальном времени."""
    
    y, sr = librosa.load(audio_path, sr=sr)
    
    # CQT трансформ (музыкальнее чем STFT)
    cqt = np.abs(librosa.cqt(y, sr=sr, hop_length=hop_length))
    
    # Определяем хромаграмму (12 нот)
    chroma = librosa.feature.chroma_cqt(C=cqt, sr=sr)
    
    # Для каждого фрейма ищем nearest key
    modulations = []
    window_size = 32  # ~1 сек при sr=44100
    
    for i in range(window_size, chroma.shape[1] - window_size):
        window_chroma = np.mean(chroma[:, i-window_size:i+window_size], axis=1)
        
        # Коррелируем с каждым major/minor key
        max_corr = -1
        best_key = 'C'
        
        note_names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
        
        for shift in range(12):
            rotated = np.roll(window_chroma, shift)
            corr = np.dot(rotated, [1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1])
            if corr > max_corr:
                max_corr = corr
                best_key = note_names[shift]
        
        frame_time = i * hop_length / sr
        modulations.append({
            'time': frame_time,
            'key': best_key,
            'confidence': max_corr / np.sum([1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1])
        })
    
    # Сглаживаем: удаляем шумовые переходы
    filtered = []
    for i, m in enumerate(modulations):
        if i == 0 or m['key'] != modulations[i-1]['key']:
            # Проверяем, сколько фреймов идентичны
            count = 1
            j = i + 1
            while j < len(modulations) and modulations[j]['key'] == m['key']:
                count += 1
                j += 1
            
            if count >= 4:  # Минимум ~4 сек одного key
                filtered.append({
                    'time': m['time'],
                    'key': m['key'],
                    'duration': (modulations[min(j-1, len(modulations)-1)]['time'] - m['time'])
                })
    
    return {'modulations': filtered}
```

### 2.5 C++ вызов Python через pybind11

**CMakeLists.txt:**

```cmake
# Поддержка Python
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# Добавляем pybind11
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/pybind11)

# Создаём Python binding
pybind11_add_module(obsidian_analysis src/python_bindings.cpp)

target_link_libraries(obsidian_analysis PRIVATE
    Python3::Python
    ${JUCE_LIBRARIES}
)
```

**src/python_bindings.cpp:**

```cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <Python.h>

namespace py = pybind11;

class AudioAnalyzer {
public:
    std::map<std::string, py::object> analyzeGuideTrack(const std::string& audioPath) {
        // GIL (Global Interpreter Lock) management
        py::gil_scoped_acquire acquire;
        
        try {
            // Импортируем Python модуль
            auto analysis_module = py::module::import("obsidian.analysis");
            
            // Вызываем функцию Python
            auto tonality_result = analysis_module.attr("analyze_tonality")(audioPath);
            auto bpm_result = analysis_module.attr("analyze_bpm")(audioPath);
            auto modulation_result = analysis_module.attr("detect_modulations")(audioPath);
            
            // Преобразуем результаты в C++ structures
            std::map<std::string, py::object> results;
            results["tonality"] = tonality_result;
            results["bpm"] = bpm_result;
            results["modulations"] = modulation_result;
            
            return results;
            
        } catch (const py::error_already_set& e) {
            std::cerr << "Python error: " << e.what() << std::endl;
            throw;
        }
    }
};

// Регистрируем C++ класс в Python
PYBIND11_MODULE(obsidian_analysis, m) {
    py::class_<AudioAnalyzer>(m, "AudioAnalyzer")
        .def(py::init<>())
        .def("analyzeGuideTrack", &AudioAnalyzer::analyzeGuideTrack);
    
    m.doc() = "Synthetic Obsidian Audio Analysis Module";
}
```

---

## 3. AI Коррекция питча (без артефактов)

### 3.1 Детекция питча: pYIN vs CREPE vs PENN

| Алгоритм | Скорость | Точность (cents) | Latency | Лучше для |
|----------|----------|-----------------|---------|-----------|
| **pYIN** | <5ms | ±10-15 cents | <10ms real-time | Live вокал |
| **CREPE** | 20-50ms | ±5-10 cents (CNN) | 50-100ms offline | Студийная запись |
| **PENN** | 10-30ms | ±2-5 cents (CRNN) | 20-50ms | Максимальная точность |

**Python реализация (Ensemble Pitch Detection):**

```python
import crepe
import librosa
import numpy as np
from scipy.ndimage import median_filter

def detect_pitch_ensemble(audio_path: str, sr: int = 44100, 
                         threshold: float = 0.1) -> np.ndarray:
    """
    Ensemble pitch detection с использованием CREPE + librosa.
    
    Returns:
        frequencies (Hz) и confidence scores
    """
    
    y, sr = librosa.load(audio_path, sr=sr)
    
    # Метод 1: CREPE (CNN-based, very accurate)
    time, frequency, confidence, activation = crepe.predict(
        y, sr,
        viterbi=True,  # Использует Viterbi smoothing
        step_size=10,  # 10ms hop
        model='full',  # full model (not tiny)
    )
    
    # Фильтруем по confidence
    crepe_freqs = frequency.copy()
    crepe_freqs[confidence < threshold] = 0
    
    # Метод 2: pYIN (probabilistic, faster)
    f0, voiced_flag, voiced_probs = librosa.yin(
        y,
        fmin=80,  # Мин частота для вокала
        fmax=400,  # Макс частота
        trough_threshold=0.1
    )
    
    # Интерполируем librosa результат к CREPE временной шкале
    crepe_times = np.arange(len(crepe_freqs)) * 0.01  # 10ms хопс
    librosa_times = np.arange(len(f0)) * librosa.get_samplerate(audio_path) / len(y)
    librosa_interp = np.interp(crepe_times, librosa_times, f0, 
                               left=0, right=0)
    
    # Ensemble: берём CREPE если confidence высокая, иначе librosa
    ensemble_freqs = np.where(
        confidence > threshold,
        crepe_freqs,
        librosa_interp
    )
    
    # Сглаживание с Kalman filter (см. раздел 3.4)
    smoothed = kalman_filter_pitch(ensemble_freqs, confidence)
    
    return {
        'frequencies': smoothed.tolist(),
        'confidence': confidence.tolist(),
        'times': time.tolist(),
        'algorithm': 'ensemble_crepe_yin'
    }

def kalman_filter_pitch(frequencies: np.ndarray, 
                       confidence: np.ndarray,
                       process_variance: float = 1.0,
                       observation_variance: float = 4.0) -> np.ndarray:
    """Сглаживание контура питча с Kalman filter."""
    
    filtered = np.zeros_like(frequencies)
    estimate = frequencies[0]
    error_estimate = 1.0
    
    for i, (z, conf) in enumerate(zip(frequencies, confidence)):
        if z == 0:  # Unvoiced
            filtered[i] = 0
            continue
        
        # Prediction
        estimate_prior = estimate
        error_estimate_prior = error_estimate + process_variance
        
        # Update
        kalman_gain = error_estimate_prior / (
            error_estimate_prior + observation_variance / (conf + 0.01)
        )
        estimate = estimate_prior + kalman_gain * (z - estimate_prior)
        error_estimate = (1 - kalman_gain) * error_estimate_prior
        
        filtered[i] = estimate
    
    return filtered
```

### 3.2 Почему обычные алгоритмы дают артефакты

**Phase Vocoder Smearing:**
- При сдвиге питча Phase Vocoder растягивает/сжимает спектр
- Результат: **формантовый сдвиг** (певец звучит выше/ниже в голосе)
- Решение: WORLD vocoder с раздельной обработкой питча и форманты

**Formant Shift Problem:**
```
Исходный голос:     f0=100Hz, formant1=700Hz, formant2=1220Hz
Phase Vocoder +5st: f0=133Hz, formant1=933Hz, formant2=1627Hz  ❌
WORLD vocoder +5st: f0=133Hz, formant1=700Hz, formant2=1220Hz  ✓
```

**Оригинальный WORLD vocoder — это C++, но мы используем его через Python обёртку:**

```python
import pyworld
import numpy as np

def shift_pitch_world(audio: np.ndarray, sr: int, 
                     shift_cents: float) -> np.ndarray:
    """
    WORLD vocoder для сдвига питча без артефактов.
    
    Advantages:
    - Разделение: f0 + spectrum + aperiodicity
    - Сохранение формант
    - MOS 4.5 (очень естественно)
    - Лучше для женских голосов
    """
    
    # Шаг 1: Анализ
    _f0, t = pyworld.harvest(audio, sr)
    sp = pyworld.cheaptrick(audio, _f0, t, sr)
    ap = pyworld.d4c(audio, _f0, t, sr)
    
    # Шаг 2: Модификация f0
    shift_ratio = 2 ** (shift_cents / 1200)  # cents -> ratio
    modified_f0 = _f0 * shift_ratio
    
    # Шаг 3: Синтез
    synthesized = pyworld.synthesize(modified_f0, sp, ap, sr)
    
    return synthesized
```

### 3.3 Сохранение форманты: LPC анализ + Frequency Warping

**LPC (Linear Predictive Coding) для формант:**

```python
from scipy.signal import lpc
import numpy as np

def preserve_formants_lpc(audio: np.ndarray, sr: int,
                         pitch_shift_cents: float,
                         n_formants: int = 4) -> np.ndarray:
    """
    Сохранение формант через LPC анализ + frequency warping.
    
    Алгоритм:
    1. Извлекаем LPC coefficients (формантовая огибающая)
    2. Warping spectrum в формантовое пространство
    3. Сдвигаем только питч, оставляя форманты на месте
    """
    
    # Анализ с WORLD (как выше)
    _f0, t = pyworld.harvest(audio, sr)
    sp = pyworld.cheaptrick(audio, _f0, t, sr)
    ap = pyworld.d4c(audio, _f0, t, sr)
    
    # LPC для каждого фрейма
    frame_length = int(sr * 0.02)  # 20ms
    formant_contour = []
    
    for i in range(0, len(audio) - frame_length, frame_length // 2):
        frame = audio[i:i + frame_length]
        # LPC order для вокала обычно 10-14
        coeffs, error = lpc(frame, order=12)
        formant_contour.append(coeffs)
    
    # Pitch shifting с сохранением LPC формант
    shift_ratio = 2 ** (pitch_shift_cents / 1200)
    modified_f0 = _f0 * shift_ratio
    
    # Синтез с сохранёнными формантами
    # (спектр остаётся исходным, меняем только питч)
    synthesized = pyworld.synthesize(modified_f0, sp, ap, sr)
    
    return synthesized

def extract_formants(lpc_coeffs: np.ndarray, sr: int) -> dict:
    """Извлечение частот формант из LPC коэффициентов."""
    
    # Корни полинома LPC дают формантовые частоты
    roots = np.roots(lpc_coeffs)
    
    # Только комплексные корни внутри единичного круга
    valid_roots = roots[np.abs(roots) < 1]
    valid_roots = valid_roots[np.imag(valid_roots) > 0]
    
    # Преобразуем в Hz
    formant_freqs = np.angle(valid_roots) * sr / (2 * np.pi)
    formant_freqs = np.sort(formant_freqs)[:4]  # Top 4 formants
    
    return {
        'formant_freqs': formant_freqs.tolist(),
        'bandwidths': calculate_bandwidth(valid_roots, sr)
    }

def calculate_bandwidth(roots: np.ndarray, sr: int) -> np.ndarray:
    """Расчёт bandwidth формант."""
    bandwidths = -sr / np.pi * np.log(np.abs(roots))
    return bandwidths.tolist()
```

### 3.4 Сглаживание контура: Kalman Filter

```cpp
// C++ Kalman Filter для smoothing pitch contour (real-time)
class PitchKalmanFilter {
private:
    float estimate;
    float estimate_error;
    const float process_variance = 1.0f;
    const float observation_variance = 4.0f;

public:
    PitchKalmanFilter() : estimate(0.0f), estimate_error(1.0f) {}
    
    float filter(float measurement, float confidence) {
        // Prediction
        float estimate_prior = estimate;
        float error_estimate_prior = estimate_error + process_variance;
        
        // Update
        float kalman_gain = error_estimate_prior / 
            (error_estimate_prior + observation_variance / (confidence + 0.01f));
        
        estimate = estimate_prior + kalman_gain * (measurement - estimate_prior);
        estimate_error = (1.0f - kalman_gain) * error_estimate_prior;
        
        return estimate;
    }
};
```

---

## 4. RVC (Retrieval-based Voice Conversion)

### 4.1 Архитектура RVC v3

```
┌──────────────────────────────────────┐
│  Исходный вокал (WAV)                 │
└────────────┬─────────────────────────┘
             │
┌────────────▼─────────────────────────┐
│  1. PITCH DETECTION (pYIN/CREPE)      │
│     Output: f0 contour (Hz)           │
└────────────┬─────────────────────────┘
             │
┌────────────▼─────────────────────────┐
│  2. FEATURE EXTRACTION (HuBERT)       │
│     Из спектрограммы → 768D embedding │
│     (speech units, speaker-agnostic)  │
└────────────┬─────────────────────────┘
             │
┌────────────▼─────────────────────────┐
│  3. SPEAKER EMBEDDINGS (Speaker ID)   │
│     Рассчитываем speaker vector       │
│     из training данных целевого голоса│
└────────────┬─────────────────────────┘
             │
┌────────────▼─────────────────────────┐
│  4. RETRIEVAL (FAISS Index)           │
│     Ищем ближайшие примеры            │
│     в тренировочном наборе            │
└────────────┬─────────────────────────┘
             │
┌────────────▼─────────────────────────┐
│  5. VOICE CONVERSION (VITS Vocoder)   │
│     f0 + HuBERT + Speaker ID → Audio  │
└────────────┬─────────────────────────┘
             │
┌────────────▼─────────────────────────┐
│  Сконвертированный вокал (WAV)        │
└──────────────────────────────────────┘
```

### 4.2 Training Pipeline

**Требования:**

| Параметр | Минимум | Оптимально | Максимум |
|----------|---------|-----------|----------|
| Дюрация аудио | 5 мин | 30 мин | 100+ мин |
| Количество файлов | 20 | 100 | 300+ |
| Качество | 16-bit PCM | 24-bit WAV | 96kHz |
| Sample Rate | 16kHz | 44.1kHz | 48kHz |
| Диапазон (pitch) | 1 октава | 1.5 октавы | 2+ октавы |

**Python training script:**

```python
import torch
from torch.utils.data import DataLoader
from rvc.train import RVCTrainer
from rvc.datasets import AudioDataset
import logging

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

def train_rvc_model(train_data_dir: str, 
                   output_model_path: str,
                   epochs: int = 500,
                   batch_size: int = 8,
                   device: str = 'cuda:0'):
    """
    Обучение RVC модели для преобразования голоса.
    
    Args:
        train_data_dir: Папка с WAV файлами целевого голоса
        output_model_path: Где сохранить обученную модель
        epochs: Количество эпох
        batch_size: Размер батча
        device: 'cuda:0', 'cuda:1', 'cpu'
    """
    
    # Инициализация данных
    dataset = AudioDataset(train_data_dir, sr=44100)
    dataloader = DataLoader(dataset, batch_size=batch_size, shuffle=True)
    
    logger.info(f"Loaded {len(dataset)} audio samples")
    
    # Инициализация тренера
    trainer = RVCTrainer(
        model_name='rvc_v3',
        device=device,
        learning_rate=2e-4,
        weight_decay=1e-5
    )
    
    # Training loop
    for epoch in range(epochs):
        total_loss = 0.0
        
        for batch_idx, (waveforms, spectrogram, f0_contour) in enumerate(dataloader):
            # Forward pass
            loss = trainer.train_step(
                waveforms.to(device),
                spectrogram.to(device),
                f0_contour.to(device)
            )
            
            total_loss += loss.item()
            
            # Log progress
            if batch_idx % 10 == 0:
                logger.info(f"Epoch {epoch}/{epochs}, Batch {batch_idx}, Loss: {loss.item():.4f}")
        
        # Validation и checkpoint
        if epoch % 50 == 0:
            val_loss = trainer.validate(dataloader)
            logger.info(f"Epoch {epoch} - Val Loss: {val_loss:.4f}")
            
            # Сохраняем checkpoint
            trainer.save_checkpoint(f"{output_model_path}_epoch_{epoch}.pth")
    
    # Финальная сохранение
    trainer.save_model(output_model_path)
    logger.info(f"Model saved to {output_model_path}")
    
    return trainer

# Вызов
if __name__ == '__main__':
    train_rvc_model(
        train_data_dir='/data/my_voice_samples/',
        output_model_path='/models/my_voice_rvc.pth',
        epochs=500,
        device='cuda:0'
    )
```

### 4.3 Железо для обучения

| GPU | Время | VRAM | Cost |
|-----|-------|------|------|
| RTX 4090 | 15-20 мин | 24GB | $2000 |
| RTX 3090 | 25-35 мин | 24GB | $1200 |
| RTX 4080 | 20-25 мин | 16GB | $1200 |
| RTX 3060 Ti | 30-45 мин | 8GB | $400 |
| RTX 3060 | 40-60 мин | 12GB | $300 |
| RTX 2080 Ti | 60-90 мин | 11GB | $200 used |
| **CPU (Intel i9)** | **6-8 часов** | RAM 32GB | - |

### 4.4 ONNX Экспорт для C++ Inference

**Python:**

```python
import torch
import onnx
from rvc.models import RVCModel

def export_rvc_to_onnx(pytorch_model_path: str, 
                       onnx_model_path: str,
                       opset_version: int = 14):
    """
    Конвертируем PyTorch RVC модель в ONNX для C++ inference.
    """
    
    # Загружаем модель
    model = RVCModel.load_pretrained(pytorch_model_path)
    model.eval()
    
    # Dummy inputs для ONNX export
    dummy_hubert = torch.randn(1, 44, 768)  # [batch, frames, features]
    dummy_f0 = torch.randn(1, 44)           # [batch, frames]
    dummy_speaker_id = torch.zeros(1, dtype=torch.long)  # [batch]
    
    # Export
    input_names = ['hubert_features', 'f0_contour', 'speaker_id']
    output_names = ['audio_output']
    
    torch.onnx.export(
        model,
        (dummy_hubert, dummy_f0, dummy_speaker_id),
        onnx_model_path,
        input_names=input_names,
        output_names=output_names,
        opset_version=opset_version,
        do_constant_folding=True,
        verbose=False,
        dynamic_axes={
            'hubert_features': {1: 'num_frames'},
            'f0_contour': {1: 'num_frames'},
            'audio_output': {1: 'num_samples'}
        }
    )
    
    # Валидация ONNX модели
    onnx_model = onnx.load(onnx_model_path)
    onnx.checker.check_model(onnx_model)
    print(f"✓ ONNX model exported and validated: {onnx_model_path}")
    
    return onnx_model_path
```

### 4.5 ONNX Runtime C++ API

**CMakeLists.txt:**

```cmake
find_package(ONNX REQUIRED)
find_package(onnxruntime REQUIRED)

target_link_libraries(synthetic_obsidian PRIVATE
    onnxruntime
    onnx
)
```

**C++ inference code:**

```cpp
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <iostream>

class RVCInference {
private:
    Ort::Session session_;
    Ort::Env env_;
    const char* input_names_[3] = {"hubert_features", "f0_contour", "speaker_id"};
    const char* output_names_[1] = {"audio_output"};

public:
    RVCInference(const std::string& model_path) 
        : env_(ORT_LOGGING_LEVEL_WARNING, "RVC") {
        
        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(4);
        
        // GPU поддержка (CUDA)
        #ifdef USE_CUDA
        OrtCUDAProviderOptions cuda_options;
        cuda_options.device_id = 0;
        session_options.AppendExecutionProvider_CUDA(cuda_options);
        #endif
        
        // CPU fallback
        session_options.AppendExecutionProvider_CPU();
        
        session_ = Ort::Session(env_, model_path.c_str(), session_options);
        
        std::cout << "✓ RVC model loaded from " << model_path << std::endl;
    }
    
    std::vector<float> convert_voice(
        const std::vector<float>& hubert_features,  // [44, 768]
        const std::vector<float>& f0_contour,       // [44]
        int speaker_id) {
        
        // Reshape inputs
        std::vector<int64_t> hubert_shape = {1, 44, 768};
        std::vector<int64_t> f0_shape = {1, 44};
        std::vector<int64_t> speaker_shape = {1};
        
        // Create input tensors
        auto memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        
        Ort::Value input_tensors[] = {
            Ort::Value::CreateTensor<float>(
                memory_info, (float*)hubert_features.data(), 
                hubert_features.size(), 
                hubert_shape.data(), hubert_shape.size()),
            
            Ort::Value::CreateTensor<float>(
                memory_info, (float*)f0_contour.data(), 
                f0_contour.size(), 
                f0_shape.data(), f0_shape.size()),
            
            Ort::Value::CreateTensor<int64_t>(
                memory_info, &speaker_id, 1, 
                speaker_shape.data(), speaker_shape.size())
        };
        
        // Run inference
        auto output_tensors = session_.Run(
            Ort::RunOptions{nullptr},
            input_names_, input_tensors, 3,
            output_names_, 1
        );
        
        // Extract output
        float* output_data = output_tensors[0].GetTensorMutableData<float>();
        auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t output_size = output_shape[1];  // num_samples
        
        std::vector<float> result(output_data, output_data + output_size);
        
        return result;
    }
};
```

### 4.6 GPU/CPU Fallback Strategy

```cpp
class RVCBackend {
public:
    enum Device { CUDA, METAL, DIRECTML, CPU };
    
    RVCBackend(const std::string& model_path) {
        // Попытка загрузить с CUDA (NVIDIA)
        if (has_cuda_gpu()) {
            std::cout << "Using CUDA GPU" << std::endl;
            inference_ = new RVCInference(model_path, Device::CUDA);
            return;
        }
        
        // Fallback на Metal (macOS)
        #ifdef __APPLE__
        if (has_metal_gpu()) {
            std::cout << "Using Metal GPU (macOS)" << std::endl;
            inference_ = new RVCInference(model_path, Device::METAL);
            return;
        }
        #endif
        
        // Fallback на DirectML (Windows)
        #ifdef _WIN32
        if (has_directml_support()) {
            std::cout << "Using DirectML GPU (Windows)" << std::endl;
            inference_ = new RVCInference(model_path, Device::DIRECTML);
            return;
        }
        #endif
        
        // Финальный fallback на CPU
        std::cout << "Using CPU inference (slower)" << std::endl;
        inference_ = new RVCInference(model_path, Device::CPU);
    }

private:
    RVCInference* inference_;
    
    bool has_cuda_gpu() {
        // Проверяем наличие NVIDIA GPU и CUDA toolkit
        cudaDeviceProp prop;
        return cudaGetDeviceProperties(&prop, 0) == cudaSuccess;
    }
};
```

### 4.7 Размеры моделей

| Компонент | Размер | Примечание |
|-----------|--------|-----------|
| **HuBERT base** | 350-500MB | Feature extraction, CPU inference |
| **HuBERT large** | 1.2-1.5GB | Более точный, но медленнее |
| **VITS Vocoder** | 35-45MB | Синтез аудио, GPU preferred |
| **FAISS Index** | 10-50MB/model | Per-speaker retrieval index |
| **Total per model** | ~400-600MB | С HuBERT + VITS + FAISS |

### 4.8 Лицензии

| Компонент | Лицензия | Цена |
|-----------|----------|------|
| RVC v3 | AGPL-3.0 | Свободно (комплиментарно) |
| HuBERT | CC BY-NC | Некоммерческое использование |
| VITS | Apache 2.0 | Коммерческое OK |
| FAISS | MIT | Коммерческое OK |

---

## 5. Генерация бэк-вокала

### 5.1 Drone (на fundamental frequency)

**Алгоритм:** осциллятор, модулируемый ADSR envelope.

```python
import numpy as np

def generate_drone(f0_contour: np.ndarray, sr: int, 
                  duration: float, waveform_type: str = 'sine',
                  amplitude: float = 0.3) -> np.ndarray:
    """
    Генерируем drone на основной частоте с волнообразной модуляцией.
    
    Args:
        f0_contour: Frequency contour from pitch detection (Hz)
        sr: Sample rate
        duration: Duration in seconds
        waveform_type: 'sine', 'triangle', 'sawtooth', 'square'
        amplitude: -48dB to -6dB рекомендуется для бэка
    """
    
    n_samples = int(sr * duration)
    phase = np.zeros(n_samples)
    output = np.zeros(n_samples)
    
    # Интерполируем f0_contour на n_samples
    if len(f0_contour) != n_samples:
        f0_interp = np.interp(
            np.linspace(0, 1, n_samples),
            np.linspace(0, 1, len(f0_contour)),
            f0_contour
        )
    else:
        f0_interp = f0_contour
    
    # Генерируем осциллятор
    dt = 1.0 / sr
    for i in range(n_samples):
        phase[i] = 2 * np.pi * f0_interp[i] * i * dt
        
        if waveform_type == 'sine':
            output[i] = np.sin(phase[i])
        elif waveform_type == 'triangle':
            output[i] = 2 * np.abs(2 * (phase[i] / (2*np.pi) - 
                         np.floor(phase[i] / (2*np.pi) + 0.5))) - 1
        elif waveform_type == 'sawtooth':
            output[i] = 2 * (phase[i] / (2*np.pi) - 
                       np.floor(phase[i] / (2*np.pi) + 0.5))
        elif waveform_type == 'square':
            output[i] = 1.0 if np.sin(phase[i]) > 0 else -1.0
    
    # ADSR envelope для мягкого входа/выхода
    attack_samples = int(0.05 * sr)   # 50ms
    release_samples = int(0.1 * sr)   # 100ms
    
    envelope = np.ones(n_samples)
    # Attack
    envelope[:attack_samples] = np.linspace(0, 1, attack_samples)
    # Release
    envelope[-release_samples:] = np.linspace(1, 0, release_samples)
    
    output *= envelope * amplitude
    
    return output
```

### 5.2 Терция/квинта/октава (текущий путь: Seed-VC offline)

Текущий MVP-spike не использует WORLD/RVC для бэк-вокалов, потому что
прослушивания показали слишком сильную vocoder/DDSP окраску. Для кнопки
`AI BACKS` выбран Seed-VC zero-shot backend.

```text
Selected vocal track
    │
    ├─ SyntheticObsidianProcessor::renderSeedVCBackVocals()
    │       background thread only
    │
    ├─ SeedVCBridge::render()
    │       Python subprocess, StringArray args
    │
    ├─ run_seed_vc_file.py
    │       intervals: +3, +4, +7, +12
    │
    ├─ polish_outputs.py --mode headroom --target-peak-db -3.0
    │       soundfile I/O, no torchaudio.load
    │
    └─ TrackManagerPanel::addGeneratedAudioTrack()
            adds polished files as Back Vox tracks
```

Implementation notes:

- Use `juce::StringArray` for `juce::ChildProcess::start`. Manual command
  quoting broke absolute paths when launched from the Standalone app.
- `polish_outputs.py` reads WAV with `soundfile.read(always_2d=True,
  dtype="float32")`; `torchaudio.load` can fail via `torchcodec` in the current
  Seed-VC venv.
- The UI consumes the `headroom_only/` output tree. Raw Seed-VC output remains
  in `raw/` for debugging.

### 5.2.1 Legacy concept: Терция/квинта/октава через WORLD/RVC

```python
import pyworld
import librosa

def generate_harmony_voices(main_vocal: np.ndarray, sr: int,
                           intervals: list = [4, 7, 12],  # semitones
                           rvc_model_path: str = None) -> dict:
    """
    Генерируем гармонические голоса (терция, квинта, октава).
    
    Args:
        main_vocal: Main vocal audio
        sr: Sample rate
        intervals: List of interval in semitones [3rd, 5th, octave]
                   4 semitones = major third
                   7 semitones = perfect fifth
                   12 semitones = octave
        rvc_model_path: Path to RVC model for voice conversion
    
    Returns:
        Dictionary with shifted voices
    """
    
    # Pitch detection
    f0, t = pyworld.harvest(main_vocal, sr)
    sp = pyworld.cheaptrick(main_vocal, f0, t, sr)
    ap = pyworld.d4c(main_vocal, f0, t, sr)
    
    harmony_voices = {}
    
    for interval in intervals:
        # Транспонируем через WORLD
        shift_ratio = 2 ** (interval / 12.0)
        shifted_f0 = f0 * shift_ratio
        
        # Синтезируем с новой частотой
        shifted_audio = pyworld.synthesize(shifted_f0, sp, ap, sr)
        
        # Опционально: Voice conversion для натуральности
        if rvc_model_path:
            # Экстрактим HuBERT embedding и делаем RVC conversion
            # (детали в разделе 4)
            pass
        
        harmony_voices[f'{interval}_semitones'] = shifted_audio
    
    return harmony_voices
```

### 5.3 Дабл вокала: микро-вариации

```python
def generate_double_vocal(main_vocal: np.ndarray, sr: int,
                         timing_variance_ms: float = 30,
                         pitch_variance_cents: float = 20,
                         formant_shift: float = 0.05) -> np.ndarray:
    """
    Создаём дабл (second take) с микро-вариациями:
    - Timing: ±20-50ms
    - Pitch: ±10-30 cents
    - Formant: ±0.05
    """
    
    # 1. Timing variance (фленжер эффект)
    timing_shift_samples = int((timing_variance_ms / 1000) * sr * np.random.randn())
    
    if timing_shift_samples > 0:
        doubled = np.concatenate([
            np.zeros(timing_shift_samples),
            main_vocal[:-timing_shift_samples]
        ])
    elif timing_shift_samples < 0:
        doubled = np.concatenate([
            main_vocal[-timing_shift_samples:],
            np.zeros(-timing_shift_samples)
        ])
    else:
        doubled = main_vocal.copy()
    
    # 2. Pitch variance (небольшой shift)
    pitch_shift = np.random.randn() * pitch_variance_cents
    
    f0, t = pyworld.harvest(doubled, sr)
    sp = pyworld.cheaptrick(doubled, f0, t, sr)
    ap = pyworld.d4c(doubled, f0, t, sr)
    
    shift_ratio = 2 ** (pitch_shift / 1200.0)
    shifted_f0 = f0 * shift_ratio
    
    doubled = pyworld.synthesize(shifted_f0, sp, ap, sr)
    
    # 3. Formant shifting (LPC-based)
    frame_length = int(sr * 0.02)
    for i in range(0, len(doubled) - frame_length, frame_length):
        frame = doubled[i:i+frame_length]
        
        # LPC анализ
        from scipy.signal import lpc
        coeffs, _ = lpc(frame, order=12)
        
        # Warping коэффициентов
        warping_factor = 1.0 + formant_shift * np.random.randn() * 0.02
        warped_coeffs = coeffs.copy()
        # (упрощение: реальный frequency warping более сложный)
        
        doubled[i:i+frame_length] = frame
    
    # 4. Усиливаем басы немного для "полноты"
    # (frequency-weighted random EQ)
    
    return doubled
```

### 5.4 Синхронизация с основным вокалом

```python
def synchronize_harmony_with_main(main_vocal: np.ndarray,
                                 harmony_vocals: dict,
                                 sr: int,
                                 crossfade_ms: float = 20) -> np.ndarray:
    """
    Синхронизируем гармоничные голоса с основным:
    - Выравниваем длину
    - Кроссфейд для мягкого входа/выхода
    - Компенсируем задержку WORLD обработки (~5-10ms)
    """
    
    max_length = len(main_vocal)
    
    # Pad/trim все голоса к одной длине
    synced_harmony = {}
    for name, audio in harmony_vocals.items():
        if len(audio) > max_length:
            audio = audio[:max_length]
        elif len(audio) < max_length:
            audio = np.concatenate([audio, np.zeros(max_length - len(audio))])
        
        synced_harmony[name] = audio
    
    # Crossfade на границах
    crossfade_samples = int((crossfade_ms / 1000) * sr)
    fade_in = np.linspace(0, 1, crossfade_samples)
    fade_out = np.linspace(1, 0, crossfade_samples)
    
    for name in synced_harmony:
        audio = synced_harmony[name]
        audio[:crossfade_samples] *= fade_in
        audio[-crossfade_samples:] *= fade_out
        synced_harmony[name] = audio
    
    return synced_harmony
```

---

## 6. Таблица моделей

| Модель | Задача | Размер | Latency | GPU | Лицензия | Замечания |
|--------|--------|--------|---------|-----|----------|-----------|
| **HuBERT base** | Feature extraction | 370MB | ~30ms | CPU | CC BY-NC | 12 слоёв, 768D embedding |
| **HuBERT large** | Feature extraction (точнее) | 1.2GB | ~50ms | Optional | CC BY-NC | 24 слоя, 1024D |
| **CREPE** | Pitch detection | 121MB | ~50ms | GPU | MIT | CNN-based, ±5-10 cents |
| **PENN** | Pitch detection (лучше) | 200MB | ~30ms | GPU | AGPL | CRNN-based, ±2-5 cents |
| **pYIN (librosa)** | Pitch detection (fast) | ~5MB | <5ms | CPU | BSD-3 | Probabilistic, real-time |
| **WORLD vocoder** | Pitch shifting, resynthesis | ~2MB | ~20ms | CPU | Modified BSD | Отличное качество |
| **VITS** | Neural vocoder (synthesis) | 45MB | ~100ms | GPU | Apache 2.0 | GAN-based, MOS 4.5 |
| **RVC v3** | Voice conversion | ~500MB total | ~100-200ms | GPU | AGPL-3.0 | HuBERT+FAISS+VITS |
| **Essentia** | Tonal analysis, chroma | ~100MB | ~100ms | CPU | AGPL-3.0 | Harmonic pitch detection |
| **madmom RNN** | Beat/tempo detection | ~50MB | ~200ms | GPU | BSD | RNN trained on 600+ hrs |
| **librosa tempogram** | Tempo detection | ~5MB | ~100ms | CPU | ISC | Autocorrelation-based |

---

## 7. Python-C++ интеграция

### 7.1 Три уровня интеграции

```
Уровень 1: C++ вызывает Python функции через pybind11
  - Обучение RVC (offline)
  - Анализ guide трека
  - Feature extraction (HuBERT)

Уровень 2: Python запускается как subprocess
  - Fallback если GIL блокирует
  - Separate процесс для вычислительно тяжёлых задач

Уровень 3: ONNX Runtime в C++
  - Real-time inference (pitch, RVC)
  - GPU support (CUDA, Metal, DirectML)
```

### 7.2 pybind11 настройка

**CMakeLists.txt:**

```cmake
cmake_minimum_required(VERSION 3.16)
project(SyntheticObsidian)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Найти Python
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# Добавить pybind11
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/pybind11)

# Основной executable
add_executable(synthetic_obsidian 
    src/main.cpp
    src/audio_processor.cpp
    src/ui/plugin.cpp
)

# Python module для анализа
pybind11_add_module(obsidian_analysis
    src/python/analysis_bindings.cpp
)

target_link_libraries(obsidian_analysis PRIVATE
    Python3::Python
    ${THIRD_PARTY_LIBS}
)

# Link everything
target_link_libraries(synthetic_obsidian PRIVATE
    obsidian_analysis
    onnxruntime
    ${JUCE_LIBRARIES}
)
```

**src/python/analysis_bindings.cpp:**

```cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

namespace py = pybind11;

class PyAudioAnalyzer {
public:
    std::map<std::string, py::object> analyzeGuideTrack(
        const std::string& audioPath,
        int sampleRate = 44100
    ) {
        // Acquire GIL (Global Interpreter Lock)
        py::gil_scoped_acquire acquire;
        
        try {
            auto analysis = py::module::import("obsidian.analysis.guide");
            
            // Вызываем Python функцию
            auto tonality = analysis.attr("analyze_tonality")(audioPath);
            auto bpm = analysis.attr("analyze_bpm")(audioPath);
            auto meter = analysis.attr("analyze_meter")(
                bpm.attr("__getitem__")("beat_times")
            );
            
            std::map<std::string, py::object> result;
            result["tonality"] = tonality;
            result["bpm"] = bpm;
            result["meter"] = meter;
            
            return result;
            
        } catch (const py::error_already_set& e) {
            throw std::runtime_error(
                std::string("Python error: ") + e.what()
            );
        }
    }
    
    py::array_t<float> extractHuBERTFeatures(
        const std::string& audioPath
    ) {
        py::gil_scoped_acquire acquire;
        
        try {
            auto feature_ext = py::module::import("obsidian.features.hubert");
            auto features = feature_ext.attr("extract_features")(audioPath);
            
            // Convert Python array to numpy
            return py::cast<py::array_t<float>>(features);
            
        } catch (const std::exception& e) {
            throw std::runtime_error(e.what());
        }
    }
};

PYBIND11_MODULE(obsidian_analysis, m) {
    m.doc() = "Synthetic Obsidian Audio Analysis Module";
    
    py::class_<PyAudioAnalyzer>(m, "AudioAnalyzer")
        .def(py::init<>())
        .def("analyze_guide_track", &PyAudioAnalyzer::analyzeGuideTrack)
        .def("extract_hubert_features", &PyAudioAnalyzer::extractHuBERTFeatures);
}
```

### 7.3 Thread Safety & Lock-Free Queues

```cpp
#include <concurrent_queue.h>  // TBB concurrent_queue
#include <thread>
#include <mutex>

// Lock-free очередь для передачи данных между потоками
class AudioAnalysisQueue {
private:
    tbb::concurrent_queue<AudioFrame> analysis_queue_;
    tbb::concurrent_queue<AnalysisResult> result_queue_;
    std::atomic<bool> should_exit_{false};
    std::thread analysis_thread_;
    
    void analysis_worker() {
        AudioFrame frame;
        while (!should_exit_) {
            // Non-blocking pop
            if (analysis_queue_.try_pop(frame)) {
                // GIL-safe Python call
                py::gil_scoped_acquire acquire;
                auto result = analyze_frame(frame);
                result_queue_.push(result);
            } else {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5)
                );
            }
        }
    }

public:
    AudioAnalysisQueue() {
        analysis_thread_ = std::thread(&AudioAnalysisQueue::analysis_worker, this);
    }
    
    ~AudioAnalysisQueue() {
        should_exit_ = true;
        if (analysis_thread_.joinable()) {
            analysis_thread_.join();
        }
    }
    
    void push_frame(const AudioFrame& frame) {
        analysis_queue_.push(frame);
    }
    
    bool try_get_result(AnalysisResult& result) {
        return result_queue_.try_pop(result);
    }
};
```

### 7.4 Subprocess Fallback

```cpp
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class PythonSubprocessAnalyzer {
public:
    json analyzeGuideTrack(const std::string& audioPath) {
        // Вызываем Python script как subprocess
        std::string cmd = std::string("python3 -c \"") +
            "import json; "
            "from obsidian.analysis.guide import analyze_tonality, analyze_bpm; "
            "result = {"
            "    'tonality': analyze_tonality('" + audioPath + "'), "
            "    'bpm': analyze_bpm('" + audioPath + "')"
            "}; "
            "print(json.dumps(result))\"";
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) throw std::runtime_error("popen failed");
        
        std::string output;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        pclose(pipe);
        
        return json::parse(output);
    }
};
```

---

## 8. Деплой моделей

### 8.1 Структура папок

```
~/.synthetic_obsidian/
├── models/
│   ├── hubert/
│   │   ├── hubert-base.onnx (370MB)
│   │   └── config.json
│   ├── vits/
│   │   ├── vits-vocoder.onnx (45MB)
│   │   └── config.json
│   ├── crepe/
│   │   └── crepe-full.onnx (121MB)
│   ├── rvc/
│   │   └── [user-trained-models]/
│   │       ├── model.pth (500MB+)
│   │       ├── hubert.onnx (reused)
│   │       ├── vits.onnx (reused)
│   │       └── faiss.index (10-50MB)
│   └── essentia/
│       └── models.db (small)
├── cache/
│   ├── guide_analysis_cache.json
│   └── processed_audio/
├── config.json
└── logs/
    └── obsidian.log
```

### 8.2 First-Run Wizard

```python
import os
import urllib.request
import json
from pathlib import Path

class ModelDownloader:
    """Скачиваем необходимые модели при первом запуске."""
    
    MODELS = {
        'hubert-base': {
            'url': 'https://huggingface.co/facebook/hubert-base-ls960/resolve/main/model.onnx',
            'size_mb': 370,
            'destination': 'models/hubert/hubert-base.onnx'
        },
        'vits': {
            'url': 'https://huggingface.co/datasets/artifician/vits-models/resolve/main/vits.onnx',
            'size_mb': 45,
            'destination': 'models/vits/vits-vocoder.onnx'
        },
        'crepe': {
            'url': 'https://huggingface.co/spaces/openai/whisper/resolve/main/models/crepe.onnx',
            'size_mb': 121,
            'destination': 'models/crepe/crepe-full.onnx'
        },
    }
    
    def __init__(self, obsidian_dir: str = None):
        if obsidian_dir is None:
            obsidian_dir = os.path.expanduser('~/.synthetic_obsidian')
        self.obsidian_dir = Path(obsidian_dir)
        self.obsidian_dir.mkdir(parents=True, exist_ok=True)
    
    def download_required_models(self, progress_callback=None):
        """Скачиваем минимальный набор моделей."""
        
        manifest = self.obsidian_dir / 'models_manifest.json'
        
        for model_name, info in self.MODELS.items():
            model_path = self.obsidian_dir / info['destination']
            
            # Пропускаем, если уже есть
            if model_path.exists():
                print(f"✓ {model_name} already downloaded")
                continue
            
            # Создаём директорию
            model_path.parent.mkdir(parents=True, exist_ok=True)
            
            # Скачиваем
            print(f"Downloading {model_name} ({info['size_mb']}MB)...")
            
            def download_progress(block_num, block_size, total_size):
                downloaded = block_num * block_size
                if progress_callback:
                    progress_callback(
                        model_name,
                        downloaded,
                        total_size
                    )
            
            urllib.request.urlretrieve(
                info['url'],
                model_path,
                reporthook=download_progress
            )
            
            print(f"✓ {model_name} downloaded")
        
        # Сохраняем manifest
        manifest_data = {
            'downloaded_models': list(self.MODELS.keys()),
            'total_size_mb': sum(m['size_mb'] for m in self.MODELS.values())
        }
        manifest.write_text(json.dumps(manifest_data, indent=2))

# Вызов при первом запуске плагина
def initialize_models():
    downloader = ModelDownloader()
    downloader.download_required_models()
```

### 8.3 Механизм обновления моделей

```python
class ModelManager:
    """Управление обновлениями моделей."""
    
    def check_for_updates(self):
        """Проверяем обновления для каждой модели."""
        
        # Читаем manifest
        manifest_path = Path.home() / '.synthetic_obsidian' / 'models_manifest.json'
        with open(manifest_path) as f:
            current = json.load(f)
        
        # Скачиваем remote manifest
        remote_manifest_url = 'https://obsidian-models.s3.amazonaws.com/manifest.json'
        
        import urllib.request
        with urllib.request.urlopen(remote_manifest_url) as response:
            remote = json.load(response)
        
        updates_available = []
        for model_name, remote_info in remote.items():
            current_info = current.get(model_name, {})
            
            if remote_info.get('version') > current_info.get('version', 0):
                updates_available.append({
                    'name': model_name,
                    'current_version': current_info.get('version'),
                    'new_version': remote_info['version'],
                    'size_mb': remote_info['size_mb']
                })
        
        return updates_available
    
    def apply_update(self, model_name: str):
        """Скачиваем и устанавливаем обновление модели."""
        
        # Скачиваем новую версию
        downloader = ModelDownloader()
        # (реализация аналогична download_required_models)
        
        # Backup старой версии
        old_model_path = downloader.obsidian_dir / self.MODELS[model_name]['destination']
        backup_path = old_model_path.with_suffix('.backup')
        old_model_path.rename(backup_path)
        
        # Скачиваем новую
        downloader.download_required_models()
        
        print(f"✓ Updated {model_name}")
```

### 8.4 Минимальный бандл

```
Synthetic Obsidian VST Distribution
├── synthetic_obsidian.vst3 (~5MB, no models)
├── models/ (bundled, ~300MB total)
│   ├── hubert-base.onnx (370MB → compressed 150MB)
│   ├── vits-vocoder.onnx (45MB → 20MB)
│   ├── crepe-full.onnx (121MB → 50MB)
│   └── essentia.db (small)
└── first_run.exe (downloads additional models from CDN)

Total: ~330MB download
After extraction + CDN downloads: ~600MB SSD
```

---

## 9. Альтернативы RVC

| Метод | Плюсы | Минусы | Рекомендация |
|-------|-------|--------|--------------|
| **RVC v3** | SOTA quality, fast, open-source | AGPL-3.0, GPU optional | ✅ **Выбираем это** |
| **SVC** | Good quality, older | Slower training, smaller community | For comparison |
| **so-vits-svc** | Novel architecture | Complex setup, less stable | Experimental |
| **Diff-SVC** | Diffusion-based, smooth | Very slow (real-time unfeasible) | Research only |
| **Vall-E** | Microsoft SOTA | Proprietary, no open source | Future consideration |

**Сравнительная таблица:**

```python
comparison = {
    'RVC v3': {
        'training_time_min_50min_audio': 30,  # minutes
        'inference_latency_ms': 100,
        'quality_mos': 4.2,
        'voice_naturalness': 4.5,
        'training_data_min_min': 5,
        'rtf_real_time_factor': 0.5,  # 2x faster than real-time
        'cpu_inference': True,
        'gpu_optional': True
    },
    'SVC': {
        'training_time_min_50min_audio': 60,
        'inference_latency_ms': 200,
        'quality_mos': 3.8,
        'voice_naturalness': 4.0,
        'training_data_min_min': 10,
        'rtf_real_time_factor': 1.5,  # Slower than real-time
        'cpu_inference': False,
        'gpu_required': True
    },
    'so-vits-svc': {
        'training_time_min_50min_audio': 45,
        'inference_latency_ms': 150,
        'quality_mos': 4.0,
        'voice_naturalness': 4.1,
        'training_data_min_min': 7,
        'rtf_real_time_factor': 0.8,
        'cpu_inference': False,
        'gpu_required': True
    }
}
```

**Рекомендация: RVC v3** — лучший выбор для гибридной VST архитектуры.

---

## 10. Риски и митигация

### 10.1 Технические риски

| Риск | Вероятность | Impact | Митигация |
|------|------------|--------|-----------|
| GIL блокирует real-time audio | HIGH | Critical | Subprocess для Python, ONNX для inference |
| GPU crash при переключении моделей | MEDIUM | High | Graceful fallback на CPU, state machine |
| Memory leak в pybind11 | MEDIUM | High | Профилирование с Valgrind, RAII patterns |
| ONNX Runtime version incompatibility | MEDIUM | Medium | Version pinning, vendor bundling |
| Model format obsolescence (torch→ONNX) | LOW | Medium | Keep PyTorch checkpoints, periodic re-export |

### 10.2 Лицензионные риски

| Компонент | Лицензия | Риск | Решение |
|-----------|----------|------|---------|
| RVC | AGPL-3.0 | Copyleft | Лицензирование под GPL-3.0 (OSS OK) или proprietary fork |
| HuBERT | CC BY-NC | Некоммерческое only | Не использовать в коммерческих продуктах или лицензировать |
| VITS | Apache 2.0 | Low | Коммерческое использование OK |
| Essentia | AGPL-3.0 | Copyleft | Выше |

### 10.3 Performance риски

```python
# Мониторим latency каждого компонента
class PerformanceMonitor:
    def __init__(self):
        self.timings = {}
    
    def record_latency(self, component: str, latency_ms: float):
        if component not in self.timings:
            self.timings[component] = []
        self.timings[component].append(latency_ms)
    
    def get_stats(self):
        stats = {}
        for component, latencies in self.timings.items():
            stats[component] = {
                'mean_ms': np.mean(latencies),
                'p95_ms': np.percentile(latencies, 95),
                'p99_ms': np.percentile(latencies, 99),
                'max_ms': np.max(latencies),
                'min_ms': np.min(latencies)
            }
        return stats

# Budget для каждого компонента
LATENCY_BUDGET = {
    'pitch_detection': 50,      # 50ms
    'feature_extraction': 30,   # 30ms
    'voice_conversion': 100,    # 100ms
    'harmonic_generation': 20,  # 20ms
    'mixing': 10,               # 10ms
    'total_rtf': 0.5           # Real-time factor
}
```

---

## 11. Roadmap (4 месяца)

### Месяц 1: Foundation & Analysis

**Неделя 1-2: Infrastructure**
- [ ] Настроить pybind11, ONNX Runtime
- [ ] Реализовать lock-free audio queue
- [ ] Setup Python environment & packaging

**Неделя 3-4: Guide Track Analysis**
- [ ] Имплементить Essentia HPCP + key detection
- [ ] Интегрировать madmom + librosa для BPM
- [ ] Детекция модуляций (frame-by-frame chord analysis)
- [ ] Unit тесты для анализа

**Deliverables:** Working guide track analysis, confidence scores

### Месяц 2: Pitch Correction

**Неделя 1-2: Pitch Detection**
- [ ] Интегрировать CREPE (ONNX)
- [ ] Реализовать pYIN fallback
- [ ] Kalman smoothing filter

**Неделя 3-4: Pitch Shifting**
- [ ] WORLD vocoder integration (Python)
- [ ] LPC формант preservation
- [ ] Test on vocal samples

**Deliverables:** Real-time pitch detection + shifting, no artifacts

### Месяц 3: RVC Integration

**Неделя 1-2: Training Pipeline**
- [ ] RVC training script
- [ ] ONNX export (HuBERT, VITS, FAISS)
- [ ] Model validation

**Неделя 3-4: Inference in C++**
- [ ] ONNX Runtime C++ API
- [ ] GPU support (CUDA + CPU fallback)
- [ ] Integration with DSP layer

**Deliverables:** Working RVC voice conversion, trained on test voice

### Месяц 4: Polish & Deployment

**Неделя 1: Harmony Generation**
- [ ] Drone synthesis
- [ ] Tertia/quinta/octava generation
- [ ] Synchronization with main vocal

**Неделя 2: Model Deployment**
- [ ] First-run wizard
- [ ] Model downloader
- [ ] Update mechanism

**Неделя 3: Testing & Optimization**
- [ ] Performance profiling
- [ ] Latency benchmarking
- [ ] Memory leak checks

**Неделя 4: Release**
- [ ] Final documentation
- [ ] VST3 certification
- [ ] Release build

**Deliverables:** Production-ready Synthetic Obsidian VST

---

## Заключение

Synthetic Obsidian объединяет три мощные технологии:
1. **Классический DSP** (WORLD, фазовый вокодер) для real-time обработки
2. **Deep Learning** (HuBERT, VITS, RVC) для качества и переменности
3. **Гибридная архитектура** с graceful fallback на CPU

Это позволяет создать профессиональный VST плагин, который работает как на high-end студийных машинах, так и на ноутбуках без GPU.

**Ключевые метрики успеха:**
- Pitch detection accuracy: ±5 cents
- Latency budget: <100ms total
- Voice conversion quality: MOS 4.0+
- Training time: 30 мин для 50 мин audio
- CPU fallback: всегда доступен
