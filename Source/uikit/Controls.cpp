#include "Controls.h"
#include "Menu.h"

#include <cmath>
#include <utility>

namespace obsidian::ui
{
namespace
{
template <typename Control, typename Callback>
void notify(Control& control, juce::NotificationType type, Callback callback)
{
    if (type == juce::dontSendNotification)
        return;
    if (type == juce::sendNotificationAsync)
    {
        juce::Component::SafePointer<Control> safe(&control);
        juce::MessageManager::callAsync([safe, pendingCallback = std::move(callback)]
        {
            if (safe != nullptr)
                pendingCallback(*safe);
        });
        return;
    }
    callback(control);
}

void valueChanged(juce::Component& component)
{
    if (auto* handler = component.getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

class StateHandler final : public juce::AccessibilityHandler
{
public:
    StateHandler(juce::Component& component, juce::AccessibilityRole role,
                 juce::AccessibilityActions actions, std::function<juce::AccessibleState(juce::AccessibleState)> state)
        : AccessibilityHandler(component, role, std::move(actions)), state_(std::move(state)) {}
    juce::AccessibleState getCurrentState() const override
    {
        return state_(AccessibilityHandler::getCurrentState());
    }
private:
    std::function<juce::AccessibleState(juce::AccessibleState)> state_;
};

class SliderValue final : public juce::AccessibilityValueInterface
{
public:
    explicit SliderValue(Slider& slider) : slider_(slider) {}
    bool isReadOnly() const override { return !slider_.isEnabled(); }
    double getCurrentValue() const override { return slider_.getValue(); }
    juce::String getCurrentValueAsString() const override { return slider_.getValueText(); }
    void setValue(double value) override
    {
        slider_.setValueFromUser(value);
    }
    void setValueAsString(const juce::String& value) override { setValue(value.getDoubleValue()); }
    AccessibleValueRange getRange() const override
    {
        return { { slider_.getMinimum(), slider_.getMaximum() }, slider_.getInterval() };
    }
private:
    Slider& slider_;
};

class MeterValue final : public juce::AccessibilityValueInterface
{
public:
    explicit MeterValue(LevelMeter& meter) : meter_(meter) {}
    bool isReadOnly() const override { return true; }
    double getCurrentValue() const override { return juce::jmax(meter_.getLeftLevel(), meter_.getRightLevel()); }
    juce::String getCurrentValueAsString() const override
    {
        return "Left " + juce::String(juce::roundToInt(meter_.getLeftLevel() * 100.0f)) + "%, right "
             + juce::String(juce::roundToInt(meter_.getRightLevel() * 100.0f)) + "%";
    }
    void setValue(double) override {}
    void setValueAsString(const juce::String&) override {}
    AccessibleValueRange getRange() const override { return { { 0.0, 1.0 }, 0.01 }; }
private:
    LevelMeter& meter_;
};

class SelectValue final : public juce::AccessibilityTextValueInterface
{
public:
    explicit SelectValue(Select& select) : select_(select) {}
    bool isReadOnly() const override { return !select_.isEnabled(); }
    juce::String getCurrentValueAsString() const override { return select_.getSelectedText(); }
    void setValueAsString(const juce::String& value) override
    {
        const auto index = select_.getItems().indexOf(value);
        if (select_.isEnabled() && index >= 0) select_.setSelectedIndex(index, juce::sendNotificationSync);
    }
private:
    Select& select_;
};
}

Button::Button(juce::String text, Icon icon) : text_(std::move(text)), icon_(icon)
{
    setTitle(text_);
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}
void Button::setText(juce::String text) { text_ = std::move(text); setTitle(text_); repaint(); }
void Button::setIcon(Icon icon) { icon_ = icon; repaint(); }
void Button::setIconColour(juce::Colour iconColour) { iconColour_ = iconColour; repaint(); }
void Button::setAccent(juce::Colour accent) { accent_ = accent; repaint(); }
void Button::setTextJustification(juce::Justification justification) { textJustification_ = justification; repaint(); }
void Button::setSelected(bool selected) { if (selected_ != selected) { selected_ = selected; repaint(); valueChanged(*this); } }
void Button::setVariant(Variant variant) { variant_ = variant; repaint(); }
void Button::setSize(Size size) { size_ = size; repaint(); }
int Button::getPreferredHeight() const noexcept
{
    return size_ == Size::small ? tokens::controlSmall : size_ == Size::large ? tokens::controlLarge : tokens::controlHeight;
}
void Button::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    auto fill = colour(variant_ == Variant::subtle ? tokens::surface : tokens::raised);
    if (selected_ || variant_ == Variant::accent) fill = accent_.withAlpha(0.24f);
    if (hovered_) fill = fill.brighter(0.12f);
    if (pressed_) fill = colour(tokens::pressed);
    const auto alpha = isEnabled() ? 1.0f : 0.4f;
    g.setColour(fill.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(bounds, tokens::radius);
    g.setColour(colour(tokens::border).withMultipliedAlpha(alpha));
    g.drawRoundedRectangle(bounds, tokens::radius, tokens::line);
    auto content = getLocalBounds().reduced(text_.length() <= 2 ? 3 : tokens::gap, 2);
    const auto foreground = (selected_ ? accent_ : colour(tokens::text)).withMultipliedAlpha(alpha);
    if (icon_ != Icon::none)
    {
        auto iconBounds = text_.isEmpty() ? content.withSizeKeepingCentre(16, 16) : content.removeFromLeft(18).withSizeKeepingCentre(16, 16);
        drawIcon(g, icon_, iconBounds.toFloat(), iconColour_.has_value() ? iconColour_->withMultipliedAlpha(alpha) : foreground);
        if (text_.isNotEmpty()) content.removeFromLeft(4);
    }
    if (text_.isNotEmpty())
    {
        g.setColour(foreground);
        g.setFont(font(size_ == Size::small ? tokens::fontSmall : tokens::fontBody));
        g.drawFittedText(text_, content, textJustification_, 1);
    }
    drawFocus(g, *this);
}
void Button::mouseEnter(const juce::MouseEvent&) { hovered_ = true; repaint(); }
void Button::mouseExit(const juce::MouseEvent&) { hovered_ = false; repaint(); }
void Button::mouseDown(const juce::MouseEvent& e)
{
    if (isEnabled() && e.mods.isLeftButtonDown()) { pressed_ = true; grabKeyboardFocus(); repaint(); }
}
void Button::mouseUp(const juce::MouseEvent& e)
{
    const auto activate = pressed_ && isEnabled() && getLocalBounds().contains(e.getPosition());
    pressed_ = false;
    repaint();
    if (activate) triggerClick();
}
bool Button::keyPressed(const juce::KeyPress& key)
{
    if (isEnabled() && (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey))
    { triggerClick(); return true; }
    return false;
}
void Button::focusGained(FocusChangeType) { repaint(); }
void Button::focusLost(FocusChangeType) { pressed_ = false; repaint(); }
void Button::enablementChanged() { if (!isEnabled()) pressed_ = false; repaint(); }
void Button::triggerClick() { if (isEnabled()) activated(); }
void Button::activated() { auto callback = onClick; if (callback) callback(); }
std::unique_ptr<juce::AccessibilityHandler> Button::createAccessibilityHandler()
{
    return std::make_unique<StateHandler>(*this, juce::AccessibilityRole::button,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] { triggerClick(); }),
        [this](juce::AccessibleState state) { return selected_ ? state.withSelectable().withSelected() : state.withSelectable(); });
}

IconButton::IconButton(Icon icon, juce::String accessibleName) : Button({}, icon) { setTitle(accessibleName); }
ToggleButton::ToggleButton(juce::String text) : Button(std::move(text)) {}
void ToggleButton::setToggleState(bool state, juce::NotificationType type)
{
    if (state == getToggleState()) return;
    setSelected(state);
    notify(*this, type, [state](ToggleButton& button) { auto callback = button.onToggle; if (callback) callback(state); });
}
void ToggleButton::activated()
{
    juce::Component::SafePointer<ToggleButton> safe(this);
    setToggleState(!getToggleState(), juce::sendNotificationSync);
    if (safe != nullptr) Button::activated();
}
std::unique_ptr<juce::AccessibilityHandler> ToggleButton::createAccessibilityHandler()
{
    return std::make_unique<StateHandler>(*this, juce::AccessibilityRole::toggleButton,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] { triggerClick(); })
            .addAction(juce::AccessibilityActionType::toggle, [this] { triggerClick(); }),
        [this](juce::AccessibleState state) { return getToggleState() ? state.withCheckable().withChecked() : state.withCheckable(); });
}

