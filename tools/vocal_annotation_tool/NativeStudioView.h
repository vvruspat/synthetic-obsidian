#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace vocal_annotation
{
class StudioShortcuts;
// Native presentation of the existing studio command/event protocol. All calls
// belong to the message thread; this view does not own playback or analysis.
class NativeStudioView final : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    using CommandHandler = std::function<void(const juce::var&)>;
    explicit NativeStudioView(CommandHandler, std::shared_ptr<StudioShortcuts> = {});
    ~NativeStudioView() override;
    void dispatch(const juce::var& event);
    std::function<void()> onShowSettings, onShowVoiceLab;
    struct Snapshot
    {
        int trackCount = 0, clipCount = 0, noteCount = 0;
        juce::String selectedTrack, selectedNoteId;
        double playheadSeconds = 0.0, pixelsPerBeat = 0.0, verticalZoom = 1.0;
        bool pianoCollapsed = false, loopActive = false, pitchReadOnly = false;
        double loopStart = 0.0, loopEnd = 0.0;
        int selectedTrackCount = 0;
        int lowestMidi = 0, highestMidi = 127, pianoScrollY = 0;
    };
    Snapshot state() const;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int x, int y) override;
private:
    class Content;
    std::unique_ptr<Content> content_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NativeStudioView)
};
}
