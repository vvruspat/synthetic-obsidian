#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace obsidian::demo
{
class Demo final : public juce::Component
{
public:
    struct State
    {
        double playhead = 0.0, firstBeat = 0.0, pixelsPerBeat = 0.0;
        int tracks = 0, notes = 0, selectedTrack = -1, selectedNote = -1;
        bool playing = false, backingExpanded = true;
    };
    Demo();
    ~Demo() override;
    State state() const;
    void reset();
    std::function<void()> onShowGallery;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
private:
    class Content;
    std::unique_ptr<Content> content_;
};
}
