#include "Menu.h"
#include "Controls.h"

#include <cmath>
#include <utility>

namespace obsidian::ui
{
namespace
{
class MenuRow final : public Button
{
public:
    MenuRow(juce::String text, bool checked, std::function<bool(const juce::KeyPress&)> keyHandler)
        : Button(std::move(text), checked ? Icon::check : Icon::none), keyHandler_(std::move(keyHandler))
    { setVariant(Variant::subtle); }
    bool keyPressed(const juce::KeyPress& key) override { return keyHandler_(key); }
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::menuItem,
            juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] { triggerClick(); }));
    }
private:
    std::function<bool(const juce::KeyPress&)> keyHandler_;
};
}

Popover::Popover() { setVisible(false); setWantsKeyboardFocus(true); setTitle("Popover"); }
Popover::~Popover() { if (isCurrentlyModal()) exitModalState(); }
void Popover::showFor(juce::Component& anchor, int width, int height)
{
    if (isCurrentlyModal()) exitModalState();
    anchor_ = &anchor;
    auto* root = anchor.getTopLevelComponent();
    if (root == &anchor && anchor.getParentComponent() == nullptr) return;
    root->addAndMakeVisible(*this);
    const auto anchorBounds = root->getLocalArea(&anchor, anchor.getLocalBounds());
    const auto available = root->getLocalBounds().reduced(4);
    width = juce::jlimit(1, juce::jmax(1, available.getWidth()), width);
    height = juce::jlimit(1, juce::jmax(1, available.getHeight()), height);
    const auto below = anchorBounds.getBottom() + 4;
    const auto y = below + height <= available.getBottom() ? below : anchorBounds.getY() - height - 4;
    setBounds(juce::Rectangle<int>(anchorBounds.getX(), y, width, height).constrainedWithin(available));
    toFront(false);
    enterModalState(true, nullptr, false);
}
void Popover::dismiss()
{
    if (!isOpen()) return;
    auto callback = onDismiss;
    auto anchor = anchor_;
    exitModalState(); setVisible(false);
    if (auto* parent = getParentComponent()) parent->removeChildComponent(this);
    if (anchor != nullptr && anchor->isShowing() && anchor->isEnabled()) anchor->grabKeyboardFocus();
    if (callback) callback();
}
void Popover::paint(juce::Graphics& g)
{
    g.setColour(colour(tokens::raised));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), tokens::radius);
    g.setColour(colour(tokens::faint));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), tokens::radius, tokens::line);
}
void Popover::inputAttemptWhenModal() { dismiss(); }
bool Popover::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::tabKey) { dismiss(); return true; }
    return false;
}
std::unique_ptr<juce::AccessibilityHandler> Popover::createAccessibilityHandler()
{ return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::dialogWindow); }

