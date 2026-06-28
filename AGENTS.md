# AGENTS.md — Synthetic Obsidian

## Purpose

This document defines the engineering rules, coding standards, and architectural constraints that any AI agent or contributor must follow when working on **Synthetic Obsidian** — a JUCE 8-based C++ VST3/ARA2 vocal processing plugin with AI/ML components (RVC, ONNX Runtime, pybind11).

The goal is to keep the codebase:
- safe for real-time audio
- maintainable across a mixed C++/Python stack
- idiomatic modern C++ and JUCE
- easy to review
- stable across macOS and Windows
- consistent with JUCE best practices

If a request conflicts with these rules, prefer these rules unless explicitly instructed otherwise.

---

## Project Context

- **Language:** C++20 (core), Python 3.10+ (AI/ML via pybind11 or subprocess)
- **Framework:** JUCE 8.x
- **Build system:** CMake
- **Plugin formats:** VST3, Standalone (ARA2 in Phase 2)
- **Platforms:** macOS (ARM64 + x86_64 Universal), Windows x86_64
- **License:** AGPL-3.0 (due to RVC dependency)
- **AI stack:** ONNX Runtime 1.15+, RVC v2/v3, pybind11, Essentia, librosa, WORLD vocoder
- **Key modules:** AudioEngine, AnalysisModule, PitchCorrectionModule, VoicePresetModule, HarmonyGenerationModule, PianoRollEditor, ExportModule

See `docs/03-technical-architecture.md` for the full architecture reference.

---

## Core Principles

1. Write code that is correct, simple, and easy to maintain.
2. Prefer clarity over cleverness.
3. Do not introduce hidden allocations, hidden locks, or hidden latency into real-time code.
4. Keep audio-thread code deterministic and bounded in time.
5. Use modern C++ features where they improve safety and readability, but avoid unnecessary abstraction.
6. Minimize global state.
7. Keep JUCE integration idiomatic and conventional.
8. Make small, reviewable changes rather than broad rewrites.
9. Preserve existing behavior unless the task explicitly requests behavior changes.
10. Always consider cross-platform behavior: macOS and Windows. Linux is optional/future.

---

## Project Assumptions

Unless explicitly stated otherwise, assume:

- C++20 is the standard
- JUCE 8 is the core framework
- Real-time audio safety is **mandatory**
- The project builds with CMake (do not regenerate or switch to Projucer)
- The code should compile warning-free on both Clang (macOS) and MSVC/Clang-cl (Windows)
- Plugin formats: VST3 and Standalone. ARA2 is Phase 2 — do not entangle it in MVP code paths unless clearly isolated
- AI inference runs off the audio thread, always. ONNX Runtime calls are never made from `processBlock()`
- Python (pybind11/subprocess) is used only from background threads for analysis and training

---

## Rules for the Agent

### The agent must

- read nearby code before making changes
- match existing naming and file organization
- keep changes minimal and scoped to the request
- explain risky tradeoffs when introducing them
- prefer incremental refactors over architecture rewrites
- preserve public APIs unless a change is explicitly requested
- update related code paths when modifying interfaces
- add comments only when they add real value
- write code that another C++ developer can quickly understand

### The agent must not

- perform speculative rewrites
- add dependencies without strong justification
- introduce macros when ordinary C++ solves the problem
- use exceptions in real-time critical paths
- allocate memory on the audio thread
- lock mutexes on the audio thread
- block the audio thread with file I/O, logging, waiting, networking, OS calls, or ONNX inference
- call Python (pybind11 or subprocess) from the audio thread
- introduce singletons unless already used and clearly justified
- silence warnings without addressing root causes
- over-template code when a concrete type is simpler
- replace working JUCE idioms with custom infrastructure unless clearly beneficial

---

## Modern C++ Guidelines

### General style

- Prefer modern C++ idioms over legacy C++
- Prefer stack allocation when ownership is simple
- Prefer RAII for all resource management
- Prefer `const` correctness everywhere appropriate
- Prefer `constexpr` and `noexcept` where meaningful
- Prefer `enum class` over unscoped enums
- Prefer `using` over `typedef`
- Prefer initialization at declaration
- Prefer range-based loops where clarity improves
- Prefer standard library facilities unless JUCE types are more appropriate

### Type usage

