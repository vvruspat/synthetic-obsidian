#include "Gallery.h"

namespace obsidian::demo
{
namespace
{
constexpr int kCardPadding = 18;
constexpr int kCardTitleHeight = 36;
constexpr int kGap = 16;
constexpr int kRowHeight = 38;

void label(juce::Graphics& g, const juce::String& value, juce::Rectangle<int> bounds,
           float size = ui::tokens::fontBody, juce::uint32 colour = ui::tokens::text)
{
    g.setColour(ui::colour(colour));
    g.setFont(ui::font(size));
    g.drawText(value, bounds, juce::Justification::centredLeft, true);
}

class SectionContent final : public juce::Component
{
public:
    SectionContent()
    {
        addAndMakeVisible(button_);
        button_.onClick = [this] { auto callback = onAction; if (callback) callback(); };
    }
    void paint(juce::Graphics& g) override
    {
        if (getHeight() >= 60)
            label(g, "Owned content stays alive when collapsed.", getLocalBounds().reduced(10).removeFromTop(24), 12.0f, ui::tokens::muted);
    }
    void resized() override { button_.setBounds(10, getHeight() >= 60 ? 36 : 6, juce::jmax(0, juce::jmin(200, getWidth() - 20)), 28); }
    std::function<void()> onAction;
private:
    ui::Button button_ { "Content action", ui::Icon::plus };
};
}

void Gallery::Card::paint(juce::Graphics& g)
{
    ui::Panel::paint(g);
    label(g, heading, getLocalBounds().reduced(kCardPadding).removeFromTop(24), 14.0f);
    if (onPaint) onPaint(g);
}

Gallery::Gallery()
{
    setTitle("Obsidian UI component gallery");
    addAndMakeVisible(showDemo_);
    showDemo_.onClick = [this] { auto callback = onShowDemo; if (callback) callback(); };
    buttonsPanel_.heading = "Buttons, variants and states";
    valuesPanel_.heading = "Values and controlled model updates";
    layoutPanel_.heading = "Toolbar, panels and collapsible content";
    eventsPanel_.heading = "Interaction log";
    valuesPanel_.onPaint = [this](juce::Graphics& g)
    {
        label(g, "Gain", { kCardPadding, gain_.getY(), 60, gain_.getHeight() }, 12.0f, ui::tokens::muted);
        label(g, "Zoom", { kCardPadding, zoom_.getY(), 60, zoom_.getHeight() }, 12.0f, ui::tokens::muted);
    };
    eventsPanel_.onPaint = [this](juce::Graphics& g)
    {
        int y = 46;
        for (const auto& event : events_)
        {
            label(g, event, { kCardPadding, y, eventsPanel_.getWidth() - 2 * kCardPadding, 18 }, 12.0f, ui::tokens::muted);
            y += 18;
        }
    };
    for (auto* panel : { &buttonsPanel_, &valuesPanel_, &layoutPanel_, &eventsPanel_ })
        addAndMakeVisible(panel);
    for (auto* control : std::initializer_list<juce::Component*> { &normal_, &subtle_, &accent_, &small_, &large_, &disabled_, &mute_, &solo_, &selected_, &disabledToggle_, &play_, &record_, &help_ })
        buttonsPanel_.addAndMakeVisible(control);
    for (auto* control : std::initializer_list<juce::Component*> { &gain_, &zoom_, &select_, &menuButton_, &modelUpdate_, &meter_ })
        valuesPanel_.addAndMakeVisible(control);
    layoutPanel_.addAndMakeVisible(toolbar_);
    layoutPanel_.addAndMakeVisible(section_);
    subtle_.setVariant(ui::Button::Variant::subtle);
    accent_.setVariant(ui::Button::Variant::accent);
    small_.setSize(ui::Button::Size::small);
    large_.setSize(ui::Button::Size::large);
    disabled_.setEnabled(false);
    disabledToggle_.setEnabled(false);
    disabledToggle_.setToggleState(true);
    selected_.setToggleState(true);
    gain_.setTitle("Gain in decibels");
    gain_.setRange(-60.0, 6.0, 0.1);
    gain_.setValue(-8.0);
    gain_.valueToText = [](double value) { return juce::String(value, 1) + " dB"; };
    zoom_.setTitle("Zoom percentage");
    zoom_.setRange(50.0, 200.0, 10.0);
    zoom_.setValue(100.0);
    zoom_.setAccent(ui::colour(ui::tokens::olive));
    zoom_.valueToText = [](double value) { return juce::String(static_cast<int>(value)) + "%"; };
    select_.setTitle("Track selection");
    select_.setItems({ "Instrumental", "Voice Main", "Audio 3", "Unison Double" });
    select_.setSelectedIndex(1);
    meter_.setLevels(0.62f, 0.45f);
    for (auto* button : { &normal_, &subtle_, &accent_, &small_, &large_ })
        button->onClick = [this, button] { log(button->getText() + " button activated"); };
    mute_.onToggle = [this](bool state) { ++notificationCount_; log(state ? "Mute on" : "Mute off"); };
    solo_.onToggle = [this](bool state) { ++notificationCount_; log(state ? "Solo on" : "Solo off"); };
    selected_.onToggle = [this](bool state) { ++notificationCount_; log(state ? "Selected on" : "Selected off"); };
    play_.onClick = [this] { play_.setSelected(!play_.isSelected()); log(play_.isSelected() ? "Playback started" : "Playback paused"); };
    record_.onClick = [this] { record_.setSelected(!record_.isSelected()); log(record_.isSelected() ? "Record armed" : "Record disarmed"); };
    help_.onClick = [this] { log("Tooltip example activated; hold hover or focus to inspect"); };
    gain_.onDragStart = [this] { log("Gain gesture began"); };
    gain_.onValueChange = [this](double) { ++notificationCount_; meter_.setLevels(static_cast<float>((gain_.getValue() + 60.0) / 66.0), 0.45f); log("Gain changed: " + gain_.getValueText()); };
    gain_.onDragEnd = [this] { log("Gain gesture ended"); };
    zoom_.onDragStart = [this] { log("Zoom gesture began"); };
    zoom_.onValueChange = [this](double) { ++notificationCount_; log("Zoom changed: " + zoom_.getValueText()); };
    zoom_.onDragEnd = [this] { log("Zoom gesture ended"); };
    select_.onChange = [this](int) { ++notificationCount_; log("Track selected: " + select_.getSelectedText()); };
    modelUpdate_.onClick = [this] { updateModel(); };
    menuButton_.onClick = [this]
    {
        menu_.setItems({
            { "Rename track", [this] { log("Menu: rename track"); } },
            { "Duplicate track", [this] { log("Menu: duplicate track"); } },
            { "Unavailable action", {}, false },
            { "Checked example", [this] { log("Menu: checked example"); }, true, true }
        });
        menu_.showFor(menuButton_);
    };
    toolbar_.addItem(pointer_, 36);
    toolbar_.addItem(pencil_, 36);
    toolbar_.addItem(scissors_, 36);
    toolbar_.addItem(divider_, 1);
    pointer_.setSelected(true);
    for (auto* button : { &pointer_, &pencil_, &scissors_ })
        button->onClick = [this, button]
        {
            pointer_.setSelected(button == &pointer_);
            pencil_.setSelected(button == &pencil_);
            scissors_.setSelected(button == &scissors_);
            log(button->getTitle() + " selected");
        };
    auto content = std::make_unique<SectionContent>();
    content->onAction = [this] { log("Collapsible content action"); };
    section_.setContent(std::move(content));
    section_.onExpandedChange = [this](bool expanded) { log(expanded ? "Section expanded" : "Section collapsed"); };
    log("Ready | Tab to navigate; Space/Enter to activate; arrow keys adjust values.");
}

Gallery::~Gallery()
{
    // Slider destruction can end an active gesture. End both while the log and
    // meter used by their callbacks are still alive, then disconnect the owner.
    for (auto* slider : { &gain_, &zoom_ })
    {
        slider->cancelDrag();
        slider->onDragStart = {};
        slider->onValueChange = {};
        slider->onDragEnd = {};
    }
}

void Gallery::log(juce::String message)
{
    events_.insert(0, std::move(message));
    while (events_.size() > 5)
        events_.remove(events_.size() - 1);
    repaint();
}

void Gallery::updateModel()
{
    const int before = notificationCount_;
    ++modelRevision_;
    gain_.setValue(modelRevision_ % 2 == 0 ? -8.0 : -3.0, juce::dontSendNotification);
    zoom_.setValue(modelRevision_ % 2 == 0 ? 100.0 : 150.0, juce::dontSendNotification);
    select_.setSelectedIndex(modelRevision_ % 2 == 0 ? 1 : 2, juce::dontSendNotification);
    mute_.setToggleState(modelRevision_ % 2 != 0, juce::dontSendNotification);
    meter_.setLevels(0.62f, 0.45f);
    log("Model revision " + juce::String(modelRevision_) + ": " + juce::String(notificationCount_ - before) + " value callbacks (expected 0)");
}

void Gallery::paint(juce::Graphics& g)
{
    g.fillAll(ui::colour(ui::tokens::background));
    label(g, "UI COMPONENTS", { 24, 12, getWidth() - 220, 24 }, 18.0f);
    label(g, "Native JUCE components | hover / press / select / disable / focus", { 24, 40, getWidth() - 220, 24 }, 12.0f, ui::tokens::muted);
}

void Gallery::resized()
{
    // Every control stays in a bounded card; the demo window sets a 1000px minimum width.
    showDemo_.setBounds(getWidth() - 174, 20, 150, 30);
    auto area = getLocalBounds().reduced(24);
    area.removeFromTop(52);
    auto eventArea = area.removeFromBottom(150);
    eventsPanel_.setBounds(eventArea);
    area.removeFromBottom(kGap);
    auto lower = area.removeFromBottom(getHeight() < 800 ? 160 : 202);
    layoutPanel_.setBounds(lower);
    area.removeFromBottom(kGap);
    const int half = (area.getWidth() - kGap) / 2;
    buttonsPanel_.setBounds(area.removeFromLeft(half));
    area.removeFromLeft(kGap);
    valuesPanel_.setBounds(area);
    auto left = buttonsPanel_.getLocalBounds().reduced(kCardPadding);
    left.removeFromTop(kCardTitleHeight);
    const int buttonWidth = (left.getWidth() - 16) / 3;
    int y = left.getY();
    normal_.setBounds(left.getX(), y, buttonWidth, 30);
    subtle_.setBounds(left.getX() + buttonWidth + 8, y, buttonWidth, 30);
    accent_.setBounds(left.getX() + (buttonWidth + 8) * 2, y, buttonWidth, 30);
    y += 44;
    small_.setBounds(left.getX(), y + 7, buttonWidth, 24);
    large_.setBounds(left.getX() + buttonWidth + 8, y, buttonWidth, 38);
    disabled_.setBounds(left.getX() + (buttonWidth + 8) * 2, y + 4, buttonWidth, 30);
    y += 52;
    const int toggleWidth = (left.getWidth() - 24) / 4;
    int x = left.getX();
    for (auto* toggle : { &mute_, &solo_, &selected_, &disabledToggle_ })
    {
        toggle->setBounds(x, y, toggleWidth, 28);
        x += toggleWidth + 8;
    }
    y += 44;
    x = left.getX();
    for (auto* button : { &play_, &record_, &help_ })
    {
        button->setBounds(x, y, 36, 32);
        x += 44;
    }
    auto right = valuesPanel_.getLocalBounds().reduced(kCardPadding);
    right.removeFromTop(kCardTitleHeight);
    gain_.setBounds(right.removeFromTop(kRowHeight).withTrimmedLeft(64));
    zoom_.setBounds(right.removeFromTop(kRowHeight).withTrimmedLeft(64));
    auto choices = right.removeFromTop(kRowHeight).reduced(0, 4);
    select_.setBounds(choices.removeFromLeft((choices.getWidth() - 10) / 2));
    choices.removeFromLeft(10);
    menuButton_.setBounds(choices);
    right.removeFromTop(8);
    modelUpdate_.setBounds(right.removeFromTop(30));
    right.removeFromTop(12);
    meter_.setBounds(right.removeFromTop(22));
    auto layout = layoutPanel_.getLocalBounds().reduced(kCardPadding);
    layout.removeFromTop(kCardTitleHeight);
    toolbar_.setBounds(layout.removeFromLeft(200).removeFromTop(42));
    layout.removeFromLeft(kGap);
    section_.setBounds(layout);
    repaint();
}
}
