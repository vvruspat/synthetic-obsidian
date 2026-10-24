#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <memory>
#include <vector>

namespace vocal_annotation
{
class StudioShortcuts final
{
public:
    struct Definition { juce::String id, category, label; juce::KeyPress defaultKey; };
    explicit StudioShortcuts(juce::File preferences = {});
    static const std::vector<Definition>& definitions();
    juce::String actionFor(const juce::KeyPress&) const;
    juce::KeyPress getKey(const juce::String& action) const;
    void assign(const juce::String& action, const juce::KeyPress&);
    void restoreDefaults();
    const juce::String& error() const noexcept { return error_; }
private:
    void save();
    juce::File file_;
    std::map<juce::String, juce::KeyPress> bindings_;
    juce::String error_;
};

class StudioShortcutEditor final : public juce::Component
{
public:
    explicit StudioShortcutEditor(std::shared_ptr<StudioShortcuts>);
    ~StudioShortcutEditor() override;
    void resized() override;
    void paint(juce::Graphics&) override;
private:
    class Content;
    juce::Viewport viewport_;
    std::unique_ptr<Content> content_;
};
}
