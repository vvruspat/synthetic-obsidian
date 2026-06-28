#pragma once
#include <JuceHeader.h>

class SyntheticObsidianProcessor;

// Bottom panel with Pitch Drift, Formant Shift, AI Influence, Vibrato Scale
class BottomParameterPanel final : public juce::Component
{
public:
    explicit BottomParameterPanel(SyntheticObsidianProcessor& processor);
    ~BottomParameterPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Knob + label pairs
    juce::Slider pitchDriftKnob_    { juce::Slider::Rotary, juce::Slider::NoTextBox };
    juce::Slider formantShiftSlider_{ juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider aiInfluenceKnob_   { juce::Slider::Rotary, juce::Slider::NoTextBox };
    juce::Slider vibratoScaleSlider_{ juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    juce::Label pitchDriftLabel_     { {}, "PITCH DRIFT" };
    juce::Label formantShiftLabel_   { {}, "FORMANT SHIFT" };
    juce::Label aiInfluenceLabel_    { {}, "AI INFLUENCE" };
    juce::Label vibratoScaleLabel_   { {}, "VIBRATO SCALE" };

    // Value display labels
    juce::Label pitchDriftValue_     { {}, "0.0" };
    juce::Label formantShiftValue_   { {}, "0.0" };
    juce::Label aiInfluenceValue_    { {}, "0.0" };
    juce::Label vibratoScaleValue_   { {}, "0.0" };

    // APVTS attachments — keep alive as long as sliders exist
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchDriftAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> formantShiftAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> aiInfluenceAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> vibratoScaleAttach_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BottomParameterPanel)
};
