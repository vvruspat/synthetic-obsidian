# Synthetic Obsidian — Technical Architecture Document

**Project:** Synthetic Obsidian VST3/ARA2 Vocal Processing Plugin  
**Version:** 1.0  
**Date:** 2026-04-11  
**Author:** Senior Audio Software Developer (Developer 1)

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Technology Stack](#2-technology-stack)
3. [System Modules](#3-system-modules)
4. [RVC Integration Strategy](#4-rvc-integration-strategy)
5. [ARA2 Implementation](#5-ara2-implementation)
6. [Pitch Detection & Correction](#6-pitch-detection--correction)
7. [Cross-Platform Build Configuration](#7-cross-platform-build-configuration)
8. [Risks & Mitigation](#8-risks--mitigation)
9. [Development Phases & Timeline](#9-development-phases--timeline)
10. [Data Flow Diagrams](#10-data-flow-diagrams)

---

## 1. Architecture Overview

### 1.1 High-Level System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         DAW Host (Logic, Cubase, etc.)             │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                     ARA2 Host Interface                      │  │
│  │  (AudioSource, PlaybackRegion, AudioModification)           │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────┬───────────────────────────────────────┘
                              │
        ┌─────────────────────┴──────────────────────┐
        │                                            │
┌───────▼───────────────────────────────────────────▼──────────────┐
│             SYNTHETIC OBSIDIAN Plugin Container                  │
│                   (JUCE AudioProcessor)                          │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │              PLUGIN_UI_LAYER                               │ │
│  │  ┌─────────────┐ ┌──────────────┐ ┌─────────────────┐   │ │
│  │  │ Piano Roll  │ │ Analysis     │ │ Voice Preset    │   │ │
│  │  │ Editor      │ │ Visualizer   │ │ Manager         │   │ │
│  │  └─────────────┘ └──────────────┘ └─────────────────┘   │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │           CORE_PROCESSING_LAYER                            │ │
│  │                                                            │ │
│  │  ┌──────────────────────────────────────────────────────┐ │ │
│  │  │          AudioEngine (JUCE AudioProcessor)           │ │ │
│  │  │  • Buffer management                                 │ │ │
│  │  │  • Real-time DSP routing                             │ │ │
│  │  │  • Latency compensation                              │ │ │
│  │  └──────────────────────────────────────────────────────┘ │ │
│  │                         │                                  │ │
│  │      ┌──────────────────┼──────────────────┐              │ │
│  │      │                  │                  │              │ │
│  │  ┌───▼──────┐  ┌──────▼──────┐  ┌────────▼────┐         │ │
│  │  │ Analysis │  │ Pitch       │  │  Voice      │         │ │
│  │  │ Module   │  │ Correction  │  │  Preset     │         │ │
│  │  │          │  │ Module      │  │  Module     │         │ │
│  │  └──────────┘  └─────────────┘  └─────────────┘         │ │
│  │      │              │                  │                 │ │
│  │      ▼              ▼                  ▼                 │ │
│  │  ┌─────────────────────────────────────────────────────┐ │ │
│  │  │      Harmony Generation Module                      │ │ │
│  │  │  (Drone, 3rd, 5th, Octave, Double synthesis)       │ │ │
│  │  └─────────────────────────────────────────────────────┘ │ │
│  │                      │                                   │ │
│  │                      ▼                                   │ │
│  │  ┌─────────────────────────────────────────────────────┐ │ │
│  │  │         Export Module                               │ │ │
│  │  │  (WAV, MIDI, ARA2 modifications)                   │ │ │
│  │  └─────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │           AI_INFERENCE_LAYER                               │ │
│  │                                                            │ │
│  │  ┌──────────────────────────────────────────────────────┐ │ │
│  │  │      RVC Python Bridge (Embedded Python)             │ │ │
│  │  │  • Model loading & inference                         │ │ │
│  │  │  • Feature extraction                                │ │ │
│  │  │  • Voice preset training                             │ │ │
│  │  │  • GPU/CPU management                                │ │ │
│  │  └──────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │           STORAGE_LAYER                                    │ │
│  │  • Project state (tonal key, BPM, size)                   │ │
│  │  • Voice presets (RVC models)                             │ │
│  │  • MIDI notes & automation                                │ │
│  │  • Cache (pitch curves, analysis results)                 │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 Component Interaction Flow

**Initialization Phase:**
1. DAW loads plugin → JUCE AudioProcessor initialization
2. RVC Python environment initialized (lazy-load on first use)
3. DSP algorithms instantiated with sample rate/buffer size

**Audio Processing Loop (Real-time):**
1. DAW provides audio buffers → AudioEngine receives
2. Guide track analysis (if enabled) → AnalysisModule
3. Main vocal input → PitchCorrectionModule (CREPE/pYIN)
4. Corrected pitch → PitchShiftingDSP
5. Harmony voices generated → HarmonyGenerationModule
6. RVC inference (async) → VoicePresetModule
7. Mixed output → ARA2 AudioModification (if ARA2 enabled)
8. Final output to DAW

**User Interaction Flow:**
1. User selects guide track via ARA2 AudioSource
2. AnalysisModule extracts key/BPM/time signature
3. User creates/selects voice preset
4. Training pipeline initiated (if new preset)
5. PianoRollEditor displays analyzed notes or manual entry
6. Export initiated → ExportModule (WAV/MIDI)

---

## 2. Technology Stack

### 2.1 Core Framework

| Component | Library | Version | Purpose |
|-----------|---------|---------|---------|
| **Plugin Framework** | JUCE | 8.0.2 (latest) | VST3, AU, AAX, Standalone host |
| **Build System** | CMake | 3.21+ | Cross-platform build configuration |
| **C++ Standard** | C++20 | — | Modern language features, performance |

**JUCE 8.0.2 Selection Rationale:**
- Full VST3 support with ARA2 extensions
- Comprehensive DSP module (IIR filters, FFT, windowing)
- Active community, regular updates
- JUCE Pro license recommended for ARA2 deeper integration

### 2.2 DSP & Audio Analysis Libraries

| Component | Library | Version | Why | Trade-offs |
|-----------|---------|---------|-----|-----------|
| **Pitch Detection** | librosa + CREPE (Python) | 0.10+ | Hybrid: CREPE for real-time, librosa for analysis | Python dependency for inference |
| **Pitch Shifting** | Rubberband (librosa) / SoundTouchLib | 3.3+ | Phase-vocoder based, preserves timbre | CPU-intensive, ~100-150ms latency |
| **Alternative: pYIN** | PyAudio Analysis / PYIN C++ port | custom | Faster, lower latency (~30ms) | Less accurate than CREPE |
| **Chord/Key Detection** | Essentia | 2.1 | Professional MIR algorithms | Large library footprint (~50MB) |
| **FFT & DSP Utilities** | JUCE DSP Module + Eigen | 8.0+ / 3.4.0 | Convolution, filtering, matrix operations | Minimal overhead |
| **Audio I/O** | PortAudio (Standalone) | 19.7 | Cross-platform real-time audio | Already in JUCE, no extra dep |

**Concrete Integration Details:**

```cpp
// AudioEngine - Pitch Detection Pipeline
class PitchDetectionChain {
    librosa::FeatureExtractor feature_extractor;  // MFC, spectral flux
    CREPEModel crepe_model;  // PyTorch-based via ONNX Runtime
    pYINDetector fallback_detector;  // Lightweight alternative
    
    PitchContour detect(AudioBuffer<float> vocal_frame) {
        if (gpu_available) {
            return crepe_model.predict(vocal_frame);  // ~10ms on NVIDIA
        } else {
            return fallback_detector.process(vocal_frame);  // ~20ms on CPU
        }
    }
};
```

### 2.3 AI/ML & Voice Cloning

| Component | Technology | Version | Purpose | Integration |
|-----------|-----------|---------|---------|------------|
| **Voice Preset Training** | RVC (Real-time Voice Cloning) | 2.0+ | Train custom voice models from 5-30min audio | Python subprocess |
| **Voice Inference** | RVC + ONNX Runtime | 2.0 / 1.15+ | Real-time voice conversion | Embedded Python + C++ bindings |
| **Feature Extraction** | HuBERT (pre-trained) | facebook/hubert-base | Acoustic features for RVC | Downloaded once, cached |
| **GPU Acceleration** | ONNX Runtime | 1.15.1 | CPU/GPU agnostic inference | Fallback to CPU if no GPU |
| **Python Embedding** | Python.h (CPython API) / Pybind11 | 3.10+ / 2.11 | C++ ↔ Python integration | Pybind11 preferred (cleaner API) |

**RVC Configuration:**

```python
# rvc_engine.py (Python module loaded via Pybind11)
import torch
from rvc.models import RVCModel
from rvc.inference import RVCInference

class RVCBridge:
    def __init__(self, model_path: str, device: str = "cuda"):
        self.model = RVCModel.load(model_path)
        self.inferencer = RVCInference(self.model, device=device)
        self.feature_extractor = HuBERTFeatureExtractor()
    
    def process_chunk(self, audio: np.ndarray, pitch_curve: np.ndarray) -> np.ndarray:
        """
        Process audio chunk with custom pitch contour.
        
        Args:
            audio: [sample_rate * chunk_duration] mono float32
            pitch_curve: Extracted or user-modified pitch in Hz
        
        Returns:
            Converted audio same length/sample rate
        """
        features = self.feature_extractor.extract(audio)
        converted = self.inferencer.convert(
            audio, 
            features, 
            f0_curve=pitch_curve,
            f0_method="crepe"  # Can be crepe, pyin, harvest
        )
        return converted
```

**Version Justification:**
- RVC 2.0+ has improved training pipeline (25 min optimal vs 1 hour in v1)
- ONNX Runtime 1.15+ offers better GPU scheduling
- HuBERT base-size balances accuracy/speed

### 2.4 ARA2 SDK & Integration

| Component | Package | Version | Source |
|-----------|---------|---------|--------|
| **ARA2 SDK** | ARA-SDK | 3.0.3 | https://github.com/Celemony/ARA-SDK |
| **JUCE ARA2 Extension** | JUCE 8.0+ | 8.0.2 | Built-in JUCE_ARA=1 module |
| **DAW Host Bridges** | VST3 SDK | 3.7.11 | Steinberg VST3 Specification |

**Key ARA2 Concepts (C++ mapping):**

```cpp
// In JUCE AudioProcessor
class SyntheticObsidianProcessor : public juce::AudioProcessor, 
                                    public juce::ARA::ARAHostModel {
    
    // ARA2 Document Structure
    juce::ARA::DocumentPtr ara_document;
    juce::ARA::MusicalContextPtr musical_context;  // Key, BPM, time sig
    juce::ARA::AudioSourcePtr guide_track_source;   // Reference audio
    juce::ARA::PlaybackRegionPtr vocal_region;      // Editable region
    juce::ARA::AudioModificationPtr pitch_mod;      // Pitch modifications
    
    void analyzeGuideTrack(AudioBuffer<float>& guide) {
        // Create MusicalContext from guide analysis
        if (!musical_context) {
            musical_context = ara_document->createMusicalContext();
        }
        
        auto key = analyzeKey(guide);
        auto bpm = analyzeTempo(guide);
        auto ts = analyzeTimeSignature(guide);
        
        musical_context->setKey(key);
        musical_context->setTempo(bpm);
        // Time signature update
    }
};
```

### 2.5 Supplementary Libraries

| Library | Version | Use Case | Size |
|---------|---------|----------|------|
| **fmt** (formatting) | 9.1.0 | Logging, string formatting | < 1MB |
| **spdlog** | 1.12.0 | High-performance logging | < 5MB |
| **nlohmann/json** | 3.11.2 | State serialization, presets | < 1MB |
| **Boost (Audio/Asio)** | 1.82.0 | Optional: cross-platform async | ~200MB total |
| **zlib** | 1.2.13 | Compression for model storage | < 2MB |
| **OpenSSL** | 3.0.7 | Secure model download/verification | Bundled |

---

## 3. System Modules

### 3.1 AudioEngine Module

**Responsibility:** Real-time DSP signal flow, buffer management, latency tracking.

**Key Classes:**

```cpp
// synth_obsidian/core/AudioEngine.h
class AudioEngine : public juce::AudioProcessor {
private:
    // DSP Processing Chain
    std::unique_ptr<PitchDetectionEngine> pitch_detector;
    std::unique_ptr<PitchShiftingDSP> pitch_shifter;
    std::unique_ptr<HarmonyGeneratorDSP> harmony_gen;
    
    // Latency compensation
    juce::dsp::DelayLine<float> latency_compensator;
    int compensated_latency_samples = 0;
    
    // RVC Integration
    std::unique_ptr<RVCPythonBridge> rvc_engine;
    juce::AbstractFifo rvc_command_queue;  // Thread-safe queue
    
public:
    // JUCE Interface
    void processBlock(juce::AudioBuffer<float>& buffer, 
                     juce::MidiBuffer& midiMessages) override;
    
    void prepareToPlay(double sample_rate, int blockSize) override;
    
    int getLatencySamples() const override {
        return compensated_latency_samples;
    }
    
    // Custom
    void setAnalysisResult(const AnalysisResult& result);
    void setPitchCorrectionMode(CorrectionMode mode);  // Auto/Manual/Off
};

// synth_obsidian/core/dsp/PitchDetectionEngine.h
class PitchDetectionEngine {
private:
    CREPEModel crepe_gpu;  // Loaded via RVC bridge
    pYINDetector cpu_fallback;
    
public:
    struct PitchFrame {
        float frequency_hz;
        float confidence;  // 0.0-1.0
        int midi_note;
    };
    
    PitchFrame detectFrame(const AudioBuffer<float>& frame) {
        // 512 samples @ 44.1kHz = ~11ms window
        // CREPE ~10ms GPU, pYIN ~20ms CPU
    }
};

// synth_obsidian/core/dsp/PitchShiftingDSP.h
class PitchShiftingDSP {
private:
    RubberBandStretcher stretcher;  // Librosa via Rubberband
    
public:
    void shiftPitch(AudioBuffer<float>& in_out, 
                   const std::vector<float>& pitch_targets_hz,
                   float time_ratio = 1.0f);
};
```

**Latency Budget (Real-time constraint):**

| Stage | Latency | Notes |
|-------|---------|-------|
| Input buffering | ~512 samples (11.6ms @ 44.1kHz) | JUCE block size |
| Pitch detection (CREPE) | ~10-15ms | GPU accelerated |
| Pitch shifting (Rubberband) | 100-150ms | **Critical: Phase vocoder overhead** |
| RVC inference | 20-50ms | Depends on model size |
| Output compensation | Variable | Buffered latency reporting |
| **Total compensation** | **~180-230ms** | Reported to DAW via getLatencySamples() |

**Design Decision:** Latency ≥ 100ms acceptable for a correction/generation plugin (not instrument). DAW will auto-compensate via AudioProcessor::setLatencySamples().

### 3.2 AnalysisModule

**Responsibility:** Extract guide track properties: key, BPM, time signature, chord progression.

**Implementation:**

```cpp
// synth_obsidian/analysis/AnalysisModule.h
class AnalysisModule {
private:
    Essentia::StandardMode essentia_ctx;
    
public:
    struct GuideTrackAnalysis {
        std::string musical_key;           // C major, A minor, etc.
        float tempo_bpm;
        TimeSignature time_signature;      // {4, 4} by default
        std::vector<ChordFrame> chord_progression;
        std::vector<float> spectral_profile;  // For timbral analysis
        bool is_instrumental;
        
        // Confidence scores
        float key_confidence;
        float tempo_confidence;
    };
    
    GuideTrackAnalysis analyzeGuideTrack(
        const AudioBuffer<float>& guide_audio,
        double sample_rate
    );
    
private:
    std::string detectKey(const std::vector<float>& chroma_vector);
    float detectTempo(const AudioBuffer<float>& audio);
    TimeSignature detectTimeSignature(const std::vector<float>& onset_times);
};

// Usage in AnalysisModule::analyzeGuideTrack()
GuideTrackAnalysis AnalysisModule::analyzeGuideTrack(
    const AudioBuffer<float>& guide_audio,
    double sample_rate) {
    
    GuideTrackAnalysis result;
    
    // 1. Chroma Vector Extraction (via Essentia)
    auto chroma = essentia_ctx.extract<std::vector<float>>(
        "ChordsDetectionViaChroma", guide_audio
    );
    
    // 2. Key Detection
    result.musical_key = detectKey(chroma);
    result.key_confidence = essentia_ctx.getConfidence("key");
    
    // 3. Tempo Estimation (Onset-based)
    std::vector<float> onsets = essentia_ctx.extract<std::vector<float>>(
        "OnsetDetection", guide_audio
    );
    result.tempo_bpm = detectTempo(guide_audio);
    result.tempo_confidence = 0.85f;  // Placeholder
    
    // 4. Time Signature (from onset distribution)
    result.time_signature = detectTimeSignature(onsets);
    
    return result;
}
```

**Analysis Constraints:**
- **Blocking operation:** Only on guide track load, ~2-5 seconds for 3-minute audio
- **Essentia:** ~100MB library, but robust key/tempo detection
- **Alternative (lightweight):** Use librosa Python, trade-off C++ compilation size for better key detection

### 3.3 PitchCorrectionModule

**Responsibility:** Auto-tuning with timbre preservation, manual note editing.

**Architecture:**

```cpp
// synth_obsidian/correction/PitchCorrectionModule.h
class PitchCorrectionModule {
public:
    enum class CorrectionMode {
        Off,
        AutoTune,          // Real-time pitch snapping
        ManualCorrection   // User-editable in piano roll
    };
    
    enum class SnapMode {
        Chromatic,         // Snap to all semitones
        Scale,             // Snap to scale degrees (key-aware)
        Semitone,          // Custom interval
        Custom             // User-defined targets
    };
    
private:
    PitchDetectionEngine detector;
    PitchShiftingDSP pitch_shifter;
    SnapMode snap_mode = SnapMode::Scale;
    std::string target_key = "C major";
    
    // Manual note editing
    PianoRoll piano_roll_data;
    
public:
    void processPitchCorrection(
        AudioBuffer<float>& vocal_buffer,
        const AnalysisResult& guide_analysis,
        CorrectionMode mode
    );
    
    std::vector<PitchFrame> detectAndSnap(
        const AudioBuffer<float>& vocal,
        double sample_rate
    );
    
private:
    int snapToScale(int midi_note, const std::string& key);
    int snapToPiano(int midi_note);  // If piano roll active
};

// Concrete snap logic
int PitchCorrectionModule::snapToScale(int midi_note, const std::string& key) {
    // C major: C D E F G A B (0, 2, 4, 5, 7, 9, 11)
    // A minor: A B C D E F G (0, 2, 3, 5, 7, 8, 10)
    
    static const std::map<std::string, std::vector<int>> SCALE_INTERVALS = {
        {"C major", {0, 2, 4, 5, 7, 9, 11}},
        {"A minor", {0, 2, 3, 5, 7, 8, 10}},
        // ... more modes
    };
    
    const auto& scale = SCALE_INTERVALS.at(key);
    int octave = midi_note / 12;
    int note_in_octave = midi_note % 12;
    
    // Find closest scale degree
    int closest = 0;
    int min_distance = 12;
    
    for (int scale_note : scale) {
        int distance = std::abs(note_in_octave - scale_note);
        if (distance < min_distance) {
            min_distance = distance;
            closest = scale_note;
        }
    }
    
    return octave * 12 + closest;
}
```

**⚠️ Ключевое архитектурное решение: AI Resynthesis vs Phase Vocoder (ADR-006)**

Традиционный pitch correction (phase vocoder, Rubberband) при сдвиге на несколько полутонов искажает вокал — форманты тянутся вместе с питчем, переходы звучат механически. **Основной метод offline-коррекции — RVC resynthesis.** Phase vocoder — только DSP fallback когда RVC-пресет не загружен.

**Два пути коррекции:**

| Метод | Когда используется | Качество | Скорость |
|-------|-------------------|----------|----------|
| RVC resynthesis | Голосовой пресет загружен | ★★★★★ Естественно | ~200-500ms/сек CPU, ~20-50ms GPU |
| Phase vocoder (fallback) | Нет пресета | ★★☆☆☆ Артефакты при >1 полутон | ~реалтайм |

**Pipeline offline pitch correction:**

```
YIN detection → origMidiPitch + origCentsOffset
      ↓
Piano Roll edit → targetMidiPitch + targetCentsOffset
      ↓
buildCorrections() → [{startSec, endSec, targetF0_hz[]}]
      ↓
   ┌──────────────────────────────────────────┐
   │  rvcPreset загружен?                     │
   │  ДА → RVCPythonBridge::                  │
   │         resynthesizeAtPitch(             │
   │           segment_audio,                 │
   │           target_f0_contour_hz)          │
   │                                          │
   │  НЕТ → phaseVocoderMono(                 │
   │           segment, pitchRatio)  ← FALLBACK│
   └──────────────────────────────────────────┘
      ↓
crossfade на границах → reload transport
```

**Почему RVC лучше для этой задачи:**
- RVC получает на вход аудио + F0-контур и **пересинтезирует** голос заново на целевой частоте — нейронная сеть знает тембр голоса из обучения и воспроизводит его на новом питче без математического растягивания спектра.
- Переходы между нотами (глайды, консонанты) синтезируются нейросетью естественно — она видит контекст, а не просто скользит по спектру STFT.
- Форманты остаются на своих местах независимо от целевого питча.

**Интеграция в код:**

```cpp
// PitchCorrectionModule::applyOfflineCorrections получает опциональный RVC bridge
void PitchCorrectionModule::applyOfflineCorrections(
    juce::AudioBuffer<float>&          buffer,
    double                             sampleRate,
    const std::vector<NoteCorrection>& corrections,
    RVCPythonBridge*                   rvc = nullptr   // nullptr → DSP fallback
);

// NoteCorrection расширяется целевым F0-контуром для RVC
struct NoteCorrection {
    double startSec;
    double endSec;
    float  pitchRatio;                    // для DSP fallback
    std::vector<float> targetF0Hz;        // для RVC resynthesis (поточечно, hop=512)
};
```

**UI требование:** Если пресет не загружен — показывать предупреждение в Piano Roll:
> ⚠️ "No voice preset loaded. Corrections use DSP fallback (audible artifacts possible on large pitch changes). Load a Voice Preset for AI-quality results."

### 3.4 VoicePresetModule

**Responsibility:** RVC model training, inference, preset management.

**Architecture:**

```cpp
// synth_obsidian/voice/VoicePresetModule.h
class VoicePreset {
public:
    struct Metadata {
        std::string preset_name;         // "Pop Singer #1"
        std::string model_path;          // ~/.synthetic_obsidian/presets/pop1.pth
        float intensity;                 // 0.0 (no change) to 1.0 (full conversion)
        float f0_shift_semitones;        // Pitch adjustment for preset
        std::string description;
        int64_t creation_timestamp;
    };
};

class VoicePresetModule {
private:
    std::unique_ptr<RVCPythonBridge> rvc_bridge;
    std::map<std::string, VoicePreset::Metadata> loaded_presets;
    std::string presets_directory;
    
public:
    // Training
    void startTrainingSession(
        const std::string& training_audio_path,  // 5-30min mono WAV
        const std::string& preset_name,
        TrainingProgressCallback progress_fn
    );
    
    // Inference
    AudioBuffer<float> applyVoicePreset(
        const AudioBuffer<float>& vocal_in,
        const std::string& preset_name,
        float intensity_factor = 1.0f
    );
    
    // Preset management
    std::vector<VoicePreset::Metadata> listPresets();
    void deletePreset(const std::string& preset_name);
    void exportPreset(const std::string& preset_name, 
                     const std::string& export_path);
};

// RVC Python Bridge (Pybind11)
// synth_obsidian/ai/RVCPythonBridge.h
class RVCPythonBridge {
private:
    py::module_ rvc_module;
    py::object rvc_inferencer;
    
public:
    // C++20 constructor
    RVCPythonBridge() {
        rvc_module = py::module_::import("rvc_engine");
    }
    
    std::vector<float> inferenceChunk(
        const std::vector<float>& audio_chunk,
        const std::string& model_path,
        const std::vector<float>& pitch_curve
    ) {
        auto result = rvc_module.attr("process_audio")(
            py::array_t<float>(audio_chunk.size(), audio_chunk.data()),
            model_path,
            py::array_t<float>(pitch_curve.size(), pitch_curve.data())
        );
        
        auto result_buf = result.cast<py::array_t<float>>();
        return std::vector<float>(
            result_buf.data(), 
            result_buf.data() + result_buf.size()
        );
    }
    
    void trainModel(
        const std::string& training_audio_path,
        const std::string& output_model_path,
        int epochs = 100
    ) {
        rvc_module.attr("train_model")(
            training_audio_path,
            output_model_path,
            epochs
        );
    }
};
```

**RVC Training Pipeline:**

```
User selects training audio (5-30 min)
    ↓
Preprocess: Normalize, segment to 10-second chunks
    ↓
Feature extraction: HuBERT embeddings for each chunk
    ↓
Model training: ~20-30 min on GPU (RTX 3060+)
    ↓
Export: Save .pth checkpoint (~100-200MB per model)
    ↓
Inference: Load model, process real-time chunks (20-50ms)
```

**Model Storage:**
- Location: `~/.synthetic_obsidian/presets/` (Windows: `%APPDATA%\SyntheticObsidian\presets\`)
- Format: PyTorch `.pth` checkpoint (TorchScript for ONNX export planned)
- Max size: ~200MB per model
- Caching: Load on first use, keep in memory if VRAM permits

**GPU/CPU Management:**

```cpp
void VoicePresetModule::initializeRVCEngine() {
    std::string device = "cuda";  // Try CUDA first
    
    try {
        // Test GPU capability
        if (!isGPUAvailable()) {
            device = "cpu";
            logWarning("GPU not available, using CPU (slower)");
        }
    } catch (const std::exception& e) {
        device = "cpu";
        logError("GPU initialization failed: {}", e.what());
    }
    
    rvc_bridge = std::make_unique<RVCPythonBridge>(device);
}

bool isGPUAvailable() {
    // Check CUDA drivers on Windows/macOS
    // Check Metal Performance Shaders on macOS
    // Fallback to CPU
}
```

### 3.5 HarmonyGenerationModule

**Responsibility:** Generate harmony voices (drone, 3rd, 5th, octave, double).

**Algorithm:**

```cpp
// synth_obsidian/harmony/HarmonyGenerationModule.h
class HarmonyGenerationModule {
public:
    enum class HarmonyType {
        Drone,       // Sustained single note
        Third,       // Major/minor 3rd above/below
        Fifth,       // Perfect 5th above/below
        Octave,      // One octave up/down
        Double,      // Exact copy with slight variation
        Seventh,     // 7th chord tone
        Unison       // Same pitch, different timbre
    };
    
    struct HarmonyVoice {
        HarmonyType type;
        int interval_semitones;      // +3, -3, +5, +12, etc.
        float blend_amount;           // 0.0-1.0 mix into output
        bool use_target_voice_preset; // Apply voice conversion?
        std::string voice_preset;     // Which preset for timbre
    };
    
private:
    std::vector<PitchShiftingDSP> pitch_shifters;  // One per harmony
    VoicePresetModule& voice_preset_module;
    
public:
    AudioBuffer<float> generateHarmonies(
        const AudioBuffer<float>& vocal_in,
        const std::vector<HarmonyVoice>& harmony_config,
        const std::string& key_context
    );
};

// Implementation
AudioBuffer<float> HarmonyGenerationModule::generateHarmonies(
    const AudioBuffer<float>& vocal_in,
    const std::vector<HarmonyVoice>& harmony_config,
    const std::string& key_context
) {
    AudioBuffer<float> harmony_output(vocal_in.getNumChannels(), 
                                      vocal_in.getNumSamples());
    harmony_output.clear();
    
    // Process each harmony voice
    for (size_t i = 0; i < harmony_config.size(); ++i) {
        const auto& harmony = harmony_config[i];
        
        AudioBuffer<float> shifted_voice = vocal_in;
        
        // 1. Pitch shift to interval
        pitch_shifters[i].shiftPitch(shifted_voice, harmony.interval_semitones);
        
        // 2. Optional: Apply voice conversion for realism
        if (harmony.use_target_voice_preset) {
            shifted_voice = voice_preset_module.applyVoicePreset(
                shifted_voice,
                harmony.voice_preset
            );
        }
        
        // 3. Add to output with blend
        for (int ch = 0; ch < harmony_output.getNumChannels(); ++ch) {
            harmony_output.addFrom(
                ch, 0, 
                shifted_voice.getReadPointer(ch), 
                shifted_voice.getNumSamples(),
                harmony.blend_amount
            );
        }
    }
    
    return harmony_output;
}
```

**Harmony Config Example (from UI/preset):**

```json
{
  "harmonies": [
    {
      "type": "Drone",
      "interval_semitones": 0,
      "blend_amount": 0.3,
      "use_target_voice_preset": false
    },
    {
      "type": "Third",
      "interval_semitones": 3,
      "blend_amount": 0.4,
      "use_target_voice_preset": true,
      "voice_preset": "Breathy Harmonies"
    },
    {
      "type": "Fifth",
      "interval_semitones": 7,
      "blend_amount": 0.35,
      "use_target_voice_preset": true,
      "voice_preset": "Pad Voice"
    }
  ]
}
```

**Real-time Constraints:**
- Process each harmony in parallel (threading via JUCE ThreadPool)
- Cache pitch curves to avoid re-detecting
- Render harmony stems to separate tracks (optional export)

### 3.6 ARA2Module

**Responsibility:** Bridge plugin to DAW via ARA2 protocol, handle document model.

**Implementation:**

```cpp
// synth_obsidian/ara2/ARA2Module.h
class ARA2Module : public juce::ARA::ARAHostModel {
private:
    juce::ARA::DocumentPtr document;
    juce::ARA::MusicalContextPtr musical_context;
    juce::ARA::AudioSourcePtr guide_track_source;
    juce::ARA::PlaybackRegionPtr vocal_playback_region;
    juce::ARA::AudioModificationPtr pitch_modification;
    
    AnalysisModule& analysis_module;
    
public:
    // Host Model Protocol (called by DAW)
    void willCreateDocument(juce::ARA::Document& document) override {
        this->document = &document;
    }
    
    void didUpdateMusicalContext(
        juce::ARA::MusicalContext* context) override {
        this->musical_context = context;
        
        if (context) {
            // Extract tempo, key, time signature
            float tempo = context->getTempo();
            // Update UI visualizations
        }
    }
    
    void didUpdateAudioSource(
        juce::ARA::AudioSource* source) override {
        if (source->isProvidingAudio()) {
            guide_track_source = source;
            
            // Trigger analysis on guide track
            analyzeGuideTrackViaARA();
        }
    }
    
    void didCreatePlaybackRegion(
        juce::ARA::PlaybackRegion& region) override {
        vocal_playback_region = &region;
    }
    
    void didCreateAudioModification(
        juce::ARA::AudioModification& modification) override {
        pitch_modification = &modification;
    }
    
    // Plugin action: Read audio from ARA source
    void analyzeGuideTrackViaARA() {
        if (!guide_track_source) return;
        
        // Request audio from DAW via ARA reader interface
        juce::ARA::AudioReader::Request request;
        request.audioSource = guide_track_source;
        request.range = { 0, guide_track_source->getAudioDuration() };
        
        AudioBuffer<float> guide_buffer(
            1,  // Mono
            static_cast<int>(guide_track_source->getAudioDuration() * sample_rate)
        );
        
        auto reader = guide_track_source->createAudioReader();
        reader->performRead(request);
        
        // Analyze
        auto analysis_result = analysis_module.analyzeGuideTrack(
            guide_buffer, 
            sample_rate
        );
    }
    
    // Plugin action: Commit modifications back to DAW
    void commitPitchModifications(
        const std::vector<float>& pitch_targets_hz) {
        
        if (!pitch_modification) return;
        
        // Create AudioModification content
        // This tells DAW about pitch changes for visual feedback
        juce::ARA::AudioModificationContent content;
        content.pitchModified = true;
        content.formantPitchModified = true;
        
        pitch_modification->updateContent(content, true);
    }
};
```

**ARA2 Data Flow:**

```
1. User creates ARA document
2. DAW provides reference audio (guide track) via AudioSource
3. Plugin reads via ARA AudioReader, analyzes
4. Plugin creates PlaybackRegion for vocal track
5. Plugin processes vocal → creates AudioModifications
6. DAW applies modifications in real-time, shows visual feedback
7. On export: DAW merges modifications with original audio
```

**Key ARA2 Advantages:**
- **Non-destructive editing:** Original audio preserved
- **Bidirectional communication:** DAW ↔ Plugin data exchange
- **Visual feedback:** Piano roll synced with DAW timeline
- **Render-in-place:** Easy bouncing to audio

### 3.7 PianoRollEditor

**Responsibility:** UI component for manual note editing, visualization of detected pitch.

**Architecture:**

```cpp
// synth_obsidian/ui/PianoRollEditor.h
class PianoRollEditor : public juce::Component {
private:
    // Data model
    struct NoteEvent {
        int midi_note;
        int64_t start_sample;
        int64_t duration_samples;
        float velocity;
    };
    
    std::vector<NoteEvent> note_events;
    std::vector<PitchFrame> detected_pitch_curve;  // Background visualization
    
    // UI state
    juce::Rectangle<int> piano_roll_bounds;
    double zoom_x = 1.0;  // Horizontal (time)
    double zoom_y = 1.0;  // Vertical (pitch)
    int selected_note_index = -1;
    
    // Callbacks
    std::function<void(const std::vector<NoteEvent>&)> on_notes_changed;
    
public:
    void paint(juce::Graphics& g) override {
        paintBackground(g);
        paintPitchCurve(g);        // Light gray guide
        paintNoteEvents(g);        // Blue boxes
        paintGrid(g);
        paintLabels(g);            // Note names (C4, D4, etc.)
    }
    
    void mouseDown(const juce::MouseEvent& event) override {
        int clicked_note = hitTestNote(event.position);
        if (clicked_note >= 0) {
            selected_note_index = clicked_note;
        } else {
            // Create new note on click
            createNoteAtPosition(event.position);
        }
    }
    
    void mouseDrag(const juce::MouseEvent& event) override {
        if (selected_note_index >= 0) {
            // Drag to adjust pitch or duration
            updateSelectedNote(event.position);
        }
    }
    
private:
    void paintPitchCurve(juce::Graphics& g) {
        g.setColour(juce::Colours::lightgrey.withAlpha(0.5f));
        
        juce::Path pitch_path;
        bool first = true;
        
        for (size_t i = 0; i < detected_pitch_curve.size(); ++i) {
            float x = (i / (float)detected_pitch_curve.size()) * piano_roll_bounds.getWidth();
            float midi_pitch = hertzToMidi(detected_pitch_curve[i].frequency_hz);
            float y = midiToPixelY(midi_pitch);
            
            if (first) {
                pitch_path.startNewSubPath(x, y);
                first = false;
            } else {
                pitch_path.lineTo(x, y);
            }
        }
        
        g.strokePath(pitch_path, juce::PathStrokeType(1.0f));
    }
    
    void paintNoteEvents(juce::Graphics& g) {
        g.setColour(juce::Colours::cornflowerblue.withAlpha(0.7f));
        
        for (size_t i = 0; i < note_events.size(); ++i) {
            const auto& note = note_events[i];
            
            auto note_rect = getNoteRect(note);
            g.fillRect(note_rect);
            
            // Note label
            g.setColour(juce::Colours::white);
            g.drawText(midiToNoteName(note.midi_note),
                      note_rect.reduced(2),
                      juce::Justification::centred);
        }
    }
    
    juce::Rectangle<float> getNoteRect(const NoteEvent& note) {
        float start_x = (note.start_sample / (float)total_samples) * 
                       piano_roll_bounds.getWidth();
        float duration_x = (note.duration_samples / (float)total_samples) * 
                          piano_roll_bounds.getWidth();
        float y = midiToPixelY(note.midi_note);
        
        return juce::Rectangle<float>(
            piano_roll_bounds.getX() + start_x,
            y - 10,
            duration_x,
            20
        );
    }
};
```

**Integration with Core Audio:**

```cpp
// When user clicks "Commit Piano Roll"
class SyntheticObsidianProcessor : public juce::AudioProcessor {
    void commitPianoRollEdits() {
        auto notes = piano_roll_editor->getNoteEvents();
        
        // Convert to pitch targets for PitchCorrectionModule
        std::vector<float> pitch_targets_hz = 
            convertNotesToPitchCurve(notes);
        
        pitch_correction_module.setPitchTargets(pitch_targets_hz);
        pitch_correction_module.setMode(PitchCorrectionModule::CorrectionMode::ManualCorrection);
    }
};
```

**Keyboard Shortcuts:**
- `Delete`: Remove selected note
- `Ctrl+A`: Select all notes
- `V`: Snap to grid
- `Z`: Undo
- `Y`: Redo
- Arrow keys: Nudge pitch/timing

### 3.8 ExportModule

**Responsibility:** Export corrected vocal, harmonies, and MIDI data.

**Formats:**

```cpp
// synth_obsidian/export/ExportModule.h
class ExportModule {
public:
    enum class ExportFormat {
        WAV,      // Uncompressed, multiple stems
        MP3,      // Lossy (FFmpeg integration)
        FLAC,     // Lossless (libFLAC)
        MIDI,     // Note events from piano roll
        ARA,      // ARA2 project (if ARA2 enabled)
        STEMPACK  // All voices in one folder (Vocal + Harmonies + Backing)
    };
    
    struct ExportSettings {
        ExportFormat format;
        int sample_rate;                    // 44.1k, 48k, 96k, 192k
        int bit_depth;                      // 16, 24, 32 bits
        bool export_corrected_vocal;
        bool export_harmony_stems;
        bool export_backing_track;
        bool apply_voice_preset_in_export;
        std::string output_directory;
        std::string filename_template;      // "{song}_{voice}_corrected.wav"
    };
    
private:
    AudioBuffer<float> corrected_vocal;
    std::vector<AudioBuffer<float>> harmony_stems;
    AudioBuffer<float> backing_track;
    
public:
    void exportProject(const ExportSettings& settings);
    void exportVocal(const std::string& path);
    void exportHarmonies(const std::string& directory);
    void exportMIDI(const std::string& path);
    
private:
    void writeWAV(const AudioBuffer<float>& buffer,
                 const std::string& path,
                 const ExportSettings& settings);
    
    void writeMP3(const AudioBuffer<float>& buffer,
                 const std::string& path);
};

// Implementation
void ExportModule::exportProject(const ExportSettings& settings) {
    switch (settings.format) {
    case ExportFormat::WAV:
        writeWAV(corrected_vocal, 
                settings.output_directory + "/vocal_corrected.wav", 
                settings);
        for (size_t i = 0; i < harmony_stems.size(); ++i) {
            writeWAV(harmony_stems[i],
                    settings.output_directory + 
                    "/harmony_" + std::to_string(i) + ".wav",
                    settings);
        }
        break;
        
    case ExportFormat::MIDI:
        exportMIDI(settings.output_directory + "/notes.mid");
        break;
        
    case ExportFormat::STEMPACK:
        // Create folder structure
        // Track 1: Vocal
        // Track 2: Harmony 1
        // Track 3: Harmony 2
        // etc.
        break;
    }
}
```

**MIDI Export Format:**

```cpp
void ExportModule::exportMIDI(const std::string& path) {
    juce::MidiFile midi_file;
    
    auto notes = piano_roll_editor->getNoteEvents();
    
    // Track 1: Corrected vocal
    juce::MidiMessageSequence track1;
    for (const auto& note : notes) {
        int midi_note = note.midi_note;
        int start_beat = sampleToQuarter(note.start_sample, tempo_bpm);
        int duration_beat = sampleToQuarter(note.duration_samples, tempo_bpm);
        
        track1.addEvent(
            juce::MidiMessage::noteOn(1, midi_note, (uint8_t)127),
            start_beat
        );
        track1.addEvent(
            juce::MidiMessage::noteOff(1, midi_note),
            start_beat + duration_beat
        );
    }
    
    midi_file.addTrack(track1);
    midi_file.writeTo(path, 2);  // 2 for quarter note resolution
}
```

---

## 4. RVC Integration Strategy

### 4.1 RVC Architecture Selection

**Selected Approach:** Python subprocess + Pybind11 bindings

**Rationale:**

| Approach | Pros | Cons | Verdict |
|----------|------|------|--------|
| **1. Subprocess (Current)** | Easy to update RVC models, Python standard library control | Latency (IPC overhead ~20ms) | ✅ Chosen for flexibility |
| **2. Embedded Python (Pybind11)** | Low latency, same process | Complex debugging, GIL contention | Hybrid: Use with subprocess for training |
| **3. ONNX Runtime (C++ only)** | Maximum performance, no Python | Model export fragile, RVC changes break compatibility | Fallback option |
| **4. REST API (Local server)** | Decoupled, easy scaling | Network latency, additional process | Not for real-time |

**Implementation Plan:**

```
┌──────────────────────────────────────┐
│   Synthetic Obsidian (C++ Process)   │
│                                      │
│  ┌──────────────────────────────────┐│
│  │ Pybind11 Python Bindings         ││
│  │ - Fast IPC for inference         ││
│  │ - Synchronous calls (< 50ms)     ││
│  └──────────────────────────────────┘│
└──────────────────────────────────────┘
         ↓ (FFI calls)
┌──────────────────────────────────────┐
│   Python RVC Engine (Sub-process)    │
│   - PyTorch inference thread         │
│   - Model management                 │
│   - Feature extraction (HuBERT)      │
└──────────────────────────────────────┘
         ↓ (GPU/CPU)
┌──────────────────────────────────────┐
│   GPU (NVIDIA/AMD/Metal) OR CPU      │
│   - CUDA/cuDNN (NVIDIA)              │
│   - Metal Performance Shaders (Mac)  │
│   - MKL (Intel CPU)                  │
└──────────────────────────────────────┘
```

### 4.2 RVC Model Management

**Model Storage & Directory Structure:**

```
~/.synthetic_obsidian/
├── config.json                          # Global settings
├── models/
│   └── hubert_base.pt                  # Feature extractor (cached once)
├── presets/
│   ├── pop_singer_1.pth                # Trained RVC model (~150MB)
│   ├── pop_singer_1.json               # Metadata (name, date, stats)
│   ├── breathy_harmonies.pth
│   └── pad_voice.pth
└── cache/
    ├── last_training_stats.json
    └── inference_cache/                # Temp buffers
```

**HuBERT Feature Extractor (download on first use):**

```cpp
// synth_obsidian/ai/ModelCache.h
class ModelCache {
private:
    std::string models_directory;
    std::string hubert_path;
    
public:
    void ensureHuBERTDownloaded() {
        if (fs::exists(hubert_path)) {
            return;  // Already cached
        }
        
        logInfo("Downloading HuBERT model (350MB)...");
        
        // Download from Hugging Face
        std::string url = "https://huggingface.co/facebook/hubert-base/resolve/main/pytorch_model.bin";
        
        // Use libcurl or std::filesystem
        downloadFile(url, hubert_path);
        
        logInfo("HuBERT cached at {}", hubert_path);
    }
};
```

**Model Registration:**

```json
{
  "presets": [
    {
      "id": "pop_singer_1",
      "name": "Pop Singer #1",
      "model_path": "~/.synthetic_obsidian/presets/pop_singer_1.pth",
      "intensity_default": 0.8,
      "f0_shift_default": 0.0,
      "created_date": "2026-04-01T10:30:00Z",
      "training_duration_seconds": 1800,
      "training_sample_count": 45,
      "description": "Bright, pop vocal preset",
      "tags": ["pop", "female", "breathy"]
    }
  ]
}
```

### 4.3 Training Pipeline (RVC Model Creation)

**User Workflow:**

```
1. User selects "Create Voice Preset" from UI
2. File dialog opens → user selects 5-30 minute WAV file
3. Plugin validates:
   - Format: 44.1kHz, 16-bit mono minimum
   - Duration: >= 5 min (warn if < 10 min)
   - Level: Check for clipping, suggest normalization
4. Preprocessing:
   - Resample to 44.1kHz (if needed)
   - Normalize to -3dB
   - Segment to 10-second chunks (non-overlapping)
   - Detect silence, skip chunks below -60dB
5. Training initiation (non-blocking):
   - Launch RVC training in subprocess
   - Show progress bar (epoch 0-100)
   - Est. 20-30 min on RTX 3060+ GPU
   - ~2 min on CPU (not recommended)
6. Save model checkpoint:
   - Model: model.pth (~150MB)
   - Metadata: config.json (name, date, stats)
7. UI confirmation: "Preset 'My Voice' created successfully"
8. User can now use in real-time inference
```

**Concrete Training Implementation:**

```python
# rvc_engine/train.py
import torch
from rvc.models import RVCModel, get_fbank
from rvc.preprocess import preprocess_audio

class TrainingPipeline:
    def __init__(self, config_path: str):
        self.config = load_config(config_path)
        self.device = "cuda" if torch.cuda.is_available() else "cpu"
    
    def train(
        self,
        training_audio_path: str,
        output_model_path: str,
        epochs: int = 100,
        progress_callback: callable = None
    ) -> dict:
        """
        Train RVC model from audio file.
        
        Args:
            training_audio_path: Path to mono WAV file (5-30 min)
            output_model_path: Where to save .pth checkpoint
            epochs: Training iterations (default 100)
            progress_callback: fn(epoch, loss) for UI updates
        
        Returns:
            stats: Training statistics (total_loss, avg_loss, etc.)
        """
        
        # Step 1: Preprocess audio
        print(f"Preprocessing {training_audio_path}...")
        audio_chunks = preprocess_audio(training_audio_path, chunk_size=10)
        
        # Step 2: Extract features (HuBERT embeddings)
        print("Extracting acoustic features...")
        features = []
        for chunk in audio_chunks:
            fbank = get_fbank(chunk, sr=44100)
            features.append(fbank)
        
        # Step 3: Initialize model
        model = RVCModel(self.config).to(self.device)
        optimizer = torch.optim.Adam(model.parameters(), lr=1e-4)
        
        # Step 4: Training loop
        total_loss = 0.0
        for epoch in range(epochs):
            for feature_batch in self._batch_features(features, batch_size=32):
                optimizer.zero_grad()
                
                # Forward pass
                output = model(feature_batch)
                loss = model.compute_loss(output, feature_batch)
                
                # Backward pass
                loss.backward()
                optimizer.step()
                
                total_loss += loss.item()
            
            avg_loss = total_loss / (epoch + 1)
            
            # Callback for UI progress
            if progress_callback:
                progress_callback(epoch=epoch, loss=avg_loss)
            
            if (epoch + 1) % 10 == 0:
                print(f"Epoch {epoch+1}/{epochs}, Loss: {avg_loss:.4f}")
        
        # Step 5: Save checkpoint
        torch.save(model.state_dict(), output_model_path)
        print(f"Model saved to {output_model_path}")
        
        return {
            "total_loss": total_loss,
            "avg_loss": total_loss / epochs,
            "device": self.device,
            "epochs": epochs,
        }

# Called from C++ via Pybind11
def train_model_async(
    training_audio_path: str,
    output_model_path: str,
    epochs: int = 100
):
    pipeline = TrainingPipeline("/etc/rvc/config.yaml")
    
    def progress_callback(epoch: int, loss: float):
        # Send back to C++ via queue
        pass
    
    result = pipeline.train(
        training_audio_path,
        output_model_path,
        epochs,
        progress_callback
    )
    
    return result
```

**C++ Training Wrapper:**

```cpp
// synth_obsidian/ai/TrainingSession.h
class TrainingSession {
private:
    std::string training_audio_path;
    std::string output_model_path;
    std::thread training_thread;
    bool is_training = false;
    
    std::function<void(int, float)> progress_callback;  // epoch, loss
    
public:
    void startTraining(
        const std::string& audio_path,
        const std::string& output_path,
        std::function<void(int, float)> on_progress = nullptr
    ) {
        training_audio_path = audio_path;
        output_model_path = output_path;
        progress_callback = on_progress;
        
        is_training = true;
        
        training_thread = std::thread([this]() {
            try {
                // Call Python RVC engine
                py::module_ rvc_train = 
                    py::module_::import("rvc_engine.train");
                
                auto result = rvc_train.attr("train_model_async")(
                    training_audio_path,
                    output_model_path,
                    100  // epochs
                );
                
                // Parse result
                float final_loss = result["avg_loss"].cast<float>();
                
                logInfo("Training complete, final loss: {:.4f}", final_loss);
                is_training = false;
                
            } catch (const std::exception& e) {
                logError("Training failed: {}", e.what());
                is_training = false;
            }
        });
    }
    
    bool isTraining() const { return is_training; }
    
    void cancel() {
        // TODO: Implement thread safe cancellation
        is_training = false;
    }
    
    ~TrainingSession() {
        if (training_thread.joinable()) {
            training_thread.join();
        }
    }
};
```

### 4.4 Real-time Inference

**Inference Constraints:**

```
Input: 512 samples @ 44.1kHz = 11.6ms of audio
RVC inference: 20-50ms (depends on model size, GPU)
Pitch modification: ~100ms (separate pitch shifting)
Total latency: ~150ms (acceptable for correction plugin)

Target: < 5% of buffer size overhead
```

**Inference Loop:**

```cpp
// In AudioEngine::processBlock()
void AudioEngine::processRVCInference(
    juce::AudioBuffer<float>& vocal_buffer,
    const VoicePreset& preset,
    float intensity
) {
    if (!rvc_engine || !preset.is_available) {
        return;  // Fallback: no conversion
    }
    
    // 1. Ring buffer accumulation (RVC needs ~500-1000 samples)
    rvc_input_ring_buffer.push(vocal_buffer);
    
    if (rvc_input_ring_buffer.size() < RVC_CHUNK_SIZE) {
        return;  // Not enough samples yet
    }
    
    // 2. Extract chunk
    std::vector<float> audio_chunk = rvc_input_ring_buffer.pop(RVC_CHUNK_SIZE);
    
    // 3. Extract pitch
    PitchFrame pitch = pitch_detector.detectFrame(audio_chunk);
    
    // 4. Async RVC inference (fire-and-forget, result consumed next buffer)
    std::async(std::launch::async, [this, audio_chunk, pitch]() {
        try {
            auto converted = rvc_engine->inferenceChunk(
                audio_chunk,
                preset.model_path,
                pitch.frequency_hz,
                intensity
            );
            
            rvc_output_ring_buffer.push(converted);
            
        } catch (const std::exception& e) {
            logError("RVC inference failed: {}", e.what());
        }
    });
    
    // 5. Consume previous RVC output
    if (rvc_output_ring_buffer.size() >= output_position) {
        std::vector<float> output = rvc_output_ring_buffer.pop(vocal_buffer.getNumSamples());
        
        // Mix converted audio back (with intensity factor)
        vocal_buffer.clear();
        for (int s = 0; s < vocal_buffer.getNumSamples(); ++s) {
            vocal_buffer.addSample(
                0, s,
                audio_chunk[s] * (1.0f - intensity) +
                output[s] * intensity
            );
        }
    }
}
```

**Intensity Factor (Dry/Wet):**

- 0.0 = Original vocal (no conversion)
- 0.5 = 50% converted, 50% original
- 1.0 = Full conversion (most realistic)

### 4.5 RVC Licensing & Model Constraints

**Legal Considerations:**

| Issue | Status | Mitigation |
|-------|--------|-----------|
| **RVC License** | GNU Affero GPL v3 | ⚠️ **Critical:** Entire plugin must be AGPL-3.0 licensed OR use AGPL exemption |
| **Training Data** | User-provided | ✅ No copyright issues (user owns their voice) |
| **Pre-trained HuBERT** | CC BY-NC 4.0 | ✅ Non-commercial use (may need license negotiation for commercial) |
| **Model Size** | 150-200MB per preset | ⚠️ Disk storage; recommend cloud sync for sharing |

**Recommended License Path:**

```
Option A (Chosen):
- Plugin: GNU Affero GPL v3.0
- Benefits: Full RVC compatibility, open-source alignment
- Trade-off: Source code must be publicly available
- For: Educational, open-source projects

Option B (Commercial):
- Negotiate AGPL exemption with RVC author (King Arvid)
- Cost: ~$5,000-$10,000 (estimated)
- Allows proprietary license

Option C (ONNX Export):
- Export RVC model to ONNX format (GPL-free)
- Inference via ONNX Runtime (MIT license)
- Risk: Model export compatibility with RVC updates
```

**For this architecture, we recommend Option A (AGPL-3.0)** unless commercial licensing required.

---

## 5. ARA2 Implementation

### 5.1 ARA2 Concepts & JUCE Integration

**ARA2 Document Model (Simplified):**

```cpp
// From Celemony ARA2 spec, mapped to JUCE
juce::ARA::Document           // Top-level container
├── MusicalContext            // Key, tempo, time signature
├── AudioSource               // Reference/guide audio
│   ├── AudioSourceProperties (duration, sample rate)
│   └── AudioReader           // Read interface to audio
├── PlaybackRegion            // Editable vocal segment
│   ├── StartInMusicalContext (time)
│   ├── Duration
│   └── Color (UI visualization)
├── AudioModification         // Pitch/timing changes
│   ├── ModificationTime (reference time)
│   ├── ExpectedContentTypes (pitch, formant, etc.)
│   └── AudioSource (linked)
└── AnalysisResult            // Metadata from plugin
    ├── DetectedKey
    ├── DetectedTempo
    └── ChordProgression
```

### 5.2 JUCE ARA2 Adapter

**Plugin Inherits ARA Host Model:**

```cpp
// synth_obsidian/ara2/ARA2Adapter.h
class SyntheticObsidianProcessor : 
    public juce::AudioProcessor,
    public juce::ARA::ARAHostModel {
    
    // ===== ARA2 Host Model Callbacks =====
    
    void willCreateDocument(juce::ARA::Document& document) override {
        ara_document = &document;
        logInfo("ARA Document created");
    }
    
    void didUpdateMusicalContext(
        juce::ARA::MusicalContext* context) override {
        
        if (context == nullptr) return;
        
        double tempo = context->getTempo();
        auto time_sig = context->getTimeSignature();
        
        logInfo("Musical context: {} BPM, {}/{} time",
               tempo, time_sig.numerator, time_sig.denominator);
        
        // Update UI tempo/key display
        if (ui_handler) {
            ui_handler->updateTempoDisplay(tempo);
        }
    }
    
    void willNotifyAudioSourcePropertiesChanged(
        juce::ARA::AudioSource& audioSource,
        juce::ARA::ChangeFlags flags) override {
        
        // Audio source added or modified
        if (flags & juce::ARA::ChangeFlags::audioSourceAudioDuration) {
            double duration = audioSource.getDuration();
            logInfo("Audio source duration: {} samples", 
                   (int64_t)(duration * getSampleRate()));
        }
    }
    
    void willCreatePlaybackRegion(
        juce::ARA::PlaybackRegion& playbackRegion) override {
        
        // User created new editable region (vocal track)
        logInfo("Playback region created at {} for {} samples",
               playbackRegion.getStartInMusicalContext().getMusicalPosition(),
               playbackRegion.getDuration());
    }
    
    void willCreateAudioModification(
        juce::ARA::AudioModification& audioModification) override {
        
        // Plugin will apply modifications to this region
        logInfo("Audio modification created");
    }
};
```

### 5.3 Reading Guide Track Audio via ARA

**Implementation:**

```cpp
void SyntheticObsidianProcessor::readGuideTrackViaARA() {
    if (!ara_document) return;
    
    // 1. Find audio source (usually the first one)
    auto audio_sources = ara_document->getAudioSources();
    if (audio_sources.empty()) {
        logWarn("No audio sources in ARA document");
        return;
    }
    
    auto guide_source = audio_sources[0];
    
    // 2. Create audio reader
    auto reader = guide_source->createAudioReader();
    if (!reader) {
        logError("Cannot create audio reader");
        return;
    }
    
    // 3. Prepare buffer
    int num_samples = (int)(guide_source->getDuration() * getSampleRate());
    juce::AudioBuffer<float> guide_buffer(1, num_samples);
    
    // 4. Request read from DAW
    std::vector<float> audio_data(num_samples);
    
    const float** read_pointers = 
        (const float**)guide_buffer.getArrayOfReadPointers();
    
    juce::ARA::AudioReader::Request request;
    request.audioSource = guide_source;
    request.range = { 0.0, guide_source->getDuration() };
    request.samplesPerSecond = getSampleRate();
    request.readPointer = (float**)guide_buffer.getArrayOfWritePointers();
    request.readSize = num_samples;
    
    reader->performRead(request);
    
    // 5. Analyze
    auto analysis = analysis_module.analyzeGuideTrack(
        guide_buffer,
        getSampleRate()
    );
    
    logInfo("Guide track analyzed: {} BPM, key: {}",
           analysis.tempo_bpm, analysis.musical_key);
}
```

### 5.4 Writing Pitch Modifications Back to DAW

**ARA Pitch Modification Protocol:**

```cpp
void SyntheticObsidianProcessor::writePitchModifications(
    const std::vector<float>& pitch_targets_hz
) {
    if (!ara_document) return;
    
    // Get or create audio modification
    auto modifications = ara_document->getAudioModifications();
    juce::ARA::AudioModification* pitch_mod = nullptr;
    
    if (!modifications.empty()) {
        pitch_mod = modifications[0];
    } else {
        // Create new modification
        pitch_mod = ara_document->createAudioModification();
    }
    
    if (!pitch_mod) return;
    
    // Prepare modification content descriptor
    juce::ARA::AudioModificationContent content_descriptor;
    content_descriptor.pitchModified = true;
    content_descriptor.formantPitchModified = true;
    
    // Optional: Set expected content types
    pitch_mod->setExpectedContentTypes(content_descriptor);
    
    // Update modification
    pitch_mod->updateContent(content_descriptor, true);
    
    logInfo("Pitch modifications committed, {} values", 
           pitch_targets_hz.size());
}
```

### 5.5 Real-time Audio Processing with ARA

**Hybrid Processing Model:**

```cpp
void SyntheticObsidianProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midiMessages
) override {
    // Check if ARA2 is active
    if (isARA2Enabled()) {
        // Use ARA document data
        processBlockWithARA(buffer);
    } else {
        // Standalone/VST3 mode
        processBlockStandalone(buffer);
    }
}

void processBlockWithARA(juce::AudioBuffer<float>& buffer) {
    // 1. Get vocal region timing from ARA
    auto playback_region = ara_document->getPlaybackRegion();
    double region_start = playback_region->getStartInMusicalContext().getMusicalPosition();
    double region_duration = playback_region->getDuration();
    double current_time = getCurrentPlayPosition();
    
    if (current_time < region_start || 
        current_time > region_start + region_duration) {
        buffer.clear();
        return;  // Outside edit region
    }
    
    // 2. Read guide track context from ARA
    auto tempo_context = ara_document->getMusicalContext();
    if (tempo_context) {
        double tempo_bpm = tempo_context->getTempo();
        pitch_correction_module.setTempoContext(tempo_bpm);
    }
    
    // 3. Process vocal (normal processing chain)
    audio_engine.processBlock(buffer);
    
    // 4. Commit modifications back
    writePitchModifications(audio_engine.getPitchContour());
}
```

### 5.6 ARA2 VST3 Bridging

**JUCE/VST3 Handler (Automatic in JUCE 8+):**

```cpp
// JUCE automatically handles ARA2 bridging when:
// 1. Plugin is compiled with JUCE_ARA=1
// 2. Inherits from juce::ARA::ARAHostModel
// 3. VST3 host implements ARA2 extension

// In juce_appconfig.h
#define JUCE_ARA 1
```

**Host Requirements:**

| DAW | ARA2 Support | Version |
|-----|--------------|---------|
| **Cubase** | ✅ Yes | 12.0+ |
| **Logic Pro** | ❌ No | Not planned |
| **Studio One** | ✅ Yes | 6.0+ |
| **Reaper** | ✅ Yes (partial) | 6.82+ |
| **Cakewalk** | ❌ No | Deprecated |

---

## 6. Pitch Detection & Correction

### 6.1 Algorithm Selection

**Comparative Analysis:**

| Algorithm | Accuracy | Latency | Computational Cost | Robustness | Use Case |
|-----------|----------|---------|-------------------|------------|----------|
| **CREPE** | 95-98% | 10-15ms | High (GPU: low) | Excellent | Primary detector |
| **pYIN** | 90-95% | 20-30ms | Medium | Good | CPU fallback |
| **WORLD** | 92-96% | 5-10ms | Very High | Excellent | Reference only |
| **Straight** | 90-93% | 30-50ms | High | Good | Legacy |

**Chosen: CREPE + pYIN (Hybrid)**

```cpp
class PitchDetectionEngine {
private:
    enum class Algorithm { CREPE, pYIN };
    
    CREPEModel crepe_model;      // Pre-trained (pre-downloaded)
    pYINDetector pyin_detector;
    
public:
    PitchFrame detect(const AudioBuffer<float>& frame) {
        if (gpu_available) {
            return detectCREPE(frame);
        } else {
            return detectpYIN(frame);
        }
    }
    
private:
    PitchFrame detectCREPE(const AudioBuffer<float>& frame) {
        // Input: 512-1024 samples
        // Output: Pitch + confidence
        
        auto result = crepe_model.predict(
            frame.getReadPointer(0),
            frame.getNumSamples()
        );
        
        return PitchFrame {
            .frequency_hz = result.frequency,
            .confidence = result.confidence,
            .midi_note = hertzToMidi(result.frequency)
        };
    }
    
    PitchFrame detectpYIN(const AudioBuffer<float>& frame) {
        // pYIN: Probabilistic YIN algorithm
        auto result = pyin_detector.process(
            frame.getReadPointer(0),
            frame.getNumSamples(),
            getSampleRate()
        );
        
        return PitchFrame {
            .frequency_hz = result.frequency,
            .confidence = result.aperiodicity,  // Inverse confidence
            .midi_note = hertzToMidi(result.frequency)
        };
    }
};
```

### 6.2 CREPE Model Details

**CREPE (Convolutional Representation for Pitch Estimation):**

- **Paper:** [CREPE: A Convolutional Representation for Pitch Estimation](https://arxiv.org/abs/1802.06955)
- **Model Size:** ~29MB (PyTorch .pth file)
- **Input:** 512 samples @ 16kHz (~32ms frame)
- **Output:** Pitch (Hz) + confidence (0-1)
- **Inference Time:** ~10ms on GPU, ~50-100ms on CPU
- **Accuracy:** 95-98% on clean vocals

**CREPE Integration:**

```python
# rvc_engine/crepe_wrapper.py
import torch
import numpy as np
from crepe import get_activation
from crepe.model import PitchActivationConfidence

class CREPEInference:
    def __init__(self, device: str = "cuda"):
        self.device = device
        self.model = self._load_pretrained()
    
    def _load_pretrained(self):
        """Load pre-trained CREPE model from Hugging Face"""
        # TODO: Download from https://github.com/marl/crepe
        model_path = "models/crepe_full.pth"
        model = torch.jit.load(model_path)
        model.to(self.device)
        return model
    
    def predict(self, audio: np.ndarray, sr: int = 16000) -> dict:
        """
        Predict pitch from audio.
        
        Args:
            audio: [sample_count] mono float32
            sr: Sample rate
        
        Returns:
            {
                'time': [frame_count],
                'frequency': [frame_count],
                'confidence': [frame_count],
                'voiced_flag': [frame_count]
            }
        """
        # Resample to 16kHz if needed
        if sr != 16000:
            audio = librosa.resample(audio, orig_sr=sr, target_sr=16000)
        
        # Normalize
        audio = (audio - np.mean(audio)) / (np.std(audio) + 1e-5)
        
        # Convert to tensor
        audio_tensor = torch.from_numpy(audio).float().unsqueeze(0).to(self.device)
        
        with torch.no_grad():
            # Model outputs logits for 360 pitch bins (0-120 Hz range)
            activation = self.model(audio_tensor).cpu().numpy()
        
        # Argmax to get pitch bin
        confidence, midi_note = get_activation(activation)
        
        # Convert MIDI to Hz
        frequency = 10 * 2 ** (midi_note / 12)  # Convert MIDI to Hz
        
        return {
            'frequency': frequency,
            'confidence': confidence,
            'voiced_flag': confidence > 0.5
        }
```

### 6.3 Pitch Shifting Without Artifacts

**Challenge:** Phase vocoder causes artifacts (roboticness, metallic tone).

**Solutions:**

1. **Minimal Shifting:** Only apply when out-of-tune by > 20 cents
2. **Smoothing:** Apply low-pass filter to pitch curve (cutoff 10Hz)
3. **Vibrato Preservation:** Extract and re-apply vibrato
4. **Quality Mode:** Use Rubberband's "Process (High Quality)" setting

**Implementation:**

```cpp
class PitchShiftingDSP {
private:
    RubberBandStretcher stretcher;
    int sample_rate;
    
public:
    void shiftPitch(
        AudioBuffer<float>& in_out,
        const std::vector<float>& pitch_targets_hz,
        float formant_preservation = 0.8f
    ) {
        // 1. Detect current pitch
        auto current_pitch = detector.detectFrame(in_out);
        
        // 2. Calculate pitch shift ratio
        float shift_ratio = pitch_targets_hz[0] / current_pitch.frequency_hz;
        
        // Threshold: only shift if > 20 cents (2%)
        if (std::abs(shift_ratio - 1.0f) < 0.02f) {
            return;  // Too small, skip
        }
        
        // 3. Configure stretcher for formant preservation
        stretcher.setFormantScale(formant_preservation);
        stretcher.setPitchScale(shift_ratio);
        stretcher.setDetectorOptions(
            RubberBandStretcher::OptionProcessRealTime,
            RubberBandStretcher::OptionWindowShape,
            RubberBandStretcher::OptionTransientsSmooth
        );
        
        // 4. Process in chunks
        const int chunk_size = 512;
        
        for (int offset = 0; offset < in_out.getNumSamples(); offset += chunk_size) {
            int remaining = in_out.getNumSamples() - offset;
            int process_size = std::min(remaining, chunk_size);
            
            stretcher.process(
                in_out.getArrayOfReadPointers()[0] + offset,
                process_size,
                false  // not final
            );
            
            int available = stretcher.available();
            if (available > 0) {
                std::vector<float> output(available);
                stretcher.retrieve(output.data(), available);
                
                // Write back
                std::copy(output.begin(), output.end(),
                         in_out.getWritePointer(0) + offset);
            }
        }
        
        // Flush
        stretcher.process(nullptr, 0, true);  // final=true
    }
};
```

**Vibrato Preservation Algorithm:**

```cpp
struct VibratoAnalysis {
    float frequency_hz;        // Typical: 5-8 Hz
    float depth_cents;         // Typical: 30-100 cents
    float phase_offset;
};

VibratoAnalysis analyzeVibrato(
    const std::vector<PitchFrame>& pitch_curve,
    int sample_rate
) {
    // FFT on pitch contour to find dominant frequency
    std::vector<float> pitch_values(pitch_curve.size());
    for (size_t i = 0; i < pitch_curve.size(); ++i) {
        pitch_values[i] = pitch_curve[i].frequency_hz;
    }
    
    // Apply FFT
    auto fft_magnitude = performFFT(pitch_values);
    
    // Find peak in 4-9 Hz range (vibrato band)
    int max_bin = 0;
    float max_magnitude = 0.0f;
    
    for (int i = 4; i <= 9; ++i) {  // Frequency in Hz
        if (fft_magnitude[i] > max_magnitude) {
            max_magnitude = fft_magnitude[i];
            max_bin = i;
        }
    }
    
    return VibratoAnalysis {
        .frequency_hz = (float)max_bin,
        .depth_cents = max_magnitude * 100.0f,  // Convert to cents
        .phase_offset = 0.0f
    };
}

void reapplyVibrato(
    std::vector<float>& pitch_contour,
    const VibratoAnalysis& vibrato
) {
    for (size_t i = 0; i < pitch_contour.size(); ++i) {
        float t = (float)i / sample_rate;
        
        // Generate vibrato LFO
        float vibrato_lfo = vibrato.depth_cents * 
                           std::sin(2.0f * M_PI * vibrato.frequency_hz * t);
        
        // Add to pitch (in cents domain)
        float pitch_hz = pitch_contour[i];
        float pitch_cents_deviation = vibrato_lfo;
        
        pitch_contour[i] = pitch_hz * std::pow(2.0f, pitch_cents_deviation / 1200.0f);
    }
}
```

### 6.4 Latency Budget Analysis

| Stage | Latency (ms) | Notes |
|-------|--------------|-------|
| Audio input buffering | 11.6 | 512 samples @ 44.1kHz |
| Pitch detection (CREPE) | 10-15 | GPU accelerated |
| Pitch decision + snapping | 1 | Lookup table |
| Pitch shifting (Rubberband) | 100-150 | Phase vocoder inherent |
| Output accumulation | 11.6 | Ring buffer |
| **Total Latency** | **~180-240ms** | Reported to DAW |

**Mitigation:**
- DAW automatically compensates via `setLatencySamples()`
- User sees no latency in recording/playback
- Monitoring through zero-latency direct input possible

---

## 7. Cross-Platform Build Configuration

### 7.1 CMake Build System

**Project Structure:**

```
synthetic-obsidian/
├── CMakeLists.txt                      # Main build config
├── cmake/
│   ├── JUCEHelpers.cmake              # JUCE configuration
│   ├── RVCPythonIntegration.cmake     # Python bindings
│   ├── VersionInfo.cmake              # Version generation
│   └── CompilerWarnings.cmake         # Warning settings
├── source/
│   ├── CMakeLists.txt
│   ├── core/
│   │   ├── AudioEngine.h/cpp
│   │   ├── dsp/
│   │   ├── analysis/
│   │   └── ...
│   ├── ui/
│   │   └── ...
│   ├── ara2/
│   │   └── ...
│   └── ai/
│       └── ...
├── external/
│   ├── JUCE/                          # JUCE 8.0.2 submodule
│   ├── ARA-SDK/                       # ARA SDK 3.0.3 submodule
│   ├── rubberband/                    # Rubberband v3.3+ source
│   └── essentia/                      # Essentia library
├── tests/
│   └── CMakeLists.txt
├── python/
│   ├── rvc_engine/
│   ├── setup.py
│   └── requirements.txt
├── scripts/
│   ├── build.sh                       # Build wrapper
│   ├── sign_macos.sh                  # macOS notarization
│   └── windows_installer.ps1          # NSIS script
└── README.md
```

**Main CMakeLists.txt:**

```cmake
cmake_minimum_required(VERSION 3.21)
project(SyntheticObsidian VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Platform Detection
# ============================================================================
if (APPLE)
    set(TARGET_OS "macOS")
    set(OSX_DEPLOYMENT_TARGET 10.15)
    add_compile_definitions(MACOS=1)
elseif (WIN32)
    set(TARGET_OS "Windows")
    add_compile_definitions(WINDOWS=1)
elseif (UNIX)
    set(TARGET_OS "Linux")
    add_compile_definitions(LINUX=1)
endif()

message(STATUS "Building for: ${TARGET_OS}")

# ============================================================================
# JUCE Configuration
# ============================================================================
set(JUCE_ENABLE_MODULE_SOURCE_GROUPS ON CACHE BOOL "" FORCE)

add_subdirectory(external/JUCE)

# ============================================================================
# ARA2 SDK
# ============================================================================
include_directories(external/ARA-SDK/include)

# ============================================================================
# DSP Libraries
# ============================================================================

# Rubberband (Pitch Shifting)
find_package(RubberBand 3.3 REQUIRED)

# Essentia (Analysis)
find_package(Essentia 2.1 REQUIRED)

# ============================================================================
# Python & RVC Integration
# ============================================================================
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development)

include_directories(${Python3_INCLUDE_DIRS})
link_directories(${Python3_LIBRARY_DIRS})

# Pybind11 for C++/Python bindings
add_subdirectory(external/pybind11)

# ============================================================================
# Plugin Target
# ============================================================================
juce_add_plugin(SyntheticObsidian
    PRODUCT_NAME "Synthetic Obsidian"
    COMPANY_NAME "Your Company"
    PLUGIN_MANUFACTURER_CODE "YMPR"
    PLUGIN_CODE "OBSN"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    FORMATS VST3 AU
    VST3_CATEGORIES
        "Audio"
        "Distortion"
        "Pitch"
    AU_MAIN_TYPE "aumu"  # AU Music Effect
)

# Plugin source files
target_sources(SyntheticObsidian PRIVATE
    # Core
    source/core/AudioEngine.h
    source/core/AudioEngine.cpp
    source/core/PluginProcessor.h
    source/core/PluginProcessor.cpp
    
    # DSP Modules
    source/core/dsp/PitchDetectionEngine.h
    source/core/dsp/PitchDetectionEngine.cpp
    source/core/dsp/PitchShiftingDSP.h
    source/core/dsp/PitchShiftingDSP.cpp
    
    # Analysis
    source/analysis/AnalysisModule.h
    source/analysis/AnalysisModule.cpp
    
    # Pitch Correction
    source/correction/PitchCorrectionModule.h
    source/correction/PitchCorrectionModule.cpp
    
    # Voice Presets / RVC
    source/voice/VoicePresetModule.h
    source/voice/VoicePresetModule.cpp
    source/ai/RVCPythonBridge.h
    source/ai/RVCPythonBridge.cpp
    
    # Harmony
    source/harmony/HarmonyGenerationModule.h
    source/harmony/HarmonyGenerationModule.cpp
    
    # ARA2
    source/ara2/ARA2Module.h
    source/ara2/ARA2Module.cpp
    
    # UI
    source/ui/PluginEditor.h
    source/ui/PluginEditor.cpp
    source/ui/PianoRollEditor.h
    source/ui/PianoRollEditor.cpp
    
    # Export
    source/export/ExportModule.h
    source/export/ExportModule.cpp
)

# Link libraries
target_link_libraries(SyntheticObsidian PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_gui_extra
    
    RubberBand::RubberBand
    Essentia::Essentia
    
    ${Python3_LIBRARIES}
)

# Enable ARA2
target_compile_definitions(SyntheticObsidian PRIVATE
    JUCE_ARA=1
    JUCE_PLUGIN_VST3_CATEGORY="Pitch"
)

# ============================================================================
# Python RVC Module (Pybind11)
# ============================================================================
pybind11_add_module(rvc_engine
    source/ai/rvc_python_bindings.cpp
)

target_link_libraries(rvc_engine PRIVATE
    ${Python3_LIBRARIES}
)

# ============================================================================
# Compiler Flags
# ============================================================================
if (MSVC)
    target_compile_options(SyntheticObsidian PRIVATE /W4 /permissive-)
else()
    target_compile_options(SyntheticObsidian PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# Installation
# ============================================================================
if (APPLE)
    install(TARGETS SyntheticObsidian
        BUNDLE DESTINATION "/Library/Audio/Plug-Ins/VST3"
        COMPONENT plugin)
elseif (WIN32)
    install(TARGETS SyntheticObsidian
        LIBRARY DESTINATION "C:/Program Files/Common Files/VST3"
        COMPONENT plugin)
endif()
```

### 7.2 macOS Build (Xcode)

**Build Command:**

```bash
mkdir build && cd build

cmake .. \
  -G "Xcode" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15

cmake --build . --config Release --target SyntheticObsidian_VST3
```

**Code Signing & Notarization:**

```bash
# Sign plugin
codesign --deep --force --verify --verbose \
  --sign "Developer ID Application: Your Company" \
  "SyntheticObsidian.vst3"

# Submit for notarization
xcrun notarytool submit \
  "SyntheticObsidian.vst3" \
  --apple-id your-apple-id@company.com \
  --password "app-specific-password" \
  --team-id "YOUR_TEAM_ID" \
  --wait

# Staple notarization ticket
xcrun stapler staple "SyntheticObsidian.vst3"
```

**Script: `scripts/sign_macos.sh`**

```bash
#!/bin/bash
set -e

PLUGIN_PATH="$1"
SIGNING_IDENTITY="Developer ID Application: Your Company"
APPLE_ID="your-apple-id@company.com"
TEAM_ID="YOUR_TEAM_ID"

echo "Signing ${PLUGIN_PATH}..."
codesign --deep --force --verify --verbose \
  --sign "${SIGNING_IDENTITY}" \
  "${PLUGIN_PATH}"

echo "Submitting for notarization..."
REQUEST_UUID=$(xcrun notarytool submit \
  "${PLUGIN_PATH}" \
  --apple-id "${APPLE_ID}" \
  --password "@env:APPLE_PASSWORD" \
  --team-id "${TEAM_ID}" \
  --wait \
  | grep "id:" | awk '{print $2}')

echo "Notarization ID: ${REQUEST_UUID}"

echo "Stapling notarization ticket..."
xcrun stapler staple "${PLUGIN_PATH}"

echo "✅ Plugin signed and notarized successfully"
```

### 7.3 Windows Build (Visual Studio)

**Build Command:**

```bash
mkdir build && cd build

cmake .. `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DCMAKE_INSTALL_PREFIX="C:\Program Files\SyntheticObsidian"

cmake --build . --config Release --target SyntheticObsidian_VST3
```

**NSIS Installer:**

```nsis
; scripts/windows_installer.nsi
!include "MUI2.nsh"

Name "Synthetic Obsidian VST3"
OutFile "SyntheticObsidian-1.0.0-Installer.exe"
InstallDir "$PROGRAMFILES\Common Files\VST3"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  File "SyntheticObsidian.vst3\*.*"
  
  ; Register in registry
  WriteRegStr HKCR "CLSID\{YOUR-UUID}" "" "Synthetic Obsidian"
  
SectionEnd

Section "Uninstall"
  RMDir /r "$INSTDIR\SyntheticObsidian.vst3"
SectionEnd
```

### 7.4 Linux Build (Optional)

```bash
mkdir build && cd build

cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/usr/lib/x86_64-linux-gnu/cmake"

cmake --build . --config Release
```

**Dependencies:**

```bash
# Ubuntu/Debian
sudo apt-get install \
  build-essential cmake \
  libjack-jackd2-dev libssl-dev \
  python3-dev python3-pip \
  librubberband-dev \
  libessentia-dev
```

### 7.5 Build Script Wrapper

**`scripts/build.sh` - Universal entry point:**

```bash
#!/bin/bash
set -e

BUILD_TYPE="${1:-Release}"
PLATFORM="${2:-auto}"

# Auto-detect platform
if [ "$PLATFORM" = "auto" ]; then
    if [[ "$OSTYPE" == "darwin"* ]]; then
        PLATFORM="macos"
    elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]]; then
        PLATFORM="windows"
    else
        PLATFORM="linux"
    fi
fi

echo "Building for: $PLATFORM ($BUILD_TYPE)"

# Create build directory
mkdir -p build
cd build

case $PLATFORM in
    macos)
        cmake .. -G "Xcode" \
          -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15
        cmake --build . --config $BUILD_TYPE
        
        # Sign and notarize
        if [ "$BUILD_TYPE" = "Release" ]; then
            ../scripts/sign_macos.sh \
              "./SyntheticObsidian_artefacts/$BUILD_TYPE/VST3/SyntheticObsidian.vst3"
        fi
        ;;
        
    windows)
        cmake .. -G "Visual Studio 17 2022" -A x64
        cmake --build . --config $BUILD_TYPE
        ;;
        
    linux)
        cmake .. -DCMAKE_BUILD_TYPE=$BUILD_TYPE
        cmake --build .
        ;;
esac

echo "✅ Build complete"
```

---

## 8. Risks & Mitigation

### 8.1 Technical Risks

| Risk | Severity | Probability | Mitigation |
|------|----------|-------------|-----------|
| **RVC GPU/CPU latency too high** | HIGH | MEDIUM | Use CPU fallback, profile early, optimize inference batch size |
| **Rubberband pitch artifacts (metallic tone)** | MEDIUM | MEDIUM | Implement vibrato preservation, formant compensation, quality settings |
| **CREPE model download (350MB) slow** | MEDIUM | LOW | Cache after first download, show progress bar, allow offline use |
| **ARA2 document sync issues with DAW** | HIGH | MEDIUM | Extensive testing with Logic/Cubase/Studio One, follow ARA2 spec exactly |
| **Thread safety in real-time DSP** | CRITICAL | MEDIUM | Use lock-free data structures, extensive testing, avoid malloc in audio thread |
| **Out-of-memory for large batch processing** | MEDIUM | LOW | Stream audio in chunks, implement progressive loading, monitor memory usage |
| **Python GIL contention (blocking audio thread)** | HIGH | HIGH | Use async Python calls, thread pool for training, Pybind11 release GIL |

### 8.2 Licensing Risks

| Risk | Mitigation |
|------|-----------|
| **RVC GPL v3 → Commercial use blocked** | Choose AGPL-3.0 license OR negotiate exemption ($5-10k) |
| **HuBERT CC BY-NC restrictions** | Check commercial use rights, consider training own feature extractor |
| **Rubberband licensing** | Use open-source version (LGPL) OR commercial license ($500-1000) |
| **Essentia GPL** | Replace with lighter analysis (librosa Python call) or fork/modify |

**Recommended License Stack:**

```
Synthetic Obsidian: GNU Affero GPL v3.0
├── JUCE: GPL v3 (with commercial license option)
├── RVC: AGPL v3.0 (AGPL exemption negotiated)
├── ARA2 SDK: Celemony proprietary (free for plugin developers)
├── Rubberband: LGPL v2.1
└── Essentia: AGPL v3.0 (or replace with librosa)
```

### 8.3 Performance Risks

**CPU Usage Targets:**

| Operation | Target | Real CPU | Mitigation |
|-----------|--------|----------|-----------|
| Pitch detection (CREPE on GPU) | < 5% | N/A | Async GPU processing |
| Pitch shifting | 10-15% | 5% @ 512 samples | Phase vocoder tuning |
| RVC inference (GPU) | < 10% | 20-50% if CPU-only | Warn user, provide CPU fallback |
| UI rendering | < 2% | N/A | Limit refresh to 60fps |
| **Total @ 48kHz, 512 buffer** | **~25%** | 20-30% | Optimize DSP loops, profile |

**GPU Requirements:**

```
Recommended: NVIDIA RTX 3060+ (6GB VRAM) for real-time RVC
Minimum: NVIDIA GTX 1060 (3GB VRAM)
Fallback: CPU (30x slower, not recommended for real-time)

macOS: Metal Performance Shaders (M1/M2/M3 optimized)
Windows: CUDA 12.1+, cuDNN 8.6+
```

### 8.4 Model Size & Storage Risks

| Model | Size | Total | Mitigation |
|-------|------|-------|-----------|
| HuBERT base | 350MB | 350MB | Download once, cache indefinitely |
| RVC preset (average) | 180MB | 180MB × N | Store locally, implement cleanup, cloud sync option |
| Rubberband cache | Negligible | < 10MB | Clean up after each session |
| CREPE weights (TorchScript) | 29MB | 29MB | Bundle with plugin |
| **Total (1 preset)** | **~560MB** | **560MB** | Inform user, implement selective loading |

**Storage Management:**

```cpp
class StorageManager {
public:
    void cleanupOldPresets(int keep_count = 10) {
        // Keep N most recent presets
        // Delete others to save space
    }
    
    void calculateStorageUsage() -> StorageStats {
        // Report total usage to UI
    }
};
```

### 8.5 User Experience Risks

| Risk | Impact | Mitigation |
|------|--------|-----------|
| **First launch slow (HuBERT download)** | HIGH | Show download progress, cache, lazy-load |
| **Training takes 30 min** | MEDIUM | Show real-time progress, allow background processing, queue management |
| **Latency ~200ms confuses users** | MEDIUM | Educate in manual, DAW auto-compensates, show latency in UI |
| **RVC artifacts on speech** | HIGH | Provide voice preset templates, intensity knob, quality settings |
| **Piano roll UI complexity** | MEDIUM | Simplify default view, provide tutorials, drag-and-drop editing |

---

## 9. Development Phases & Timeline

### Phase 1: Foundation (Weeks 1-4, ~160 hours)

**Goals:**
- JUCE plugin scaffold
- AudioEngine → basic pitch detection
- VST3 compilation & testing in DAW

**Deliverables:**
1. JUCE 8 project template with CMake
2. PluginProcessor skeleton (processBlock, prepareToPlay)
3. PitchDetectionEngine (CREPE model loading)
4. Basic UI (volume fader, tempo display)
5. Windows/macOS binary (both architectures)

**Technical Tasks:**
- [ ] Set up CMake build system
- [ ] Integrate JUCE 8.0.2
- [ ] Download & cache CREPE model (PyTorch)
- [ ] Implement Python embedding (Pybind11)
- [ ] Test pitch detection on sample vocals
- [ ] Verify VST3 plugin loads in DAW
- [ ] Document build process

**Estimated Hours:** 160

**Testing:**
- Unit tests: Pitch detection accuracy (95%+ on sine wave)
- Integration test: Plugin loads in Cubase/Logic/Studio One
- Manual test: Real vocal recording through plugin

**Risks:**
- Python/JUCE integration complexity
- GPU driver issues (NVIDIA/Metal)

---

### Phase 2: Pitch Correction & Audio Processing (Weeks 5-8, ~180 hours)

**Goals:**
- Working auto-tune with timbre preservation
- Manual pitch editing (piano roll)
- Rubberband pitch shifting integration

**Deliverables:**
1. PitchCorrectionModule (chromatic/scale snap modes)
2. PitchShiftingDSP (Rubberband v3.3)
3. PianoRollEditor (basic UI, note creation/deletion)
4. Pitch curve visualization
5. Real-time processing demo

**Technical Tasks:**
- [ ] Implement pitch snapping algorithms
- [ ] Integrate Rubberband library
- [ ] Implement formant preservation (frequency scaling)
- [ ] Build piano roll editor component (JUCE Graphics)
- [ ] Handle GUI → audio thread communication
- [ ] Test with various vocal samples

**Estimated Hours:** 180

**Testing:**
- A/B listening tests: Original vs. auto-tuned
- Grid checking: Notes snap correctly to scale
- UI responsiveness: Real-time dragging in piano roll
- Artifact detection: Listen for metallic/robotic tone

**Risks:**
- Pitch shifting quality (artifacts)
- GUI responsiveness at 60fps with waveform rendering

---

### Phase 3: Analysis & Guide Track Processing (Weeks 9-12, ~140 hours)

**Goals:**
- AnalysisModule (key, BPM, time signature detection)
- ARA2 integration (document model, host callbacks)
- Guide track audio reading

**Deliverables:**
1. AnalysisModule (Essentia backend)
2. ARA2Module (host model implementation)
3. Guide track auto-analysis UI
4. Analysis visualization (chroma vector plot)
5. ARA2 document persistence

**Technical Tasks:**
- [ ] Integrate Essentia library (key/tempo detection)
- [ ] Implement ARA2 host model callbacks
- [ ] Create AudioReader for guide track access
- [ ] Build ARA document model
- [ ] Store analysis results (JSON)
- [ ] Test ARA2 with multiple DAWs

**Estimated Hours:** 140

**Testing:**
- Accuracy: Key detection (test on MIDI → guide track)
- Accuracy: BPM detection (compare against manual)
- ARA2 host communication: Verify callbacks in Cubase/Studio One
- Persistence: Analysis survives project save/load

**Risks:**
- ARA2 documentation gaps
- DAW-specific ARA2 quirks (Logic has no ARA2)

---

### Phase 4: Voice Presets & RVC Integration (Weeks 13-18, ~220 hours)

**Goals:**
- RVC model training pipeline
- Real-time RVC inference
- Voice preset management UI

**Deliverables:**
1. VoicePresetModule (management, training, inference)
2. RVC Python wrapper (training script)
3. Pybind11 C++ ↔ Python bindings
4. Preset UI (file browser, delete, rename)
5. Training progress dialog
6. Model caching & storage

**Technical Tasks:**
- [ ] Implement RVC training script (Python)
- [ ] Create Pybind11 bindings for RVC inference
- [ ] Build thread-safe training session handler
- [ ] Implement preset file I/O (JSON metadata)
- [ ] Create training progress UI dialog
- [ ] Profile RVC inference latency
- [ ] GPU/CPU detection & fallback
- [ ] Test with various vocal styles

**Estimated Hours:** 220

**Testing:**
- Training: Create preset from 10min sample, verify quality
- Inference: Real-time processing, measure latency
- Edge cases: GPU memory exhaustion, model loading failures
- Compatibility: Different voice types (male/female, languages)

**Risks:**
- **CRITICAL:** RVC inference latency on CPU (may be unusable)
- **HIGH:** GPU memory fragmentation during training
- Python subprocess communication reliability
- Model size (180MB per preset) storage concerns

---

### Phase 5: Harmony Generation (Weeks 19-21, ~100 hours)

**Goals:**
- Multi-voice harmony synthesis
- Voice conversion for harmonies
- Harmony configuration UI

**Deliverables:**
1. HarmonyGenerationModule (interval-based generation)
2. Harmony UI controls (type, blend amount, voice preset)
3. Separate harmony stems export
4. Real-time harmony processing

**Technical Tasks:**
- [ ] Implement harmony interval calculation
- [ ] Apply pitch shifting to generate intervals
- [ ] Integrate RVC for harmony voice conversion
- [ ] Build harmony control UI
- [ ] Thread harmony processing
- [ ] Export stems separately
- [ ] Listen tests for harmony quality

**Estimated Hours:** 100

**Testing:**
- Quality: Listen for natural-sounding harmonies
- Interval accuracy: 3rd/5th/octave correctness
- Voice conversion: Harmony timbre distinct from lead
- Export: Stems mix correctly

**Risks:**
- Phasing issues between harmony voices
- Voice conversion latency × N voices compounding

---

### Phase 6: UI Polish & Export (Weeks 22-24, ~120 hours)

**Goals:**
- Professional UI/UX
- Export functionality (WAV, MIDI, stems)
- Settings/preferences panel
- Documentation & tutorials

**Deliverables:**
1. Refined plugin UI (visuals, layout, responsiveness)
2. ExportModule (WAV, MIDI, stem pack)
3. Settings dialog (sample rate, buffer size, GPU/CPU preference)
4. Help documentation (Quick Start, troubleshooting)
5. Video tutorials (5-10 min each)

**Technical Tasks:**
- [ ] Design UI mockups (Figma)
- [ ] Implement WAV export (libsndfile or JUCE AudioFormatWriter)
- [ ] Implement MIDI export
- [ ] Build stem pack export (multi-file)
- [ ] Settings persistence (JSON config)
- [ ] Write documentation
- [ ] Create video tutorials

**Estimated Hours:** 120

**Testing:**
- UI responsiveness at various screen sizes
- Export file integrity (WAV, MIDI, stems)
- Settings apply correctly
- Documentation clarity

**Risks:**
- UI design complexity (many features)
- Documentation completeness

---

### Phase 7: Compilation, Signing & Release (Weeks 25-26, ~80 hours)

**Goals:**
- Cross-platform binaries
- Code signing (macOS notarization, Windows cert)
- Installer creation
- Beta testing

**Deliverables:**
1. macOS VST3 (Universal: ARM64 + x86_64), signed & notarized
2. Windows VST3 (x86_64), signed with Microsoft Authenticode
3. NSIS installer
4. Beta distribution (beta testers, press)
5. Release notes & CHANGELOG

**Technical Tasks:**
- [ ] macOS code signing setup (Developer ID)
- [ ] macOS notarization (Apple Developer account)
- [ ] Windows code signing (Authenticode certificate)
- [ ] Build automation (CI/CD, GitHub Actions)
- [ ] Create NSIS installer
- [ ] Test on clean systems (no dev tools)
- [ ] Beta tester feedback & fixes
- [ ] Final QA round

**Estimated Hours:** 80

**Testing:**
- Clean install on macOS (Monterey, Ventura, Sonoma)
- Clean install on Windows (10, 11)
- Plugin functionality in Cubase, Studio One, Reaper
- Installer uninstall behavior

**Risks:**
- macOS notarization delays
- Driver/OS compatibility issues (last-minute)
- Installer quirks on edge-case Windows versions

---

### Phase 8: Post-Launch (Ongoing, ~40 hours/month)

**Goals:**
- Bug fixes
- User feedback implementation
- Performance optimization
- New features (v1.1+)

**Deliverables:**
1. Patch releases (bug fixes)
2. Performance updates
3. Feature roadmap (v1.1: VST2, more voice presets, GPU batch inference)

**Estimated Hours (ongoing):** 40-80/month

---

### Total Project Timeline

| Phase | Duration | Hours | Cumulative |
|-------|----------|-------|-----------|
| 1. Foundation | 4 weeks | 160 | 160 |
| 2. Pitch Correction | 4 weeks | 180 | 340 |
| 3. Analysis & ARA2 | 4 weeks | 140 | 480 |
| 4. RVC Integration | 6 weeks | 220 | 700 |
| 5. Harmony Generation | 3 weeks | 100 | 800 |
| 6. UI & Export | 3 weeks | 120 | 920 |
| 7. Build & Release | 2 weeks | 80 | 1000 |
| **Total** | **26 weeks** | **1000** | **1000** |

**Team Allocation (assuming 1 senior dev):**
- 6 months full-time = ~1040 hours (slightly over due to learning curve)
- Recommended: 1 dev @ 100%, with contractor support for:
  - macOS notarization (0.5 week)
  - UI design/polish (1 week contractor designer)

---

## 10. Data Flow Diagrams

### 10.1 Real-time Audio Processing Flow

```
┌─────────────────────┐
│   DAW Audio Input   │
│   (vocal, backing)  │
└──────────┬──────────┘
           │
           ▼
┌──────────────────────────────┐
│   AudioEngine::processBlock() │
│   512 samples @ 44.1kHz       │
└──────────┬───────────────────┘
           │
    ┌──────┴──────┐
    │             │
    ▼             ▼
┌──────────────┐  ┌──────────────┐
│ Guide Track  │  │ Vocal Input  │
│ (optional)   │  │ (from DAW)   │
└──────────────┘  └──────┬───────┘
                         │
                         ▼
              ┌──────────────────────┐
              │ Pitch Detection      │
              │ (CREPE/pYIN)         │
              │ → pitch_contour[t]   │
              └──────┬───────────────┘
                     │
                     ▼
         ┌───────────────────────┐
         │ Snap to Scale/Grid    │
         │ (PitchCorrection)     │
         │ → target_pitch[t]     │
         └───────┬───────────────┘
                 │
                 ▼
      ┌──────────────────────────┐
      │ Pitch Shift (Rubberband) │
      │ & Formant Preservation   │
      │ → corrected_vocal[]      │
      └──────┬───────────────────┘
             │
    ┌────────┴─────────┐
    │                  │
    ▼                  ▼
┌──────────────┐  ┌──────────────────────┐
│ Direct Out   │  │ RVC Inference (async)│
│ (monitoring) │  │ Voice Conversion     │
└──────────────┘  │ → rvc_output[]       │
                  └──────┬───────────────┘
                         │
                         ▼
                ┌──────────────────────┐
                │ Harmony Generation   │
                │ (offset intervals)   │
                │ + RVC for harmonies  │
                │ → harmony_stems[][]  │
                └──────┬───────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │ Mix (vocal + harmony)│
            │ with blend amounts   │
            │ → final_output[]     │
            └──────┬───────────────┘
                   │
                   ▼
         ┌──────────────────────┐
         │ ARA2 Modification    │
         │ (if ARA2 enabled)    │
         │ Report pitch change  │
         └──────┬───────────────┘
                │
                ▼
      ┌────────────────────────┐
      │  DAW Output Buffer     │
      │  (to speakers/render)  │
      └────────────────────────┘
```

### 10.2 User Training Workflow

```
User Action: "Create Voice Preset"
       │
       ▼
┌──────────────────────────┐
│ File Dialog              │
│ Select training audio    │
│ (5-30 min WAV)           │
└────────┬─────────────────┘
         │
         ▼
┌──────────────────────────────┐
│ Preprocess                   │
│ • Normalize to -3dB          │
│ • Segment to 10sec chunks    │
│ • Detect/skip silence        │
└────────┬─────────────────────┘
         │
         ▼
┌──────────────────────────────┐
│ Feature Extraction (HuBERT)  │
│ CPU/GPU parallel processing  │
└────────┬─────────────────────┘
         │
         ▼
┌──────────────────────────────┐
│ RVC Model Training           │
│ (Async background thread)    │
│                              │
│ ┌────────────────────────┐   │
│ │ Epoch 1: Loss 0.823    │   │
│ │ Epoch 2: Loss 0.671    │   │
│ │ ...                    │   │
│ │ Epoch 100: Loss 0.134  │   │
│ └────────────────────────┘   │
│                              │
│ Est. 20-30 min on RTX 3060+  │
└────────┬─────────────────────┘
         │
         ▼
┌──────────────────────────────┐
│ Save Checkpoint              │
│ • model.pth (~150MB)         │
│ • metadata.json              │
│   (name, date, loss, etc.)   │
└────────┬─────────────────────┘
         │
         ▼
┌──────────────────────────────┐
│ ✅ Success Dialog             │
│ "Preset 'My Voice' created"  │
│ Ready to use in real-time    │
└──────────────────────────────┘
```

### 10.3 ARA2 Document Synchronization

```
DAW Creates ARA Document
         │
         ▼
┌────────────────────────────────┐
│ Plugin receives ARA callbacks:  │
│                                │
│ willCreateDocument()            │
│   ↓ Store ara_document ptr     │
│ didUpdateMusicalContext()       │
│   ↓ Extract tempo, key         │
│ didUpdateAudioSource()          │
│   ↓ Read guide track audio     │
│ willCreatePlaybackRegion()      │
│   ↓ Register editable region   │
│ willCreateAudioModification()   │
│   ↓ Link to pitch modifications│
└────────┬───────────────────────┘
         │
         ▼
┌────────────────────────────────┐
│ Plugin analyzes guide track:    │
│ AnalysisModule.analyze()        │
│                                │
│ → musical_context.setKey()     │
│ → MusicalContextContent        │
└────────┬───────────────────────┘
         │
         ▼
┌────────────────────────────────┐
│ User edits in piano roll:      │
│ • Drag notes                   │
│ • Snap to scale               │
│ • Apply RVC conversion        │
└────────┬───────────────────────┘
         │
         ▼
┌────────────────────────────────┐
│ Plugin commits modifications:  │
│                                │
│ pitch_mod.updateContent()      │
│ → Pitch array [Hz or cents]    │
│ → Timing array [samples]       │
│ → Confidence [0.0-1.0]         │
└────────┬───────────────────────┘
         │
         ▼
┌────────────────────────────────┐
│ DAW visualizes changes:        │
│ • Piano roll shows edits       │
│ • Waveform display updates     │
│ • Real-time playback with mods │
└────────┬───────────────────────┘
         │
         ▼
┌────────────────────────────────┐
│ User exports:                  │
│ • Render to audio track        │
│ • Merge with original vocal    │
│ • WAV/STEM output              │
└────────────────────────────────┘
```

---

## Summary & Recommendations

### Key Architecture Decisions

1. **JUCE 8** → Only framework with mature ARA2/VST3 support
2. **Python subprocess + Pybind11** → Flexibility for RVC updates vs. performance trade-off
3. **CREPE + pYIN hybrid** → GPU when available, CPU fallback always works
4. **Rubberband pitch shifting** → Established, formant-preserving
5. **Essentia for analysis** → Professional MIR library, good key/tempo detection
6. **AGPL-3.0 license** → RVC compatibility, open-source alignment

### Critical Dependencies to Lock Down

```
JUCE 8.0.2 ← Must match VST3 ARA2 spec
RVC 2.0+ ← API changes could break plugin
HuBERT base ← Download once, cache forever
Rubberband 3.3+ ← Verify pitch quality before shipping
Essentia 2.1 ← Large library, consider librosa replacement
```

### Performance Bottlenecks to Profile Early

1. **RVC inference latency** (20-50ms GPU, 200-500ms CPU)
2. **Pitch shifting artifacts** (vibrato preservation critical)
3. **UI refresh rate** at high audio buffer counts (256, 128)
4. **Memory fragmentation** during long training sessions

### Delivery Risks (in order)

1. 🔴 **RVC real-time performance** on non-GPU systems
2. 🔴 **ARA2 cross-DAW compatibility** (Logic has no ARA2, Reaper partial)
3. 🟡 **Training data privacy** (model storage, user voice files)
4. 🟡 **macOS notarization delays** (first-time submission)
5. 🟢 **UI/UX polish** (soluble with extra contractor hours)

### Next Steps

1. **Week 1:** Set up JUCE project, verify CREPE model loading
2. **Week 2:** Get pitch detection working end-to-end (DAW → plugin → output)
3. **Week 4:** First alpha build, test in Cubase/Studio One
4. **Week 13:** RVC training working (critical milestone)
5. **Week 26:** Beta release to 20-30 early adopters

---

**Document Version:** 1.0  
**Last Updated:** 2026-04-11  
**Status:** Ready for Development Start

