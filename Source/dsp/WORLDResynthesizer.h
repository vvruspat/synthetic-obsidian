#pragma once
#include <JuceHeader.h>
#include <vector>

/** WORLDResynthesizer
 *
 *  Offline pitch correction using the WORLD vocoder (via pyworld Python lib).
 *
 *  ADR-006 — PRIMARY pitch correction path:
 *    WORLD decomposes the vocal signal into F0 + SpectralEnvelope + Aperiodicity.
 *    Only F0 is replaced; timbre (formants) and breathiness are preserved exactly
 *    as they were in the original clip — no training required.
 *
 *  Architecture
 *  ────────────
 *  A Python child process (world_bridge.py) is spawned once and kept alive.
 *  Each call to resynthesizeAtPitch() sends one binary frame over stdin and
 *  reads one binary frame from stdout (see world_bridge.py for protocol spec).
 *  This avoids Python startup cost on every correction.
 *
 *  Thread safety
 *  ─────────────
 *    startBridge / stopBridge    — call before/after background work begins
 *    resynthesizeAtPitch         — background thread only, NOT audio thread
 *    isAvailable                 — any thread (atomic read)
 *
 *  Lifecycle
 *  ─────────
 *    Construction does NOT start the subprocess (lazy on first use).
 *    Explicit startBridge() / stopBridge() are provided for PluginProcessor
 *    to manage the process lifetime cleanly across plugin load/unload.
 */
class WORLDResynthesizer
{
public:
    WORLDResynthesizer();
    ~WORLDResynthesizer();

    //==========================================================================
    /** Spawn the Python child process and check that pyworld is importable.
     *  Call from a background thread before the first correction.
     *  Returns true on success (isAvailable() will then return true). */
    bool startBridge();

    /** Terminate the child process cleanly. */
    void stopBridge();

    /** True when the bridge process is running and pyworld is available. */
    bool isAvailable() const noexcept { return available_.load(); }

    //==========================================================================
    /** Re-synthesise `inputMono` at the given target F0 contour.
     *
     *  @param inputMono   Source audio — must be single-channel, float PCM.
     *  @param sampleRate  Sample rate of inputMono.
     *  @param f0TargetHz  Per-frame target F0 in Hz (0 = unvoiced/keep silent).
     *                     Frame hop = kF0HopSamples.  Contour is resampled to
     *                     the WORLD 5ms grid internally.
     *  @param output      Pre-allocated output buffer (same size as inputMono).
     *                     Written on success; untouched on failure.
     *
     *  Returns false on failure — caller must fall back to phase vocoder. */
    bool resynthesizeAtPitch (const juce::AudioBuffer<float>& inputMono,
                              double                          sampleRate,
                              const std::vector<float>&       f0TargetHz,
                              juce::AudioBuffer<float>&       output);

    //==========================================================================
    /** Nominal F0 contour frame hop used by callers to size the f0TargetHz vector.
     *  Matches RVC default (512 samples @ 44.1kHz ≈ 11.6 ms). */
    static constexpr int kF0HopSamples = 512;

    //==========================================================================
    /** Locate world_bridge.py relative to the plugin binary or build tree.
     *  Returns File() if not found — startBridge() will fail gracefully. */
    static juce::File findBridgeScript();

private:
    //==========================================================================
    // Binary protocol helpers (see world_bridge.py for spec)
    bool sendFrame  (const float* audio, int numSamples,
                     double sampleRate,
                     const std::vector<double>& f0Target);
    bool recvFrame  (float* output, int numSamples);

    // Low-level socket helpers
    bool sockWrite (const void* data, int bytes);
    bool sockRead  (void* data, int bytes);

    //==========================================================================
    juce::ChildProcess    process_;
    juce::StreamingSocket socket_;
    std::atomic<bool>     available_ { false };

    // Serialise calls — only one correction may run at a time per instance
    juce::CriticalSection callLock_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WORLDResynthesizer)
};
