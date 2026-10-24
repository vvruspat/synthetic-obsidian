#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include "Theme.h"

namespace obsidian::ui
{
// Non-owning anchor; owned content should be added as children by subclasses.
// The owner keeps Popover alive. It is placed inside the anchor's top-level view.
class Popover : public juce::Component
{
public:
    Popover();
    ~Popover() override;
    void showFor(juce::Component& anchor, int width, int height);
    void dismiss();
    bool isOpen() const noexcept { return isVisible() && getParentComponent() != nullptr; }
    std::function<void()> onDismiss;
    void paint(juce::Graphics&) override;
    void inputAttemptWhenModal() override;
    bool keyPressed(const juce::KeyPress&) override;
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    juce::Component::SafePointer<juce::Component> anchor_;
};

class Menu final : public Popover
{
public:
    struct Item
    {
        juce::String text;
        std::function<void()> action;
        bool enabled = true;
        bool checked = false;
    };
    Menu();
    ~Menu() override;
    void setItems(std::vector<Item>);
    void showFor(juce::Component& anchor, int width = 200, int selectedIndex = -1);
    bool keyPressed(const juce::KeyPress&) override;
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    void highlight(int);
    void activate(int);
    std::vector<Item> items_;
    std::vector<std::unique_ptr<juce::Component>> rows_;
    int highlighted_ = -1;
    int firstVisibleRow_ = 0;
};

// Reusable tooltip presenter. Own it alongside the target; it observes the
// target weakly and displays after a delay while hovered, or while focused.
class Tooltip final : public juce::Component, private juce::Timer
{
public:
    Tooltip(juce::Component& target, juce::String text, int delayMs = 550);
    ~Tooltip() override;
    void setText(juce::String);
    void paint(juce::Graphics&) override;
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    void timerCallback() override;
    juce::Component::SafePointer<juce::Component> target_;
    juce::String text_;
    juce::int64 hoverStart_ = 0;
    int delayMs_;
};
}
