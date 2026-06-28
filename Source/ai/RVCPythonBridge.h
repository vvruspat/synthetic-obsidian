#pragma once
#include <JuceHeader.h>
#include <vector>
#include <functional>

/** RVCPythonBridge
 *
 *  C++ interface to the embedded Python RVC inference engine.
 *
 *  ADR-006 — PRIMARY pitch correction path:
 *    RVC does not shift the waveform mathematically.  It re-synthesises the
 *    voice at the target pitch using a neural model trained on the user's voice,
 *    producing natural formants and natural transitions between notes.
 *
 *  Lifecycle
 *  ---------
 *    1. Construct once at plugin startup (lazy-initialises Python runtime).
 *    2. Call loadPreset() when the user selects a voice preset.
 *    3. Call resynthesizeAtPitch() from a background thread to correct a segment.
 *    4. Call unloadPreset() / destructor to free resources.
 *
 *  Thread safety
 *  -------------
 *    loadPreset / unloadPreset     — message thread
 *    resynthesizeAtPitch           — background thread only, NOT audio thread
 *    isAvailable / currentPreset   — any thread (atomic reads)
 *
 *  Phase 1 (current): stub implementation — isAvailable() returns false,
 *    resynthesizeAtPitch() is a no-op.  All callers fall back to phase vocoder.
 *  Phase 2: full Python-bridge implementation via pybind11.
 */
class RVCPythonBridge
{
public:
    //==========================================================================
    // Progress callback type:  (fractionDone 0..1, statusMessage)
    using ProgressFn = std::function<void (float, const juce::String&)>;

    //==========================================================================
    RVCPythonBridge();
    ~RVCPythonBridge();

    //==========================================================================
    /** Returns true when a preset is loaded and Python runtime is ready.
     *  All callers must check this before calling resynthesizeAtPitch(). */
    bool isAvailable() const noexcept;

    /** Path of the currently loaded preset model file, or empty string. */
    juce::String currentPresetPath() const noexcept;

    //==========================================================================
    /** Load an RVC preset model from disk (.pth / .onnx).
     *  Blocking — call from a background thread or on first-use lazy init.
     *  @return  Empty string on success, error message on failure. */
    juce::String loadPreset (const juce::File& modelFile,
                             ProgressFn        progress = {});

    /** Unload the current preset and release Python/ONNX resources. */
    void unloadPreset();

    //==========================================================================
    /** Re-synthesise a mono audio segment at the given target pitch.
     *
     *  @param inputMono    Source audio, single channel, float PCM.
     *  @param sampleRate   Sample rate of inputMono.
     *  @param f0TargetHz   Per-frame F0 contour at target pitch (Hz).
     *                      Frame hop = kF0HopSamples.  If shorter than the
     *                      segment, the last value is repeated.
     *  @param output       Output buffer, pre-allocated to the same size as
     *                      inputMono.  Written in-place by the bridge.
     *
     *  Returns false and leaves `output` unchanged on failure (caller should
     *  fall back to phase vocoder).
     *
     *  Must NOT be called from the audio thread. */
    bool resynthesizeAtPitch (const juce::AudioBuffer<float>& inputMono,
                              double                          sampleRate,
                              const std::vector<float>&       f0TargetHz,
                              juce::AudioBuffer<float>&       output);

    //==========================================================================
    /** F0 contour frame hop in samples (matches RVC internal hop = 512 @ 44.1kHz). */
    static constexpr int kF0HopSamples = 512;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RVCPythonBridge)
};
