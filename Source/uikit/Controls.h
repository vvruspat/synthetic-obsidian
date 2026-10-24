#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "Theme.h"

namespace obsidian::ui
{
class Menu;

// All controls and callbacks belong to the message thread. dontSendNotification is
// the default for model updates; async notifications use a weak component handle.
class Button : public juce::Component
{
public:
    enum class Variant { normal, subtle, accent };
    enum class Size { small, normal, large };
    using juce::Component::setSize;
    explicit Button(juce::String text = {}, Icon icon = Icon::none);
    void setText(juce::String);
    const juce::String& getText() const noexcept { return text_; }
    void setIcon(Icon);
    void setIconColour(juce::Colour);
    void setAccent(juce::Colour);
    void setTextJustification(juce::Justification);
    void setSelected(bool);
    bool isSelected() const noexcept { return selected_; }
    void setVariant(Variant);
    void setSize(Size);
    int getPreferredHeight() const noexcept;
    void triggerClick();
    std::function<void()> onClick;
    void paint(juce::Graphics&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    void enablementChanged() override;
protected:
    virtual void activated();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    juce::String text_;
    Icon icon_;
    juce::Colour accent_ { tokens::blue };
    std::optional<juce::Colour> iconColour_;
    juce::Justification textJustification_ { juce::Justification::centred };
    Variant variant_ = Variant::normal;
    Size size_ = Size::normal;
    bool selected_ = false, hovered_ = false, pressed_ = false;
};

class IconButton final : public Button
{
public:
    IconButton(Icon, juce::String accessibleName);
};

class ToggleButton final : public Button
{
public:
    explicit ToggleButton(juce::String text = {});
    void setToggleState(bool, juce::NotificationType = juce::dontSendNotification);
    bool getToggleState() const noexcept { return isSelected(); }
    std::function<void(bool)> onToggle;
protected:
    void activated() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
};

class Slider final : public juce::Component, private juce::Timer
{
public:
    Slider();
    ~Slider() override;
    void setRange(double min, double max, double step = 0.0);
    void setValue(double, juce::NotificationType = juce::dontSendNotification);
    // Discrete user/assistive changes emit a balanced begin/change/end gesture.
    void setValueFromUser(double);
    double getValue() const noexcept { return value_; }
    double getMinimum() const noexcept { return min_; }
    double getMaximum() const noexcept { return max_; }
    double getInterval() const noexcept { return step_; }
    void setAccent(juce::Colour);
    juce::String getValueText() const;
    bool isDragging() const noexcept { return dragging_; }
    void cancelDrag();
    std::function<void()> onDragStart, onDragEnd;
    std::function<void(double)> onValueChange;
    std::function<juce::String(double)> valueToText;
    void paint(juce::Graphics&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    void enablementChanged() override;
    void visibilityChanged() override;
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    void timerCallback() override;
    void updateFromX(float);
    double min_ = 0.0, max_ = 1.0, step_ = 0.0, value_ = 0.0;
    juce::Colour accent_ { tokens::blue };
    bool dragging_ = false, hovered_ = false;
};

class Select final : public juce::Component
{
public:
    Select();
    ~Select() override;
    void setItems(juce::StringArray);
    void setItemIcons(std::vector<Icon>, bool iconOnly = false);
    bool getIconOnly() const noexcept { return iconOnly_; }
    const juce::StringArray& getItems() const noexcept { return items_; }
    void setSelectedIndex(int, juce::NotificationType = juce::dontSendNotification);
    int getSelectedIndex() const noexcept { return selectedIndex_; }
    juce::String getSelectedText() const;
    void showMenu();
    std::function<void(int)> onChange;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(FocusChangeType) override;
    void focusLost(FocusChangeType) override;
    void enablementChanged() override;
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    juce::StringArray items_;
    std::vector<Icon> itemIcons_;
    bool iconOnly_ = false;
    std::unique_ptr<Menu> menu_;
    int selectedIndex_ = -1;
    bool hovered_ = false;
};

class LevelMeter final : public juce::Component
{
public:
    LevelMeter();
    void setLevels(float left, float right);
    float getLeftLevel() const noexcept { return left_; }
    float getRightLevel() const noexcept { return right_; }
    void paint(juce::Graphics&) override;
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    float left_ = 0.0f, right_ = 0.0f;
};
}
