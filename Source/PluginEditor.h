#pragma once
#include <JuceHeader.h>
#include "ui/SyntheticObsidianLookAndFeel.h"
#include "ui/TrackManagerPanel.h"
#include "ui/PianoRollEditor.h"
#include "ui/TransportBar.h"

class SyntheticObsidianProcessor;

class SyntheticObsidianEditor final : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit SyntheticObsidianEditor (SyntheticObsidianProcessor&);
    ~SyntheticObsidianEditor() override;

    void paint   (juce::Graphics&) override;
    void resized ()                override;

private:
    void timerCallback () override;  // 30 Hz: playhead + CPU load

    SyntheticObsidianProcessor& processor_;

    SyntheticObsidianLookAndFeel lookAndFeel_;
    TrackManagerPanel            trackManager_;
    PianoRollEditor              pianoRoll_;
    TransportBar                 transportBar_;

    std::unique_ptr<juce::FileChooser> fileChooser_;

    /** UUID of the track whose notes are currently shown in the piano roll. */
    juce::Uuid selectedTrackId_;

    static constexpr int kWindowWidth     = 1200;
    static constexpr int kWindowHeight    = 800;
    static constexpr int kHeaderHeight    = 50;
    static constexpr int kTrackPanelWidth = 260;
    static constexpr int kTransportH      = 50;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyntheticObsidianEditor)
};
