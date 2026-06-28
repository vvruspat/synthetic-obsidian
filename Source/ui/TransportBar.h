#pragma once
#include <JuceHeader.h>
#include "SyntheticObsidianLookAndFeel.h"

class TransportBar final : public juce::Component
{
public:
    TransportBar();

    void paint   (juce::Graphics&) override;
    void resized ()                override;

    void setCpuLoad (float loadZeroToOne);
    void setBpm     (double bpm);

    std::function<void()>       onPlay;
    std::function<void()>       onStop;
    std::function<void()>       onReset;
    std::function<void()>       onRenderVocal;
    std::function<void()>       onRenderBacks;
    std::function<void(double)> onBpmChanged;

private:
    juce::TextButton resetBtn_       { "RESET" };
    juce::TextButton playBtn_        { "PLAY" };
    juce::TextButton stopBtn_        { "STOP" };
    juce::TextButton renderVocalBtn_ { "RENDER VOCAL" };
    juce::TextButton renderBacksBtn_ { "AI BACKS" };
    juce::Label      cpuLoadLabel_ { {}, "CPU LOAD" };
    juce::Label      bpmLabel_    { {}, "120" };

    std::atomic<float>  cpuLoad_ { 0.0f };
    std::atomic<double> bpm_     { 120.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};
