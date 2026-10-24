#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../uikit/Controls.h"
#include "TimeViewport.h"
#include "../uikit/Menu.h"

namespace obsidian::editor
{
// Editor chrome is a message-thread view. Model updates never emit callbacks.
class TransportBar final : public juce::Component
{
public:
    TransportBar();
    void setPlaying(bool);
    void setRecording(bool);
    void setRecordingAvailable(bool available) { record_.setEnabled(available); record_.setDescription(available ? "Record" : "Recording is not available in this editor"); }
    void setLooping(bool);
    // Position is zero-based quarter notes; display beats use the signature's
    // denominator and ticks remain 960 per quarter note. A 6/8 bar is 3 quarters.
    void setPosition(double beat, double seconds);
    void setTimeSignatures(std::vector<TimeViewport::TimeSignature> changes) { timing_.signatures = std::move(changes); repaint(); }
    void setProjectTitle(juce::String);
    const juce::String& getProjectTitle() const noexcept { return projectTitle_; }
    // Invalid signatures are ignored. Numerator: 1..32; denominator: powers of 2, 1..64.
    void setSignature(int numerator, int denominator);
    juce::String getPositionText() const;
    juce::String getSignatureText() const;
    // Labels are supplied by the host; actions report their zero-based index.
    void setProjectActions(juce::StringArray labels);
    void setTempo(double);
    void setMasterLevels(float left, float right);
    void setMasterVolume(double);
    std::function<void(bool)> onPlay, onRecord, onLoop;
    std::function<void()> onStop, onRewind;
    std::function<void(double)> onMasterVolume;
    std::function<void(int)> onProjectAction;
    ui::IconButton& getPlayButton() noexcept { return play_; }
    ui::IconButton& getStopButton() noexcept { return stop_; }
    ui::Slider& getMasterSlider() noexcept { return master_; }
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    ui::Button project_ { "Project", ui::Icon::chevronDown };
    ui::IconButton rewind_ { ui::Icon::rewind, "Rewind" };
    ui::IconButton play_ { ui::Icon::play, "Play" };
    ui::IconButton stop_ { ui::Icon::stop, "Stop" };
    ui::IconButton record_ { ui::Icon::record, "Record" };
    ui::IconButton loop_ { ui::Icon::loop, "Loop" };
    ui::Slider master_;
    ui::LevelMeter meter_;
    ui::Menu projectMenu_;
    ui::Tooltip playTooltip_ { play_, "Play or pause playback" };
    ui::Tooltip loopTooltip_ { loop_, "Repeat the current loop" };
    juce::Rectangle<int> brandBounds_, titleBounds_, positionBounds_, timeBounds_, tempoBounds_, signatureBounds_, masterLabelBounds_;
    bool playing_ = false, recording_ = false, looping_ = false;
    double beat_ = 0.0, seconds_ = 0.0, tempo_ = 94.0;
    juce::String projectTitle_;
    int numerator_ = 4, denominator_ = 4;
    TimeViewport timing_;
};

class TrackHeader final : public juce::Component
{
public:
    TrackHeader();
    void setTrack(int number, juce::String name, juce::Colour accent, ui::Icon icon);
    void setGain(double db);
    void setMuted(bool);
    void setSolo(bool);
    std::function<void(bool)> onMute, onSolo;
    std::function<void(double)> onGain;
    std::function<void()> onMenu;
    ui::ToggleButton& getMuteButton() noexcept { return mute_; }
    ui::ToggleButton& getSoloButton() noexcept { return solo_; }
    ui::Slider& getGainSlider() noexcept { return gain_; }
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    int number_ = 1;
    juce::String name_ = "Audio";
    juce::Colour accent_ { ui::tokens::blue };
    ui::Icon icon_ = ui::Icon::waveform;
    ui::ToggleButton mute_ { "M" }, solo_ { "S" };
    ui::Slider gain_;
    ui::IconButton menu_ { ui::Icon::more, "Track actions" };
    ui::Tooltip muteTooltip_ { mute_, "Mute track" };
    ui::Tooltip soloTooltip_ { solo_, "Solo track" };
};

class TrackGroupHeader final : public juce::Component
{
public:
    TrackGroupHeader();
    void setExpanded(bool);
    bool isExpanded() const noexcept { return expanded_; }
    void setTitle(juce::String);
    std::function<void(bool)> onExpanded;
    std::function<void()> onAdd;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    bool expanded_ = true;
    ui::Button heading_ { "Backing Vocals", ui::Icon::chevronDown };
    ui::Button add_ { "Add backing", ui::Icon::plus };
};
}