- Use `std::unique_ptr` for exclusive ownership
- Use `std::shared_ptr` only when shared ownership is genuinely required
- Use references for non-null required parameters
- Use pointers only when null is a valid state or ownership transfer semantics require it
- Use `std::optional` when the absence of a value is meaningful
- Use `std::array` for fixed-size collections
- Use `std::vector` for dynamic contiguous storage, but **never grow it on the audio thread**
- Prefer `std::span` or equivalent non-owning views where appropriate
- Use JUCE containers only when they meaningfully improve interoperability with existing JUCE APIs

### Functions

- Keep functions short and single-purpose
- Prefer pure helper functions where possible
- Pass large objects by `const&`
- Return values instead of output parameters unless performance or API constraints strongly justify otherwise
- Mark overrides explicitly with `override`
- Use `final` where it communicates intent and prevents misuse
- Avoid default arguments in virtual functions

### Classes

- Keep classes focused and cohesive
- Separate UI concerns from DSP concerns
- Separate DSP concerns from AI/ML concerns (ONNX, Python)
- Prefer composition over inheritance
- Use inheritance mainly where JUCE requires it (Component, AudioProcessor hierarchies)
- Make ownership relationships obvious

---

## JUCE-Specific Best Practices

### Architecture

Maintain the following module separation:

```
Source/
  PluginProcessor.*          # Host-facing, parameter management, state I/O
  PluginEditor.*             # UI root only
  dsp/
    AudioEngine.*            # Real-time audio routing and buffer management
    PitchCorrectionModule.*  # DSP pitch correction (pYIN, WORLD)
    HarmonyGenerationModule.* # Back vocal synthesis
  ai/
    VoicePresetModule.*      # RVC training + ONNX inference (background thread)
    AnalysisModule.*         # Guide track analysis (Python bridge, offline)
    OnnxInferenceRunner.*    # ONNX Runtime wrapper (never called from audio thread)
  ui/
    TrackManagerPanel.*
    PianoRollEditor.*
    VoicePresetCreatorWindow.*
    StylePickerDialog.*
  state/
    ProjectState.*           # ValueTree-based session state
    DecisionsLog.*           # ADR helpers if needed
  util/
    LockFreeQueue.*
    BackgroundTaskRunner.*
```

Avoid putting DSP or AI logic in the editor, and avoid UI logic in the processor.

### Parameters

- Prefer `juce::AudioProcessorValueTreeState` for all automatable parameters
- Use stable parameter IDs — never rename them in a released version
- Keep display text, ranges, skew, units, and defaults explicit
- Cache parameter pointers or atomics during setup — do not search by ID in `processBlock()`
- Do not read UI widget state from audio code

### ValueTree and state

- Use `ValueTree` for structured session state (tracks, presets, back vocal configs)
- Keep host-automatable parameters separate from non-automated session state
- Ensure serialization/deserialization is robust to missing or older fields
- Validate restored state — never assume it is valid

### Components and UI

- Keep painting lightweight — avoid unnecessary repaints
- Use `juce::AnimatedAppComponent` or `juce::Timer` for animated elements (waveform, playhead, AI meter)
- Avoid hardcoded magic numbers — centralize layout constants in a `UIConstants` namespace
- Prefer clear component ownership patterns
- Keep editor code readable and modular

### Threading in JUCE

- `processBlock()` and anything it calls = audio thread = real-time critical
- ONNX Runtime inference = background thread only
- pybind11 / subprocess Python calls = background thread only, with GIL management
- UI callbacks = message thread
- Use `juce::AsyncUpdater`, `juce::MessageManager::callAsync`, or lock-free FIFOs for cross-thread communication
- Never call code requiring the message thread from the audio thread

---

## Real-Time Audio Safety Rules

### Forbidden on the audio thread

- heap allocation or deallocation
- mutex locks of any kind
- condition variables
- file I/O
- console I/O or logging
- network access
- sleep or wait operations
- dynamic container growth
- ONNX Runtime inference calls
- Python calls (pybind11 or subprocess)
- calling code that may internally do any of the above

### Required practices

- Preallocate all buffers in `prepareToPlay()`
- Resize and reserve memory before playback begins
- Keep per-sample and per-block code bounded and simple
- Guard against NaNs and infinities in DSP code
- Handle sample-rate and block-size changes correctly
- Use bypass-safe and reset-safe state transitions

