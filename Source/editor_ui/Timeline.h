#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "TimeViewport.h"

namespace obsidian::editor
{
// Message-thread surfaces borrow one viewport owned by their enclosing editor.
class EditorSurface : public juce::Component
{
public:
    explicit EditorSurface(const TimeViewport&);
    virtual void viewportChanged();
    // The owner synchronises keyboard seeking with the current shared playhead.
    void setCurrentBeat(double);
    std::function<void(double)> onSeek;
    std::function<void(double, double, float)> onViewportGesture;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(juce::Component::FocusChangeType) override;
    void focusLost(juce::Component::FocusChangeType) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
protected:
    void seekAt(float x);
    const TimeViewport& viewport_;
private:
    double keyboardBeat_ = 0.0;
};

class TimelineRuler final : public EditorSurface
{
public:
    explicit TimelineRuler(const TimeViewport&);
    void paint(juce::Graphics&) override;
};

class TimelineGrid final : public EditorSurface
{
public:
    explicit TimelineGrid(const TimeViewport&);
    void setRowHeight(float);
    void resized() override;
    void viewportChanged() override;
    void paint(juce::Graphics&) override;
private:
    float rowHeight_ = 64.0f;
    juce::Path minor_, major_, rows_;
};

class Playhead final : public juce::Component
{
public:
    explicit Playhead(const TimeViewport&);
    void setBeat(double);
    double beat() const noexcept { return beat_; }
    void viewportChanged();
    void paint(juce::Graphics&) override;
private:
    const TimeViewport& viewport_;
    double beat_ = 0.0;
};

class TempoLane final : public EditorSurface
{
public:
    explicit TempoLane(const TimeViewport&);
    void setTempo(double);
    void setPointBeats(std::vector<double>);
    void paint(juce::Graphics&) override;
private:
    double tempo_ = 94.0;
    std::vector<double> pointBeats_;
};

class SignatureLane final : public EditorSurface
{
public:
    explicit SignatureLane(const TimeViewport&);
    void setSignature(int numerator, int denominator);
    void paint(juce::Graphics&) override;
private:
    int numerator_ = 4, denominator_ = 4;
};

struct ChordBlock
{
    double startBeat = 0.0, lengthBeats = 4.0;
    juce::String label;
};

class ChordLane final : public EditorSurface
{
public:
    explicit ChordLane(const TimeViewport&);
    void setChords(std::vector<ChordBlock>);
    void paint(juce::Graphics&) override;
private:
    std::vector<ChordBlock> chords_;
};

struct AudioClipData
{
    double startBeat = 0.0, lengthBeats = 4.0;
    juce::String name;
    juce::Colour colour { 0xff91accd };
    // Precomputed absolute peak amplitudes, uniformly spaced across the clip.
    std::vector<float> peaks;
    bool selected = false;
};

class AudioClip final : public EditorSurface
{
public:
    explicit AudioClip(const TimeViewport&);
    void setData(AudioClipData);
    void setSelected(bool);
    const AudioClipData& data() const noexcept { return data_; }
    juce::Rectangle<float> clipBounds() const noexcept;
    std::size_t geometryRevision() const noexcept { return geometryRevision_; }
    std::function<void()> onSelect;
    void resized() override;
    void viewportChanged() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
private:
    void rebuildWaveform();
    AudioClipData data_;
    juce::Path waveform_;
    std::size_t geometryRevision_ = 0;
};
}
