#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace vocal_annotation
{
class StudioShortcuts;
class StudioShortcutEditor;
// Native presentation of the existing studio command/event protocol. No engine
// or training work runs here; all calls are delivered on the message thread.
class NativeStudioSettings final : public juce::Component
{
public:
    explicit NativeStudioSettings(std::function<void(const juce::var&)> commandHandler,
                                  std::shared_ptr<StudioShortcuts> shortcuts = {});
    ~NativeStudioSettings() override;
    void dispatch(const juce::var& event);
    void refresh();
    void showVoiceLab();
    // Selects a real track ID silently; retains the request until its snapshot arrives.
    void setBackingParent(const juce::String& trackId);
    std::function<void()> onClose;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void visibilityChanged() override;
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
private:
    class Content;
    std::shared_ptr<StudioShortcuts> shortcuts_;
    std::unique_ptr<Content> content_;
    juce::Viewport viewport_;
    std::unique_ptr<StudioShortcutEditor> shortcutEditor_;
    std::unique_ptr<juce::Component> mainTab_, shortcutsTab_;
    std::unique_ptr<juce::Component> close_;
    std::function<void(const juce::var&)> commandHandler_;
};
}
