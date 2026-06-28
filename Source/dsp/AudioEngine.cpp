#include "AudioEngine.h"

AudioEngine::AudioEngine()
    : pitchCorrection_()
{
}

AudioEngine::~AudioEngine()
{
    if (isPrepared_)
    {
        release();
    }
}

void AudioEngine::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    // Prepare pitch correction module — allocations happen here
    pitchCorrection_.prepare(sampleRate, maxBlockSize);

    isPrepared_ = true;
}

void AudioEngine::release()
{
    pitchCorrection_.reset();
    isPrepared_ = false;
}

void AudioEngine::process(juce::AudioBuffer<float>& buffer)
{
    // Guard: check if engine is prepared
    if (!isPrepared_)
    {
        buffer.clear();
        return;
    }

    // Read atomic parameters into local const variables (one load each)
    const float pitchDriftCents = pitchDriftCents_.load(std::memory_order_relaxed);
    const float formantShift    = formantShift_.load(std::memory_order_relaxed);
    const float aiInfluence     = aiInfluence_.load(std::memory_order_relaxed);
    const float vibratoScale    = vibratoScale_.load(std::memory_order_relaxed);
    (void)vibratoScale;  // reserved for Phase 2 vibrato processing

    // Apply pitch correction processing
    pitchCorrection_.process(buffer, pitchDriftCents, formantShift);

    // Apply AI influence as a simple dry/wet blend
    // When aiInfluence is low, attenuate toward dry (silence) for graceful degradation
    // When aiInfluence is high (0.84-1.0), process at full level
    if (aiInfluence < 1.0f)
    {
        float gainMultiplier = aiInfluence;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            float* samples = buffer.getWritePointer(ch);
            const int numSamples = buffer.getNumSamples();

            for (int i = 0; i < numSamples; ++i)
            {
                samples[i] *= gainMultiplier;
            }
        }
    }

    // Clamp output to [-1.0, 1.0] to prevent denormals and protect host
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        float* samples = buffer.getWritePointer(ch);
        const int numSamples = buffer.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            samples[i] = std::clamp(samples[i], -1.0f, 1.0f);
        }
    }
}

void AudioEngine::setPitchDriftCents(float cents)
{
    pitchDriftCents_.store(cents, std::memory_order_relaxed);
}

void AudioEngine::setFormantShift(float shift)
{
    formantShift_.store(shift, std::memory_order_relaxed);
}

void AudioEngine::setAiInfluence(float influence)
{
    aiInfluence_.store(influence, std::memory_order_relaxed);
}

void AudioEngine::setVibratoScale(float hz)
{
    vibratoScale_.store(hz, std::memory_order_relaxed);
}
