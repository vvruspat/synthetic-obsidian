#pragma once

#include "NoteAnalysis.h"

#include <array>

namespace native_annotation
{
// Offline-only runner for the fixed residual TCN exported by research/native_notes.
// The model owns ordinary float arrays; no Python/ONNX runtime or audio callback access.
class NoteBoundaryModel final
{
public:
    struct Prediction
    {
        std::vector<std::array<float, 3>> probabilities;
        std::vector<float> features; // Time-major, schema-defined inputs. Used for parity checks.
    };
    juce::Result load(const juce::File& file);
    Prediction predict(const juce::AudioBuffer<float>& audio, double sampleRate,
                       const std::atomic<bool>& cancelled, std::atomic<float>& progress,
                       const NoteAnalysis* analysis = nullptr) const;
    std::vector<std::pair<double, double>> decode(const Prediction& prediction, double duration) const;
    void apply(NoteAnalysis& analysis, const Prediction& prediction) const;
    const juce::String& id() const noexcept { return id_; }
    const juce::String& name() const noexcept { return name_; }

private:
    struct Layer { std::vector<float> weights, bias; };
    std::vector<float> makeFeatures(const juce::AudioBuffer<float>& audio, double sampleRate,
                                    const std::atomic<bool>& cancelled, const NoteAnalysis* analysis) const;
    static std::vector<float> convolve(const std::vector<float>& input, const Layer& layer,
                                      int inputs, int outputs, int kernel, int dilation, bool relu,
                                      const std::atomic<bool>& cancelled);
    std::vector<Layer> layers_ = std::vector<Layer>(10);
    std::vector<int> dilations_ { 1, 2, 4, 8 };
    std::vector<float> filters_, longFilters_;
    int featureCount_ = 64;
    std::array<float, 3> thresholds_ { 0.5f, 0.5f, 0.5f };
    juce::String id_, name_;
};

struct NoteDraftResult
{
    NoteAnalysis analysis;
    juce::String modelId = "yin-energy-v1";
    juce::String modelHash;
    juce::String name = "Math (YIN + energy)";
    juce::String warning;
};

// Always called from the annotation worker. Missing/invalid models retain the math fallback.
NoteDraftResult analyzeDraft(const juce::AudioBuffer<float>& audio, double sampleRate,
                             const juce::File& modelFile, const std::atomic<bool>& cancelled,
                             std::atomic<float>& progress);
}
