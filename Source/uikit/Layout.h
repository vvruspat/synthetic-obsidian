#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "Controls.h"

namespace obsidian::ui
{
class Panel : public juce::Component
{
public:
    explicit Panel(juce::Colour background = colour(tokens::surface));
    void setBackground(juce::Colour);
    void paint(juce::Graphics&) override;
private:
    juce::Colour background_;
};

class Divider final : public juce::Component
{
public:
    explicit Divider(bool vertical = false);
    void paint(juce::Graphics&) override;
private:
    bool vertical_;
};

class Toolbar final : public Panel
{
public:
    Toolbar();
    // Ownership remains with caller. A width of 0 shares leftover space.
    void addItem(juce::Component&, int width = tokens::controlHeight);
    void clearItems();
    void resized() override;
private:
    struct Item { juce::Component::SafePointer<juce::Component> component; int width; };
    std::vector<Item> items_;
};

class CollapsibleSection final : public Panel
{
public:
    explicit CollapsibleSection(juce::String title = {});
    void setContent(std::unique_ptr<juce::Component>);
    juce::Component* getContent() const noexcept { return content_.get(); }
    void setExpanded(bool, juce::NotificationType = juce::dontSendNotification);
    bool isExpanded() const noexcept { return expanded_; }
    void setTitle(juce::String);
    std::function<void(bool)> onExpandedChange;
    void resized() override;
private:
    Button header_;
    std::unique_ptr<juce::Component> content_;
    bool expanded_ = true;
};
}
