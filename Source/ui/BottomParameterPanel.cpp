#include "BottomParameterPanel.h"
#include "../PluginProcessor.h"
#include "SyntheticObsidianLookAndFeel.h"

BottomParameterPanel::BottomParameterPanel(SyntheticObsidianProcessor& processor)
{
    auto& apvts = processor.getAPVTS();

    // Setup pitch drift knob
    pitchDriftKnob_.setRange(-12.0, 12.0, 0.1);
    pitchDriftKnob_.setValue(0.0);
    addAndMakeVisible(pitchDriftKnob_);
    pitchDriftAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "pitch_drift", pitchDriftKnob_);

    pitchDriftLabel_.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
    pitchDriftLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(pitchDriftLabel_);

    pitchDriftValue_.setFont(juce::FontOptions(10.0f));
    pitchDriftValue_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(pitchDriftValue_);

    pitchDriftKnob_.onValueChange = [this]()
    {
        pitchDriftValue_.setText(juce::String(pitchDriftKnob_.getValue(), 1), juce::dontSendNotification);
    };

    // Setup formant shift slider
    formantShiftSlider_.setRange(-24.0, 24.0, 0.1);
    formantShiftSlider_.setValue(0.0);
    addAndMakeVisible(formantShiftSlider_);
    formantShiftAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "formant_shift", formantShiftSlider_);

    formantShiftLabel_.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
    formantShiftLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(formantShiftLabel_);

    formantShiftValue_.setFont(juce::FontOptions(10.0f));
    formantShiftValue_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(formantShiftValue_);

    formantShiftSlider_.onValueChange = [this]()
    {
        formantShiftValue_.setText(juce::String(formantShiftSlider_.getValue(), 1), juce::dontSendNotification);
    };

    // Setup AI influence knob
    aiInfluenceKnob_.setRange(0.0, 1.0, 0.01);
    aiInfluenceKnob_.setValue(0.5);
    addAndMakeVisible(aiInfluenceKnob_);
    aiInfluenceAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "ai_influence", aiInfluenceKnob_);

    aiInfluenceLabel_.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
    aiInfluenceLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(aiInfluenceLabel_);

    aiInfluenceValue_.setFont(juce::FontOptions(10.0f));
    aiInfluenceValue_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(aiInfluenceValue_);

    aiInfluenceKnob_.onValueChange = [this]()
    {
        aiInfluenceValue_.setText(juce::String(aiInfluenceKnob_.getValue(), 2), juce::dontSendNotification);
    };

    // Setup vibrato scale slider
    vibratoScaleSlider_.setRange(0.0, 2.0, 0.01);
    vibratoScaleSlider_.setValue(1.0);
    addAndMakeVisible(vibratoScaleSlider_);
    vibratoScaleAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "vibrato_scale", vibratoScaleSlider_);

    vibratoScaleLabel_.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
    vibratoScaleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(vibratoScaleLabel_);

    vibratoScaleValue_.setFont(juce::FontOptions(10.0f));
    vibratoScaleValue_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(vibratoScaleValue_);

    vibratoScaleSlider_.onValueChange = [this]()
    {
        vibratoScaleValue_.setText(juce::String(vibratoScaleSlider_.getValue(), 2), juce::dontSendNotification);
    };

    setSize(800, 120);
}

BottomParameterPanel::~BottomParameterPanel()
{
}

void BottomParameterPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Fill background
    g.setColour(juce::Colour(Theme::kSurface));
    g.fillRect(bounds);

    // Top border line
    g.setColour(juce::Colour(Theme::kBorder));
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
}

void BottomParameterPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    // Divide width into 4 equal columns
    int columnWidth = bounds.getWidth() / 4;

    // Column 1: Pitch Drift
    {
        auto col = bounds.removeFromLeft(columnWidth).reduced(4);
        pitchDriftLabel_.setBounds(col.removeFromTop(16));
        col.removeFromTop(4);
        pitchDriftKnob_.setBounds(col.removeFromTop(50));
        col.removeFromTop(4);
        pitchDriftValue_.setBounds(col);
    }

    // Column 2: Formant Shift
    {
        auto col = bounds.removeFromLeft(columnWidth).reduced(4);
        formantShiftLabel_.setBounds(col.removeFromTop(16));
        col.removeFromTop(4);
        formantShiftSlider_.setBounds(col.removeFromTop(20));
        col.removeFromTop(4);
        formantShiftValue_.setBounds(col.removeFromTop(14));
    }

    // Column 3: AI Influence
    {
        auto col = bounds.removeFromLeft(columnWidth).reduced(4);
        aiInfluenceLabel_.setBounds(col.removeFromTop(16));
        col.removeFromTop(4);
        aiInfluenceKnob_.setBounds(col.removeFromTop(50));
        col.removeFromTop(4);
        aiInfluenceValue_.setBounds(col);
    }

    // Column 4: Vibrato Scale
    {
        auto col = bounds.reduced(4);
        vibratoScaleLabel_.setBounds(col.removeFromTop(16));
        col.removeFromTop(4);
        vibratoScaleSlider_.setBounds(col.removeFromTop(20));
        col.removeFromTop(4);
        vibratoScaleValue_.setBounds(col.removeFromTop(14));
    }
}
