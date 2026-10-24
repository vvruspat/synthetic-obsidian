#include "Layout.h"

#include <utility>

namespace obsidian::ui
{
Panel::Panel(juce::Colour background) : background_(background) {}
void Panel::setBackground(juce::Colour background) { background_ = background; repaint(); }
void Panel::paint(juce::Graphics& g)
{
    g.setColour(background_); g.fillRect(getLocalBounds());
}
Divider::Divider(bool vertical) : vertical_(vertical) { setAccessible(false); }
void Divider::paint(juce::Graphics& g)
{
    g.setColour(colour(tokens::border));
    if (vertical_) g.drawVerticalLine(getWidth() / 2, 0.0f, static_cast<float>(getHeight()));
    else g.drawHorizontalLine(getHeight() / 2, 0.0f, static_cast<float>(getWidth()));
}
Toolbar::Toolbar() : Panel(colour(tokens::surface)) {}
void Toolbar::addItem(juce::Component& component, int width)
{
    items_.push_back({ &component, juce::jmax(0, width) }); addAndMakeVisible(component); resized();
}
void Toolbar::clearItems()
{
    for (const auto& item : items_) if (item.component != nullptr) removeChildComponent(item.component);
    items_.clear();
}
void Toolbar::resized()
{
    auto bounds = getLocalBounds().reduced(tokens::gap, 4);
    auto fixed = 0, flexible = 0, count = 0;
    for (const auto& item : items_) if (item.component != nullptr)
    {
        ++count;
        if (item.width == 0) ++flexible; else fixed += item.width;
    }
    const auto space = juce::jmax(0, bounds.getWidth() - fixed - juce::jmax(0, count - 1) * tokens::gap);
    const auto flexibleWidth = flexible > 0 ? space / flexible : 0;
    for (const auto& item : items_) if (item.component != nullptr)
    {
        item.component->setBounds(bounds.removeFromLeft(juce::jmin(bounds.getWidth(), item.width == 0 ? flexibleWidth : item.width)));
        bounds.removeFromLeft(juce::jmin(bounds.getWidth(), tokens::gap));
    }
}
CollapsibleSection::CollapsibleSection(juce::String title) : header_(std::move(title), Icon::chevronDown)
{
    header_.setVariant(Button::Variant::subtle);
    header_.onClick = [this] { setExpanded(!expanded_, juce::sendNotificationSync); };
    addAndMakeVisible(header_);
}
void CollapsibleSection::setContent(std::unique_ptr<juce::Component> content)
{
    content_ = std::move(content);
    if (content_) { addChildComponent(*content_); content_->setVisible(expanded_); }
    resized();
}
void CollapsibleSection::setTitle(juce::String title) { Panel::setTitle(title); header_.setText(std::move(title)); }
void CollapsibleSection::setExpanded(bool expanded, juce::NotificationType type)
{
    if (expanded == expanded_) return;
    expanded_ = expanded;
    header_.setIcon(expanded ? Icon::chevronDown : Icon::chevronRight);
    header_.setDescription(expanded ? "Expanded" : "Collapsed");
    if (content_) content_->setVisible(expanded);
    resized(); repaint();
    if (type == juce::dontSendNotification) return;
    if (type == juce::sendNotificationAsync)
    {
        juce::Component::SafePointer<CollapsibleSection> safe(this);
        juce::MessageManager::callAsync([safe, expanded]
        { if (safe != nullptr) { auto callback = safe->onExpandedChange; if (callback) callback(expanded); } });
        return;
    }
    auto callback = onExpandedChange; if (callback) callback(expanded);
}
void CollapsibleSection::resized()
{
    auto bounds = getLocalBounds(); header_.setBounds(bounds.removeFromTop(tokens::controlHeight));
    if (content_) content_->setBounds(bounds.reduced(tokens::gap));
}
}