Slider::Slider() { setWantsKeyboardFocus(true); setTitle("Value"); }
Slider::~Slider() { stopTimer(); cancelDrag(); }
void Slider::setRange(double min, double max, double step)
{
    if (!std::isfinite(min) || !std::isfinite(max) || max <= min || !std::isfinite(max - min)) return;
    min_ = min; max_ = max; step_ = std::isfinite(step) ? juce::jmax(0.0, step) : 0.0;
    setValue(value_);
    repaint();
}
void Slider::setValue(double value, juce::NotificationType type)
{
    if (!std::isfinite(value)) return;
    value = juce::jlimit(min_, max_, value);
    if (step_ > 0.0 && value < max_) value = min_ + std::round((value - min_) / step_) * step_;
    value = juce::jlimit(min_, max_, value);
    if (juce::approximatelyEqual(value_, value)) return;
    value_ = value;
    repaint();
    valueChanged(*this);
    notify(*this, type, [value](Slider& slider) { auto callback = slider.onValueChange; if (callback) callback(value); });
}
void Slider::setAccent(juce::Colour accent) { accent_ = accent; repaint(); }
juce::String Slider::getValueText() const { return valueToText ? valueToText(value_) : juce::String(value_, 2); }
void Slider::paint(juce::Graphics& g)
{
    const auto y = static_cast<float>(getHeight()) * 0.5f;
    const auto start = 7.0f, end = juce::jmax(start, static_cast<float>(getWidth()) - 7.0f);
    const auto x = start + static_cast<float>((value_ - min_) / (max_ - min_)) * (end - start);
    const auto alpha = isEnabled() ? 1.0f : 0.4f;
    g.setColour(colour(tokens::border).withMultipliedAlpha(alpha));
    g.drawLine(start, y, end, y, 4.0f);
    g.setColour(accent_.withMultipliedAlpha(alpha));
    g.drawLine(start, y, x, y, 4.0f);
    g.setColour((dragging_ ? accent_ : colour(tokens::text)).withMultipliedAlpha(alpha));
    const auto radius = hovered_ || dragging_ ? 6.0f : 5.0f;
    g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
    drawFocus(g, *this);
}
void Slider::updateFromX(float x)
{
    const auto width = juce::jmax(1.0, static_cast<double>(getWidth() - 14));
    setValue(min_ + juce::jlimit(0.0, 1.0, (static_cast<double>(x) - 7.0) / width) * (max_ - min_), juce::sendNotificationSync);
}
void Slider::mouseEnter(const juce::MouseEvent&) { hovered_ = true; repaint(); }
void Slider::mouseExit(const juce::MouseEvent&) { hovered_ = false; repaint(); }
void Slider::mouseDown(const juce::MouseEvent& e)
{
    if (!isEnabled() || !e.mods.isLeftButtonDown()) return;
    grabKeyboardFocus(); dragging_ = true; startTimerHz(30);
    juce::Component::SafePointer<Slider> safe(this);
    auto callback = onDragStart; if (callback) callback();
    if (safe != nullptr && dragging_) updateFromX(e.position.x);
}
void Slider::mouseDrag(const juce::MouseEvent& e) { if (dragging_ && isEnabled()) updateFromX(e.position.x); }
void Slider::mouseUp(const juce::MouseEvent&) { cancelDrag(); }
void Slider::cancelDrag()
{
    if (!dragging_) return;
    dragging_ = false; stopTimer(); repaint();
    auto callback = onDragEnd; if (callback) callback();
}
void Slider::timerCallback()
{
    // Hosts can steal mouse capture without delivering mouseUp. Balance the gesture.
    if (!juce::ModifierKeys::getCurrentModifiersRealtime().isLeftButtonDown() || !isShowing()) cancelDrag();
}
void Slider::setValueFromUser(double value)
{
    if (!isEnabled() || !std::isfinite(value)) return;
    if (dragging_)
    {
        setValue(value, juce::sendNotificationSync);
        return;
    }
    dragging_ = true;
    juce::Component::SafePointer<Slider> safe(this);
    auto start = onDragStart; if (start) start();
    if (safe == nullptr || !dragging_) return;
    setValue(value, juce::sendNotificationSync);
    if (safe != nullptr) cancelDrag();
}
bool Slider::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled()) return false;
    const auto increment = step_ > 0.0 ? step_ : (max_ - min_) / 100.0;
    const auto amount = increment * (key.getModifiers().isShiftDown() ? 10.0 : 1.0);
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey) setValueFromUser(value_ - amount);
    else if (code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey) setValueFromUser(value_ + amount);
    else if (code == juce::KeyPress::homeKey) setValueFromUser(min_);
    else if (code == juce::KeyPress::endKey) setValueFromUser(max_);
    else if (code == juce::KeyPress::escapeKey && dragging_) cancelDrag();
    else return false;
    return true;
}
void Slider::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (!isEnabled() || !hasKeyboardFocus(false)) return;
    const auto delta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;
    if (delta != 0.0f) setValueFromUser(value_ + (delta > 0 ? 1 : -1) * (step_ > 0 ? step_ : (max_ - min_) / 100.0));
}
void Slider::focusGained(FocusChangeType) { repaint(); }
void Slider::focusLost(FocusChangeType) { repaint(); cancelDrag(); }
void Slider::enablementChanged() { repaint(); if (!isEnabled()) cancelDrag(); }
void Slider::visibilityChanged() { if (!isShowing()) cancelDrag(); }
std::unique_ptr<juce::AccessibilityHandler> Slider::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::slider,
        juce::AccessibilityActions{}, juce::AccessibilityHandler::Interfaces(std::make_unique<SliderValue>(*this)));
}

