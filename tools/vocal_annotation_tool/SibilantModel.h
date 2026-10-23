#pragma once

#include "AnnotationDocument.h"

#include <array>
#include <atomic>

namespace vocal_annotation
{

// The application analysis worker owns this offline classifier; no audio-thread access.
class SibilantModel final
{
public:
    struct Range { double start = 0.0, end = 0.0; };
    struct Inspection
    {
        std::vector<Range> ranges;
        juce::String error;
    };

    struct Prediction
    {
        std::vector<float> probabilities;
        std::vector<float> features; // Optional normalized, time-major parity diagnostics.
    };

    juce::Result load(const juce::File& file);
    Prediction predict(const juce::AudioBuffer<float>& audio, double sampleRate,
                       const std::atomic<bool>& cancelled, bool diagnostics = false) const;
    static Inspection inspectFile(const juce::File& audioFile, const juce::File& modelFile,
                                  const std::atomic<bool>& cancelled);
    // Every positive 10 ms frame is shown, without note coverage or duration filtering.
    static std::vector<Range> detectedRanges(const std::vector<float>& probabilities,
                                             float threshold, double duration);
    static bool hasVoicedCore(const NoteBlock& note, const std::vector<float>& probabilities, float threshold);
    static int mergeToNearest(std::vector<NoteBlock>& notes, const std::vector<float>& probabilities,
                              float threshold, double minimumCoverage = 0.50, double maximumGap = 0.12);
    const juce::String& id() const noexcept { return id_; }
    float threshold() const noexcept { return threshold_; }
    float detectionThreshold() const noexcept { return displayThreshold_; }

private:
    struct Layer { std::vector<float> weights, bias; };
    static std::vector<float> convolve(const std::vector<float>& input, const Layer& layer,
                                      int inputs, int outputs, int kernel, int dilation, bool relu,
                                      const std::atomic<bool>& cancelled);
    std::vector<float> makeFeatures(const juce::AudioBuffer<float>& audio, int start, int samples,
                                    int sampleRate, const std::atomic<bool>& cancelled) const;
    std::array<Layer, 14> layers_;
    std::vector<float> filters_, mean_, std_;
    juce::String id_;
    float threshold_ = 0.89f;
    float displayThreshold_ = 0.48f;
};

} // namespace vocal_annotation
