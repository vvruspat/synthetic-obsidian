#pragma once
#include <JuceHeader.h>
#include "PitchCorrectionModule.h"

// AudioEngine owns all real-time DSP processing.
// Lives in PluginProcessor. Called from processBlock — audio thread only.
// All setup happens in prepare(), never during process().
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    // Called from prepareToPlay — message thread, safe to allocate
    void prepare(double sampleRate, int maxBlockSize);

    // Called from releaseResources
    void release();

    // Called from processBlock — audio thread, NO allocation, NO locks
    void process(juce::AudioBuffer<float>& buffer);

    // Thread-safe setters called from message/UI thread via atomics
    void setPitchDriftCents(float cents);
    void setFormantShift(float shift);      // -1.0 to +1.0
    void setAiInfluence(float influence);   // 0.0 to 1.0
    void setVibratoScale(float hz);         // 0.0 to 10.0

private:
    PitchCorrectionModule pitchCorrection_;

    std::atomic<float> pitchDriftCents_  { 0.0f };
    std::atomic<float> formantShift_     { 0.0f };
    std::atomic<float> aiInfluence_      { 0.84f };
    std::atomic<float> vibratoScale_     { 3.4f };

    double sampleRate_    { 44100.0 };
    int    maxBlockSize_  { 512 };
    bool   isPrepared_    { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