Select::Select() { setWantsKeyboardFocus(true); setTitle("Select"); }
Select::~Select() = default;
void Select::setItems(juce::StringArray items)
{
    if (menu_) menu_->dismiss();
    items_ = std::move(items);
    selectedIndex_ = items_.isEmpty() ? -1 : juce::jlimit(0, items_.size() - 1, selectedIndex_);
    repaint(); valueChanged(*this);
}
juce::String Select::getSelectedText() const { return juce::isPositiveAndBelow(selectedIndex_, items_.size()) ? items_[selectedIndex_] : juce::String{}; }
void Select::setSelectedIndex(int index, juce::NotificationType type)
{
    index = items_.isEmpty() ? -1 : juce::jlimit(-1, items_.size() - 1, index);
    if (index == selectedIndex_) return;
    selectedIndex_ = index; repaint(); valueChanged(*this);
    notify(*this, type, [index](Select& select) { auto callback = select.onChange; if (callback) callback(index); });
}
void Select::paint(juce::Graphics& g)
{
    const auto alpha = isEnabled() ? 1.0f : 0.4f;
    g.setColour(colour(hovered_ ? tokens::hover : tokens::raised).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), tokens::radius);
    g.setColour(colour(tokens::border).withMultipliedAlpha(alpha));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), tokens::radius, tokens::line);
    auto bounds = getLocalBounds().reduced(tokens::gap, 0);
    drawIcon(g, Icon::chevronDown, bounds.removeFromRight(12).toFloat().withSizeKeepingCentre(10.0f, 10.0f), colour(tokens::muted).withMultipliedAlpha(alpha));
    bounds.removeFromRight(tokens::gap);
    g.setColour(colour(tokens::text).withMultipliedAlpha(alpha)); g.setFont(font());
    g.drawFittedText(getSelectedText(), bounds, juce::Justification::centredLeft, 1);
    drawFocus(g, *this);
}
void Select::showMenu()
{
    if (!isEnabled() || items_.isEmpty()) return;
    if (menu_ && menu_->isOpen()) { menu_->dismiss(); return; }
    if (!menu_) menu_ = std::make_unique<Menu>();
    std::vector<Menu::Item> rows;
    juce::Component::SafePointer<Select> safe(this);
    for (int i = 0; i < items_.size(); ++i)
        rows.push_back({ items_[i], [safe, i] { if (safe != nullptr) safe->setSelectedIndex(i, juce::sendNotificationSync); }, true, i == selectedIndex_ });
    menu_->setItems(std::move(rows));
    menu_->showFor(*this, juce::jmax(160, getWidth()), selectedIndex_);
}
void Select::mouseDown(const juce::MouseEvent& e) { if (e.mods.isLeftButtonDown() && isEnabled()) { grabKeyboardFocus(); showMenu(); } }
void Select::mouseEnter(const juce::MouseEvent&) { hovered_ = true; repaint(); }
void Select::mouseExit(const juce::MouseEvent&) { hovered_ = false; repaint(); }
bool Select::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled() || items_.isEmpty()) return false;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey) showMenu();
    else if (code == juce::KeyPress::homeKey) setSelectedIndex(0, juce::sendNotificationSync);
    else if (code == juce::KeyPress::endKey) setSelectedIndex(items_.size() - 1, juce::sendNotificationSync);
    else if (code == juce::KeyPress::upKey || code == juce::KeyPress::leftKey) setSelectedIndex(juce::jmax(0, selectedIndex_ - 1), juce::sendNotificationSync);
    else if (code == juce::KeyPress::downKey || code == juce::KeyPress::rightKey) setSelectedIndex(juce::jmin(items_.size() - 1, selectedIndex_ + 1), juce::sendNotificationSync);
    else return false;
    return true;
}
void Select::focusGained(FocusChangeType) { repaint(); }
void Select::focusLost(FocusChangeType) { repaint(); }
void Select::enablementChanged() { if (!isEnabled() && menu_) menu_->dismiss(); repaint(); }
std::unique_ptr<juce::AccessibilityHandler> Select::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::comboBox,
        juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] { showMenu(); })
            .addAction(juce::AccessibilityActionType::showMenu, [this] { showMenu(); }),
        juce::AccessibilityHandler::Interfaces(std::make_unique<SelectValue>(*this)));
}