### Lock-free and shared state

- Prefer atomics for small shared values (AI Influence %, current pitch, latency ms)
- For larger data handoff between AI thread and audio thread, use double-buffering or `juce::AbstractFifo`-backed queues
- Document thread ownership of every shared object

---

## AI/ML Component Rules

These rules are specific to Synthetic Obsidian's AI stack.

### ONNX Runtime

- All ONNX inference runs in a dedicated background thread (`juce::Thread` subclass or thread pool)
- Never call `Ort::Session::Run()` from `processBlock()`
- Use `OnnxInferenceRunner` wrapper that owns the session and exposes a non-blocking async API
- Results are passed back to the audio or UI thread via lock-free queue or atomic flag
- GPU sessions (CUDA/CoreML/DirectML) must have a CPU fallback path that is always functional
- Models are loaded lazily on first use, never during plugin instantiation

### pybind11 / Python bridge

- Python is used for: guide track analysis (key, BPM, chords), RVC training pipeline
- Python calls happen only from background threads — never from audio or message thread
- Acquire/release the GIL correctly in pybind11 bindings
- The Python runtime is bundled — do not assume system Python
- Subprocess-based communication uses JSON protocol documented in `docs/04-ai-ml-implementation.md`
- Training progress is reported back via atomic counters or `juce::Value` on the message thread

### RVC and voice models

- Model files live in `~/.synthetic_obsidian/models/` — respect this path on both platforms
- Models are downloaded on first use (not bundled in the installer for size reasons)
- The training UI in `VoicePresetCreatorWindow` runs training in a background thread with a progress bar
- CPU training is supported but warned about (estimated 6-8 hours); GPU is recommended (30-45 min)
- Model format: ONNX exported from RVC v2/v3 — document the export script in `scripts/`

---

## DSP Code Guidelines

- Keep DSP classes independent from UI and AI classes
- Keep algorithms testable without requiring a plugin host
- Explicit lifecycle: `prepare(sampleRate, maxBlockSize)`, `reset()`, `process(buffer)`
- Make sample rate, block size, and channel assumptions explicit
- Support mono and stereo; do not silently assume stereo only
- Avoid hidden state changes

### Pitch correction specifics

- Real-time path: pYIN pitch detection + WORLD phase vocoder pitch shifting
- Offline path: CREPE (via ONNX) + WORLD vocoder — higher quality, no real-time constraint
- Formant preservation is mandatory — use LPC-based frequency warping
- Pitch contour smoothing: Kalman-like filter to avoid jumps
- Total real-time latency budget: ≤30ms (report to host via `getLatencySamples()`)

### Numerical stability

- Clamp output where required
- Initialize all DSP state explicitly in `reset()`
- Reset delay lines, filters, envelope followers, and smoothers on transport events

### Bypass and latency

- Report latency accurately with `getLatencySamples()` — DAW compensates automatically
- Support soft bypass (continue processing to avoid clicks)
- Smooth all audible parameter changes

---

## Memory Management

- Prefer deterministic ownership — avoid raw owning pointers
- Ensure background threads are stopped cleanly before plugin destruction
- Be explicit about object lifetime when JUCE callback registration is involved
- Be careful with lambdas capturing `this` — verify lifetime safety
- Use `juce::WeakReference` or explicit unregistering when needed

---

## Error Handling

- Fail early in non-real-time code (model loading, Python init, ONNX session creation)
- Validate inputs at boundaries — especially deserialized project state
- Use assertions for programmer errors
- In audio code, prefer graceful fallback (silence, bypass) over throwing
- Exceptions must not cross real-time or plugin-host-facing boundaries
- On model load failure: log the error and disable the AI feature gracefully — do not crash

---

## Logging and Diagnostics

- Logging must **never** occur from the audio thread
- Use `JUCE_LOG_CURRENT_EXCEPTION` and `DBG()` only from safe threads
- Remove temporary debug logging before merging
- Keep assertions meaningful and informative
- AI inference errors and Python exceptions must be caught and reported to the UI thread

---

## Performance Guidelines

