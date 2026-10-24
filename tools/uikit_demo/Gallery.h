#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include "../../Source/uikit/Controls.h"
#include "../../Source/uikit/Menu.h"
#include "../../Source/uikit/Layout.h"

namespace obsidian::demo
{
class Gallery final : public juce::Component
{
public:
    Gallery();
    ~Gallery() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    std::function<void()> onShowDemo;
    const juce::StringArray& getEventLog() const noexcept { return events_; }
    ui::Slider& getGainSlider() noexcept { return gain_; }
    ui::Slider& getZoomSlider() noexcept { return zoom_; }
    ui::Button& getModelUpdateButton() noexcept { return modelUpdate_; }
private:
    void log(juce::String);
    void updateModel();
    class Card final : public ui::Panel
    {
    public:
        void paint(juce::Graphics&) override;
        juce::String heading;
        std::function<void(juce::Graphics&)> onPaint;
    };
    Card buttonsPanel_, valuesPanel_, layoutPanel_, eventsPanel_;
    ui::Button showDemo_ { "Back to editor", ui::Icon::chevronRight };
    ui::Button normal_ { "Normal" }, subtle_ { "Subtle" }, accent_ { "Accent", ui::Icon::plus };
    ui::Button small_ { "Small" }, large_ { "Large" }, disabled_ { "Disabled" };
    ui::ToggleButton mute_ { "Mute" }, solo_ { "Solo" }, selected_ { "Selected" }, disabledToggle_ { "Disabled" };
    ui::IconButton play_ { ui::Icon::play, "Play demo" }, record_ { ui::Icon::record, "Record demo" }, help_ { ui::Icon::search, "Tooltip example" };
    ui::Slider gain_, zoom_;
    ui::Select select_;
    ui::Button menuButton_ { "Open menu", ui::Icon::chevronDown };
    ui::Button modelUpdate_ { "Apply model update (silent)", ui::Icon::check };
    ui::Menu menu_;
    ui::Tooltip tooltip_ { help_, "Custom tooltip: shown on hover and keyboard focus." };
    ui::LevelMeter meter_;
    ui::CollapsibleSection section_ { "Collapsible section" };
    ui::Toolbar toolbar_;
    ui::IconButton pointer_ { ui::Icon::pointer, "Pointer tool" }, pencil_ { ui::Icon::pencil, "Pencil tool" }, scissors_ { ui::Icon::scissors, "Scissors tool" };
    ui::Divider divider_ { true };
    juce::StringArray events_;
    int notificationCount_ = 0, modelRevision_ = 0;
};
}