Menu::Menu() { setTitle("Menu"); }
Menu::~Menu() = default;
void Menu::setItems(std::vector<Item> items)
{
    rows_.clear(); items_ = std::move(items); highlighted_ = -1; firstVisibleRow_ = 0;
    juce::Component::SafePointer<Menu> safe(this);
    for (int index = 0; index < static_cast<int>(items_.size()); ++index)
    {
        const auto& item = items_[static_cast<size_t>(index)];
        auto row = std::make_unique<MenuRow>(item.text, item.checked,
            [safe](const juce::KeyPress& key) { return safe != nullptr && safe->keyPressed(key); });
        row->setEnabled(item.enabled);
        row->onClick = [safe, index] { if (safe != nullptr) safe->activate(index); };
        addAndMakeVisible(*row); rows_.push_back(std::move(row));
    }
    resized();
}
void Menu::showFor(juce::Component& anchor, int width, int selectedIndex)
{
    if (items_.empty()) return;
    Popover::showFor(anchor, width, static_cast<int>(items_.size()) * tokens::controlHeight + tokens::gap);
    highlight(juce::isPositiveAndBelow(selectedIndex, static_cast<int>(items_.size())) ? selectedIndex : 0);
}
void Menu::highlight(int index)
{
    const auto count = static_cast<int>(items_.size());
    if (!juce::isPositiveAndBelow(index, count)) return;
    for (int tries = 0; tries < count && !items_[static_cast<size_t>(index)].enabled; ++tries) index = (index + 1) % count;
    if (!items_[static_cast<size_t>(index)].enabled) return;
    highlighted_ = index;
    const auto visibleRows = juce::jmax(1, (getHeight() - tokens::gap) / tokens::controlHeight);
    if (index < firstVisibleRow_) firstVisibleRow_ = index;
    else if (index >= firstVisibleRow_ + visibleRows) firstVisibleRow_ = index - visibleRows + 1;
    for (int i = 0; i < count; ++i) static_cast<Button&>(*rows_[static_cast<size_t>(i)]).setSelected(i == index);
    resized();
    if (isShowing()) rows_[static_cast<size_t>(index)]->grabKeyboardFocus();
}
void Menu::activate(int index)
{
    if (!juce::isPositiveAndBelow(index, static_cast<int>(items_.size())) || !items_[static_cast<size_t>(index)].enabled) return;
    auto callback = items_[static_cast<size_t>(index)].action;
    dismiss();
    if (callback) callback();
}
bool Menu::keyPressed(const juce::KeyPress& key)
{
    if (Popover::keyPressed(key)) return true;
    const auto count = static_cast<int>(items_.size());
    if (count == 0) return false;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::downKey || code == juce::KeyPress::upKey)
    {
        const auto direction = code == juce::KeyPress::downKey ? 1 : -1;
        auto next = highlighted_;
        for (int tries = 0; tries < count; ++tries)
        {
            next = (next + direction + count) % count;
            if (items_[static_cast<size_t>(next)].enabled) { highlight(next); break; }
        }
    }
    else if (code == juce::KeyPress::homeKey) highlight(0);
    else if (code == juce::KeyPress::endKey)
    {
        for (int index = count - 1; index >= 0; --index)
            if (items_[static_cast<size_t>(index)].enabled) { highlight(index); break; }
    }
    else if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey) activate(highlighted_);
    else return false;
    return true;
}
void Menu::resized()
{
    const auto visibleRows = juce::jmax(1, (getHeight() - tokens::gap) / tokens::controlHeight);
    firstVisibleRow_ = juce::jlimit(0, juce::jmax(0, static_cast<int>(rows_.size()) - visibleRows), firstVisibleRow_);
    for (int index = 0; index < static_cast<int>(rows_.size()); ++index)
        rows_[static_cast<size_t>(index)]->setBounds(tokens::gap / 2, tokens::gap / 2 + (index - firstVisibleRow_) * tokens::controlHeight,
                                                  juce::jmax(0, getWidth() - tokens::gap), tokens::controlHeight);
}
void Menu::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const auto delta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;
    if (delta == 0.0f || !std::isfinite(delta)) return;
    const auto visibleRows = juce::jmax(1, (getHeight() - tokens::gap) / tokens::controlHeight);
    const auto lastOffset = juce::jmax(0, static_cast<int>(rows_.size()) - visibleRows);
    const auto distance = juce::jmax(1, static_cast<int>(std::ceil(juce::jmin(1.0f, std::abs(delta)) * 3.0f)));
    firstVisibleRow_ = juce::jlimit(0, lastOffset, firstVisibleRow_ + (delta > 0.0f ? -distance : distance));
    resized();
}
std::unique_ptr<juce::AccessibilityHandler> Menu::createAccessibilityHandler()
{ return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::popupMenu); }

Tooltip::Tooltip(juce::Component& target, juce::String text, int delayMs)
    : target_(&target), text_(std::move(text)), delayMs_(juce::jmax(0, delayMs))
{
    setVisible(false); setInterceptsMouseClicks(false, false); setTitle(text_);
    target.setHelpText(text_); startTimerHz(15);
}
Tooltip::~Tooltip() { stopTimer(); }
void Tooltip::setText(juce::String text)
{
    text_ = std::move(text); setTitle(text_);
    if (target_ != nullptr) target_->setHelpText(text_);
    repaint();
}
void Tooltip::timerCallback()
{
    if (target_ == nullptr) { stopTimer(); setVisible(false); return; }
    auto& target = *target_;
    const auto hovered = target.isShowing() && (target.isMouseOver(true) || target.hasKeyboardFocus(false));
    if (!hovered || text_.isEmpty()) { hoverStart_ = 0; setVisible(false); return; }
    const auto now = juce::Time::currentTimeMillis();
    if (hoverStart_ == 0) hoverStart_ = now;
    if (now - hoverStart_ < delayMs_) return;
    auto* root = target.getTopLevelComponent();
    if (root == &target) return;
    if (getParentComponent() != root) root->addChildComponent(this);
    const auto available = root->getLocalBounds().reduced(4);
    const auto width = juce::jmin(available.getWidth(), juce::jlimit(80, 360, juce::GlyphArrangement::getStringWidthInt(font(), text_) + 20));
    const auto anchor = root->getLocalArea(&target, target.getLocalBounds());
    setBounds(juce::Rectangle<int>(anchor.getX(), anchor.getBottom() + 5, width, tokens::controlHeight).constrainedWithin(available));
    setVisible(true); toFront(false);
}
void Tooltip::paint(juce::Graphics& g)
{
    g.setColour(colour(tokens::background)); g.fillRoundedRectangle(getLocalBounds().toFloat(), tokens::radius);
    g.setColour(colour(tokens::faint)); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), tokens::radius, tokens::line);
    g.setColour(colour(tokens::text)); g.setFont(font(tokens::fontSmall));
    g.drawFittedText(text_, getLocalBounds().reduced(tokens::gap, 2), juce::Justification::centredLeft, 1);
}
std::unique_ptr<juce::AccessibilityHandler> Tooltip::createAccessibilityHandler()
{ return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::tooltip); }
}
