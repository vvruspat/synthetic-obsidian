#pragma once

#include "NoteEditing.h"

#include <functional>

namespace native_annotation
{
class WaveformEditor final : public juce::Component
{
public:
    WaveformEditor(vocal_annotation::AnnotationDocument& document, const NoteAnalysis& analysis);
    void setAudio(const juce::AudioBuffer<float>& audio);
    void fit();
    void zoom(double factor, double pivot = -1.0);
    void setPlayhead(double time);
    void clearSelection();
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float factor) override;
    bool keyPressed(const juce::KeyPress& key) override;

    std::function<void()> beginEdit;
    std::function<void()> changed;
    std::function<void(double, double)> audition;
    std::function<void(double)> seek;
    std::function<bool(const juce::KeyPress&)> shortcut;

private:
    juce::Rectangle<float> waveBounds() const;
    float xAt(double time) const;
    double timeAt(float x) const;
    std::optional<size_t> noteAt(double time) const;
    void pan(double delta);

    vocal_annotation::AnnotationDocument& document_;
    const NoteAnalysis& analysis_;
    struct Peak { float low = 0.0f; float high = 0.0f; };
    std::vector<Peak> peaks_;
    double start_ = 0.0;
    double end_ = 10.0;
    double playhead_ = 0.0;
    juce::String selected_;
    std::optional<size_t> dragging_;
    bool startEdge_ = false;
    vocal_annotation::NoteBlock original_;
    std::optional<vocal_annotation::NoteBlock> neighborOriginal_;
    size_t neighborIndex_ = 0;
};
}