LevelMeter::LevelMeter() { setTitle("Stereo level"); }
void LevelMeter::setLevels(float left, float right)
{
    left_ = std::isfinite(left) ? juce::jlimit(0.0f, 1.0f, left) : 0.0f;
    right_ = std::isfinite(right) ? juce::jlimit(0.0f, 1.0f, right) : 0.0f;
    repaint();
}
void LevelMeter::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(1);
    const auto rowHeight = juce::jmax(2, (bounds.getHeight() - 4) / 2);
    auto channel = 0;
    for (auto level : { left_, right_ })
    {
        auto row = bounds.removeFromTop(rowHeight); bounds.removeFromTop(4);
        const auto label = row.removeFromLeft(12);
        g.setColour(colour(tokens::muted)); g.setFont(font(tokens::fontSmall));
        g.drawText(channel++ == 0 ? "L" : "R", label, juce::Justification::centredLeft);
        const auto bar = row.toFloat();
        g.setColour(colour(tokens::border)); g.fillRoundedRectangle(bar, 2.0f);
        auto active = bar.withWidth(bar.getWidth() * level);
        g.setColour(colour(level > 0.95f ? tokens::pink : tokens::olive)); g.fillRoundedRectangle(active, 2.0f);
    }
}
std::unique_ptr<juce::AccessibilityHandler> LevelMeter::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::progressBar,
        juce::AccessibilityActions{}, juce::AccessibilityHandler::Interfaces(std::make_unique<MeterValue>(*this)));
}
}
