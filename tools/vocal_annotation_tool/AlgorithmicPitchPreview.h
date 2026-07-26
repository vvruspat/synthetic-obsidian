#pragma once

#include <JuceHeader.h>

#include <vector>

namespace vocal_annotation
{

struct AlgorithmicPitchCorrection
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float pitchRatio = 1.0f;
};

class AlgorithmicPitchPreview final
{
public:
    // Blocking and allocation-heavy by design. Call from a background thread only.
    static juce::Result render(const juce::File& sourceFile,
                               const juce::File& outputFile,
                               const std::vector<AlgorithmicPitchCorrection>& corrections);

private:
    static void applyCorrections(
        juce::AudioBuffer<float>& buffer,
        double sampleRate,
        const std::vector<AlgorithmicPitchCorrection>& corrections);
    static void phaseVocoderMono(float* data, int numSamples, float ratio);
};

} // namespace vocal_annotation
