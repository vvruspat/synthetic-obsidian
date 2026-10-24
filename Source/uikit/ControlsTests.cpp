#include "Controls.h"
#include "Layout.h"
#include "Menu.h"

#include <limits>

namespace obsidian::ui
{
namespace
{
juce::MouseEvent mouseEvent(juce::Component& component, float x, bool leftDown)
{
    const juce::Point<float> position(x, 12.0f);
    const auto time = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), position,
             juce::ModifierKeys(leftDown ? juce::ModifierKeys::leftButtonModifier : 0),
             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, time, position, time, 1, false };
}

class ControlsTests final : public juce::UnitTest
{
public:
    ControlsTests() : UnitTest("Custom controls", "Obsidian UI") {}
    void runTest() override
    {
        juce::Component testRoot;
        testRoot.setBounds(-10000, -10000, 320, 180);
        testRoot.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        testRoot.setVisible(true);
        beginTest("Slider quantisation and silent model updates");
        Slider slider;
        testRoot.addAndMakeVisible(slider);
        slider.setSize(200, 24);
        slider.setRange(-12.0, 12.0, 0.5);
        auto changes = 0, starts = 0, ends = 0;
        slider.onValueChange = [&](double) { ++changes; };
        slider.onDragStart = [&] { ++starts; };
        slider.onDragEnd = [&] { ++ends; };
        slider.setValue(1.24);
        expectEquals(slider.getValue(), 1.0);
        expectEquals(changes, 0);
        slider.setValue(1.24, juce::sendNotificationSync);
        expectEquals(changes, 0);
        slider.setValue(2.26, juce::sendNotificationSync);
        expectEquals(slider.getValue(), 2.5);
        expectEquals(changes, 1);
        slider.setValue(std::numeric_limits<double>::quiet_NaN());
        expectEquals(slider.getValue(), 2.5);
        slider.setRange(3.0, 3.0);
        expectEquals(slider.getMinimum(), -12.0);

        beginTest("Keyboard gestures and disabled controls");
        expect(slider.keyPressed(juce::KeyPress(juce::KeyPress::endKey)));
        expectEquals(slider.getValue(), 12.0);
        expectEquals(starts, 1); expectEquals(ends, 1);
        slider.keyPressed(juce::KeyPress(juce::KeyPress::leftKey));
        expectEquals(slider.getValue(), 11.5);
        slider.setEnabled(false);
        expect(!slider.keyPressed(juce::KeyPress(juce::KeyPress::homeKey)));
        expectEquals(slider.getValue(), 11.5);
        slider.setEnabled(true);

        beginTest("Mouse gesture cancellation balances exactly once");
        starts = ends = 0;
        slider.mouseDown(mouseEvent(slider, 40.0f, true));
        expect(slider.isDragging());
        slider.setEnabled(false);
        expect(!slider.isDragging());
        slider.mouseUp(mouseEvent(slider, 40.0f, false));
        expectEquals(starts, 1); expectEquals(ends, 1);
        slider.setEnabled(true);
        slider.mouseDown(mouseEvent(slider, 70.0f, true));
        slider.focusLost(juce::Component::focusChangedDirectly);
        slider.mouseUp(mouseEvent(slider, 70.0f, false));
        expectEquals(starts, 2); expectEquals(ends, 2);
        slider.mouseDown(mouseEvent(slider, 100.0f, true));
        expect(slider.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        expectEquals(starts, 3); expectEquals(ends, 3);
        slider.mouseDown(mouseEvent(slider, 100.0f, true));
        slider.setVisible(true); slider.setVisible(false);
        expectEquals(starts, 4); expectEquals(ends, 4);

        beginTest("Callbacks may destroy their control");
        auto disposable = std::make_unique<Slider>();
        disposable->setSize(120, 24);
        testRoot.addAndMakeVisible(*disposable);
        disposable->onDragStart = [&] { disposable.reset(); };
        disposable->mouseDown(mouseEvent(*disposable, 50.0f, true));
        expect(disposable == nullptr);
        auto button = std::make_unique<ToggleButton>("Mute");
        button->onToggle = [&](bool) { button.reset(); };
        button->triggerClick();
        expect(button == nullptr);

        beginTest("Button keyboard and toggle notifications");
        Button sizedButton("Sized");
        sizedButton.setSize(120, 30);
        sizedButton.setSize(Button::Size::small);
        expectEquals(sizedButton.getWidth(), 120);
        expectEquals(sizedButton.getHeight(), 30);
        expectEquals(sizedButton.getPreferredHeight(), tokens::controlSmall);
        ToggleButton toggle("Solo");
        testRoot.addAndMakeVisible(toggle);
        auto toggles = 0, clicks = 0;
        toggle.onToggle = [&](bool) { ++toggles; };
        toggle.onClick = [&] { ++clicks; };
        toggle.setToggleState(true);
        expectEquals(toggles, 0);
        toggle.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
        expect(!toggle.getToggleState());
        expectEquals(toggles, 1); expectEquals(clicks, 1);
        toggle.setEnabled(false);
        toggle.triggerClick();
        expectEquals(clicks, 1);

        beginTest("Select keeps model updates silent and handles keyboard bounds");
        Select select;
        testRoot.addAndMakeVisible(select);
        auto selections = 0;
        select.onChange = [&](int) { ++selections; };
        select.setItems({ "Voice", "Backing", "Instrumental" });
        select.setItemIcons({ Icon::microphone, Icon::backing, Icon::waveform }, true);
        select.setSelectedIndex(1);
        expect(select.getIconOnly());
        expectEquals(select.getSelectedText(), juce::String("Backing"));
        expectEquals(selections, 0);
        select.keyPressed(juce::KeyPress(juce::KeyPress::endKey));
        expectEquals(select.getSelectedIndex(), 2); expectEquals(selections, 1);
        select.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        expectEquals(selections, 1);
        select.keyPressed(juce::KeyPress(juce::KeyPress::homeKey));
        expectEquals(select.getSelectedIndex(), 0);
        select.setItems({});
        expectEquals(select.getSelectedIndex(), -1);
        expect(!select.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));

        beginTest("Accessibility roles, values and keyboard focus");
        slider.setVisible(true);
        toggle.setEnabled(true);
        expect(slider.getWantsKeyboardFocus());
        expect(toggle.getWantsKeyboardFocus());
        expect(select.getWantsKeyboardFocus());
        auto* sliderHandler = slider.getAccessibilityHandler();
        expect(sliderHandler != nullptr);
        if (sliderHandler != nullptr)
        {
            expect(sliderHandler->getRole() == juce::AccessibilityRole::slider);
            auto* value = sliderHandler->getValueInterface();
            expect(value != nullptr);
            if (value != nullptr)
            {
                expectEquals(value->getCurrentValue(), slider.getValue());
                expectEquals(value->getRange().getMinimumValue(), -12.0);
            }
        }
        auto* toggleHandler = toggle.getAccessibilityHandler();
        expect(toggleHandler != nullptr);
        if (toggleHandler != nullptr)
        {
            expect(toggleHandler->getRole() == juce::AccessibilityRole::toggleButton);
            expect(toggleHandler->getCurrentState().isCheckable());
        }
        auto* selectHandler = select.getAccessibilityHandler();
        expect(selectHandler != nullptr);
        if (selectHandler != nullptr)
            expect(selectHandler->getRole() == juce::AccessibilityRole::comboBox);

        beginTest("Stereo meter exposes a read-only accessible value");
        LevelMeter meter;
        testRoot.addAndMakeVisible(meter);
        meter.setLevels(0.25f, 0.75f);
        auto* meterHandler = meter.getAccessibilityHandler();
        expect(meterHandler != nullptr);
        if (meterHandler != nullptr)
        {
            expect(meterHandler->getRole() == juce::AccessibilityRole::progressBar);
            auto* value = meterHandler->getValueInterface();
            expect(value != nullptr);
            if (value != nullptr)
            {
                expect(value->isReadOnly());
                expectEquals(value->getCurrentValue(), 0.75);
                expectEquals(value->getCurrentValueAsString(), juce::String("Left 25%, right 75%"));
                value->setValue(0.1);
                expectEquals(value->getCurrentValue(), 0.75);
            }
        }
        meter.setLevels(std::numeric_limits<float>::quiet_NaN(), 1.5f);
        expectEquals(meter.getLeftLevel(), 0.0f);
        expectEquals(meter.getRightLevel(), 1.0f);

        beginTest("Accessibility updates emit ordered gestures and tolerate deletion");
        juce::StringArray events;
        Slider accessibleSlider;
        testRoot.addAndMakeVisible(accessibleSlider);
        accessibleSlider.onDragStart = [&] { events.add("begin"); };
        accessibleSlider.onValueChange = [&](double) { events.add("change"); };
        accessibleSlider.onDragEnd = [&] { events.add("end"); };
        auto* accessibleHandler = accessibleSlider.getAccessibilityHandler();
        expect(accessibleHandler != nullptr);
        auto* accessibleValue = accessibleHandler != nullptr ? accessibleHandler->getValueInterface() : nullptr;
        expect(accessibleValue != nullptr);
        if (accessibleValue != nullptr) accessibleValue->setValue(0.75);
        expect(events == juce::StringArray({ "begin", "change", "end" }));
        expect(!accessibleSlider.isDragging());
        events.clear();
        accessibleSlider.setEnabled(false);
        if (accessibleValue != nullptr) accessibleValue->setValue(0.25);
        expect(events.isEmpty());
        for (const auto deleteAtStart : { true, false })
        {
            events.clear();
            auto deletingSlider = std::make_unique<Slider>();
            testRoot.addAndMakeVisible(*deletingSlider);
            deletingSlider->onDragStart = [&]
            {
                events.add("begin");
                if (deleteAtStart) deletingSlider.reset();
            };
            deletingSlider->onValueChange = [&](double)
            {
                events.add("change");
                deletingSlider.reset();
            };
            deletingSlider->onDragEnd = [&] { events.add("end"); };
            auto* deletingHandler = deletingSlider->getAccessibilityHandler();
            expect(deletingHandler != nullptr);
            auto* deletingValue = deletingHandler != nullptr ? deletingHandler->getValueInterface() : nullptr;
            expect(deletingValue != nullptr);
            if (deletingValue != nullptr) deletingValue->setValue(0.5);
            expect(deletingSlider == nullptr);
            expect(events == (deleteAtStart ? juce::StringArray({ "begin", "end" })
                                            : juce::StringArray({ "begin", "change", "end" })));
        }

        beginTest("Menus scroll overflow, navigate enabled rows and dismiss safely");
        Button anchor("Menu");
        anchor.setBounds(8, 8, 100, 30);
        testRoot.addAndMakeVisible(anchor);
        Menu menu;
        std::vector<Menu::Item> menuItems;
        auto actionIndex = -1, dismissals = 0;
        for (int index = 0; index < 12; ++index)
            menuItems.push_back({ "Action " + juce::String(index), [&, index] { actionIndex = index; }, index != 1, false });
        menu.setItems(std::move(menuItems));
        menu.onDismiss = [&] { ++dismissals; };
        menu.showFor(anchor);
        expect(menu.isOpen());
        expect(menu.getChildComponent(0)->hasKeyboardFocus(false));
        juce::MouseWheelDetails wheel;
        wheel.deltaY = -0.5f;
        menu.mouseWheelMove(mouseEvent(menu, 20.0f, false), wheel);
        expect(menu.getChildComponent(0)->getY() < 0);
        menu.keyPressed(juce::KeyPress(juce::KeyPress::homeKey));
        expect(menu.getChildComponent(0)->getY() >= 0);
        menu.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        menu.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        expectEquals(actionIndex, 2);
        expect(!menu.isOpen()); expectEquals(dismissals, 1);
        expect(anchor.hasKeyboardFocus(false));
        menu.showFor(anchor);
        menu.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        expect(!menu.isOpen()); expectEquals(dismissals, 2);
        menu.showFor(anchor);
        menu.inputAttemptWhenModal();
        expect(!menu.isOpen()); expectEquals(dismissals, 3);
        menu.showFor(anchor);
        menu.keyPressed(juce::KeyPress(juce::KeyPress::endKey));
        expect(menu.getChildComponent(11)->getBottom() <= menu.getHeight());
        menu.dismiss();

        beginTest("Collapsible section owns and hides its content");
        CollapsibleSection section("Global tracks");
        section.setContent(std::make_unique<juce::Component>());
        auto expandedChanges = 0;
        section.onExpandedChange = [&](bool) { ++expandedChanges; };
        section.setExpanded(false);
        expect(!section.getContent()->isVisible()); expectEquals(expandedChanges, 0);
        section.setExpanded(true, juce::sendNotificationSync);
        expect(section.getContent()->isVisible()); expectEquals(expandedChanges, 1);
    }
};
ControlsTests controlsTests;
}