- Measure before optimizing non-obvious bottlenecks
- The audio thread is the hottest path — optimize it first
- ONNX inference is heavy — always async, cache results aggressively
- Avoid repeated ONNX session creation — reuse sessions across inferences
- Python startup cost is high — initialize the interpreter once at plugin startup, not per-request
- Minimize copies in performance-sensitive paths
- Consider cache locality in DSP inner loops

---

## Code Organization

### Preferred structure

```
Source/
  PluginProcessor.h / .cpp
  PluginEditor.h / .cpp
  dsp/
  ai/
  ui/
  state/
  util/
CMakeLists.txt
AGENTS.md
docs/
  00-README.md
  01-product-requirements.md
  02-design-specification.md
  03-technical-architecture.md
  04-ai-ml-implementation.md
  05-agent-discussion.md
  06-open-questions.md
  07-decisions-log.md
scripts/
  export_rvc_to_onnx.py
  prepare_training_data.py
```

### Header rules

- Keep headers lightweight — forward declare where practical
- Include only what is needed
- Avoid putting implementation-heavy code in headers unless templates require it
- Maintain consistent include order: JUCE → stdlib → third-party → project headers

### CPP rules

- Put non-trivial implementations in `.cpp` files
- Keep translation units focused
- Avoid giant files — split by responsibility

---

## Naming Conventions

Follow any existing style in the file being edited. For new code:

- Types: `PascalCase` (e.g., `PitchCorrectionModule`, `VoicePresetModel`)
- Functions and variables: `camelCase` (e.g., `processBlock`, `currentPitchHz`)
- Constants: `kCamelCase` (e.g., `kMaxVoiceTracks`, `kDefaultAiInfluence`)
- Member variables: trailing underscore preferred (e.g., `sampleRate_`, `onnxSession_`)
- Parameter IDs: lowercase with underscores (e.g., `"pitch_drift"`, `"formant_shift"`, `"ai_influence"`)
- ONNX model node names: match the exported model's names exactly — document them

---

## Comments and Documentation

- Write self-explanatory code first
- Add comments when intent is not obvious
- Explain **why**, not what
- Good comment targets for this project:
  - Thread-safety assumptions (especially audio vs. AI thread boundary)
  - Ownership of model sessions and Python interpreter
  - Latency compensation decisions
  - pYIN vs. CREPE selection logic
  - WORLD vocoder parameter choices
  - Host compatibility workarounds
  - Numerical stability considerations in DSP paths

---

## Testing and Validation

Before finalizing any change, verify:

- Does it compile on both macOS (Clang) and Windows (MSVC/Clang-cl)?
- Does it preserve real-time safety?
- Does it affect state serialization or parameter IDs?
- Does it affect automation behavior?
- Does it affect sample-rate or block-size handling?
- Does it affect the boundary between audio thread and AI thread?

### Recommended testing areas

- Plugin instantiation (Standalone and VST3)
- Playback with silence, sine tones, real vocal audio
- Parameter automation in a DAW
- Rapid parameter changes (pitch drift, formant shift, AI influence)
- State save/load (project round-trip)
- Editor open/close repeatedly without memory leaks
- Sample rate changes (44.1k, 48k, 96k)
- Block size changes (64, 128, 256, 512, 1024)
- ONNX inference on GPU and CPU fallback
- RVC training start/cancel/complete flow
- Back vocal generation for all modes (drone, third, fifth, octave, double)
- Export (WAV stems)

---

## Build and Tooling

- **Build system:** CMake — do not switch to Projucer
- **macOS:** Universal Binary (ARM64 + x86_64), requires notarization for distribution
- **Windows:** x86_64, requires Authenticode signing for distribution
- Keep compiler warnings clean — treat warnings as important signals
- CI should run on both platforms — see `docs/03-technical-architecture.md` §7

---

## Dependencies

Current approved third-party dependencies:

| Library | Purpose | License |
|---------|---------|---------|
| JUCE 8 | Plugin framework, UI, DSP | GPLv3 / Commercial |
| ONNX Runtime 1.15+ | AI model inference | MIT |
| pybind11 | Python-C++ bridge | BSD-2 |
| Rubberband 3.3+ | Pitch shifting (alternative/complement to WORLD) | GPLv2 |
| Essentia 2.1 | Audio analysis (key, BPM) | AGPLv3 |
| librosa / madmom | BPM + chord analysis (Python) | ISC / BSD |
| RVC v2/v3 | Voice conversion | AGPL-3.0 |
| FAISS | Vector similarity for RVC | MIT |
| WORLD vocoder | Pitch shifting + formant | Modified BSD |

