#include "RVCPythonBridge.h"

//==============================================================================
// Phase 1 stub implementation.
//
// isAvailable() always returns false — all callers fall back to phase vocoder.
// Phase 2 will replace this file with a real pybind11 / subprocess bridge.
//
// The Impl struct is kept so the pimpl boundary is already established;
// Phase 2 only needs to fill in the body without changing any headers.
//==============================================================================

struct RVCPythonBridge::Impl
{
    bool         available    { false };
    juce::String presetPath;
};

//==============================================================================
RVCPythonBridge::RVCPythonBridge()
    : impl_ (std::make_unique<Impl>())
{
}

RVCPythonBridge::~RVCPythonBridge() = default;

//==============================================================================
bool RVCPythonBridge::isAvailable() const noexcept
{
    return impl_->available;
}

juce::String RVCPythonBridge::currentPresetPath() const noexcept
{
    return impl_->presetPath;
}

//==============================================================================
juce::String RVCPythonBridge::loadPreset (const juce::File& modelFile,
                                           ProgressFn        progress)
{
    // Phase 1: Python runtime not yet embedded.
    juce::ignoreUnused (modelFile, progress);
    return "RVC Python bridge not yet implemented (Phase 2).";
}

void RVCPythonBridge::unloadPreset()
{
    impl_->available  = false;
    impl_->presetPath = {};
}

//==============================================================================
bool RVCPythonBridge::resynthesizeAtPitch (const juce::AudioBuffer<float>& inputMono,
                                            double                          sampleRate,
                                            const std::vector<float>&       f0TargetHz,
                                            juce::AudioBuffer<float>&       output)
{
    // Phase 1 stub — should never be reached because isAvailable() == false.
    // Defensive: copy input to output unchanged so caller gets dry audio.
    juce::ignoreUnused (sampleRate, f0TargetHz);

    if (output.getNumSamples() >= inputMono.getNumSamples()
        && inputMono.getNumChannels() > 0
        && output.getNumChannels()    > 0)
    {
        output.copyFrom (0, 0,
                         inputMono, 0, 0,
                         inputMono.getNumSamples());
    }

    return false;   // signal failure so caller can fall back
}