void runAsyncDeletionChecks(std::function<void(bool)> completion)
{
    struct State
    {
        bool deletedCallbackRan = false;
        bool liveCallbackRan = false;
        std::unique_ptr<Slider> live = std::make_unique<Slider>();
    };
    auto state = std::make_shared<State>();
    const std::weak_ptr<State> weak = state;
    auto deleted = [weak]
    {
        if (const auto locked = weak.lock()) locked->deletedCallbackRan = true;
    };
    {
        Slider slider;
        slider.onValueChange = [deleted](double) { deleted(); };
        slider.setValue(0.5, juce::sendNotificationAsync);
        ToggleButton toggle;
        toggle.onToggle = [deleted](bool) { deleted(); };
        toggle.setToggleState(true, juce::sendNotificationAsync);
        Select select;
        select.setItems({ "A", "B" });
        select.onChange = [deleted](int) { deleted(); };
        select.setSelectedIndex(1, juce::sendNotificationAsync);
        CollapsibleSection section;
        section.onExpandedChange = [deleted](bool) { deleted(); };
        section.setExpanded(false, juce::sendNotificationAsync);
    }
    state->live->onValueChange = [weak](double value)
    {
        if (const auto locked = weak.lock()) locked->liveCallbackRan = juce::approximatelyEqual(value, 0.5);
    };
    state->live->setValue(0.5, juce::sendNotificationAsync);
    juce::MessageManager::callAsync([state, finished = std::move(completion)]
    {
        finished(!state->deletedCallbackRan && state->liveCallbackRan);
    });
}
}