Do not add new dependencies without strong justification and team discussion. Any new dependency must have a compatible license (note: AGPL-3.0 project, so LGPL and MIT are acceptable; GPLv2-only is not without compatibility analysis).

---

## Backward Compatibility

For plugin projects, assume backward compatibility matters for state and parameters.

Be especially careful with:
- Parameter IDs (e.g., `"pitch_drift"`, `"ai_influence"`) — never rename casually
- Parameter ranges and defaults
- Serialized `ValueTree` structure (project file format)
- Latency reporting
- Preset file format
- Voice model file format (ONNX model schema)

---

## Refactoring Rules

Refactor only when it helps the requested task or clearly reduces risk.

**Good refactors:**
- extracting a focused DSP class from a large processor
- isolating AI thread handoff logic
- removing duplicated vocal processing code
- clarifying ownership of ONNX sessions

**Bad refactors:**
- renaming large parts of the codebase for style
- replacing JUCE idioms with custom frameworks
- introducing abstraction layers with no immediate value
- touching parameter IDs, serialization, or public APIs without necessity

---

## What the Agent Should Include in Change Summaries

When summarizing a change, mention:
- what changed
- why it changed
- whether real-time safety was affected
- whether the audio/AI thread boundary was affected
- whether serialization or parameter IDs changed
- whether ONNX model schema or Python API changed
- whether behavior changed
- any risks or follow-up work

---

## Preferred Implementation Patterns

### Good patterns

- Small DSP classes with explicit `prepare` / `reset` / `process` lifecycle
- Parameter caching during `prepareToPlay()`
- Atomics for lightweight cross-thread scalar state (pitch value, AI influence %)
- Lock-free FIFO for passing ONNX results from AI thread to audio/UI thread
- Background `juce::Thread` subclass for ONNX inference with a result queue
- pybind11 bridge initialized once at startup, called only from a dedicated analysis thread
- Clear separation: `PluginProcessor` ↔ `AudioEngine` ↔ `PitchCorrectionModule` ↔ `OnnxInferenceRunner`

### Discouraged patterns

- Running ONNX inference inside `processBlock()`
- Calling Python from the audio thread or message thread
- Giant processor classes mixing DSP, AI, and UI coordination
- Editor directly mutating DSP or AI module internals
- Uncontrolled shared mutable state between AI thread and audio thread
- Loading model files during playback

---

## When the Agent Is Unsure

If the task is ambiguous, prefer in this order:
1. Preserve current architecture
2. Choose the simpler implementation
3. Choose the safer real-time behavior
4. Choose the more maintainable design
5. Document assumptions clearly in the response

When unsure whether a call is safe on the audio thread: **assume it is not**, and move it off.

---

## Definition of Done

A change is considered complete only if:
- it solves the requested problem
- it follows the codebase style
- it respects real-time audio constraints
- the audio/AI thread boundary is preserved
- it does not introduce threading hazards
- it avoids unnecessary complexity
- it keeps public behavior and serialized state stable unless change was requested
- it is readable and reviewable

---

## Quick Checklist for Every Change

Before finalizing, verify:

- [ ] Is this safe on the audio thread?
- [ ] Did I introduce any allocations or locks in real-time code?
- [ ] Did I call ONNX Runtime or Python from the audio thread?
- [ ] Are ownership and lifetime clear?
- [ ] Did I preserve parameter and state compatibility?
- [ ] Is UI separated from DSP and AI?
- [ ] Is the code simpler rather than more clever?
- [ ] Did I change only what was necessary?
- [ ] Would a human maintainer understand this quickly?
- [ ] Does it compile on both macOS and Windows?

---

## Final Instruction

When working in this repository, behave like a senior C++ and JUCE engineer who also understands the AI/ML stack:

- conservative with risk
- strict about real-time safety — the audio thread is sacred
- strict about the audio/AI thread boundary — ONNX and Python never touch the audio thread
- clear in architecture
- minimal in changes
- modern in C++
- practical over clever
- aware that AGPL-3.0 governs all contributions
