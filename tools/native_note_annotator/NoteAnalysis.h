#pragma once

#include "AnnotationDocument.h"

#include <atomic>

namespace native_annotation
{
struct AnalysisFrame
{
    double time = 0.0;
    double midi = 60.0;
    double confidence = 0.0;
    float rms = 0.0f;
    bool voiced = false;
};

struct NoteAnalysis
{
    std::vector<AnalysisFrame> frames;
    std::vector<vocal_annotation::NoteBlock> notes;
    double duration = 0.0;

    // Offline only. Cancellation is checked between frames.
    static NoteAnalysis analyze(const juce::AudioBuffer<float>& audio, double sampleRate,
                                const std::atomic<bool>& cancelled, std::atomic<float>& progress);
    vocal_annotation::NoteBlock suggest(double start, double limit) const;
    vocal_annotation::NoteBlock noteForInterval(double start, double end) const;
};
}
