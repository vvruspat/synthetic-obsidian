#include "TransportBar.h"

TransportBar::TransportBar()
{
    addAndMakeVisible (resetBtn_);
    addAndMakeVisible (playBtn_);
    addAndMakeVisible (stopBtn_);
    addAndMakeVisible (renderVocalBtn_);
    addAndMakeVisible (renderBacksBtn_);
    addAndMakeVisible (cpuLoadLabel_);
    addAndMakeVisible (bpmLabel_);

    // BPM label — editable on double-click
    bpmLabel_.setEditable      (false, true, false);
    bpmLabel_.setJustificationType (juce::Justification::centred);
    bpmLabel_.setColour (juce::Label::textColourId,
                         juce::Colour (Theme::kTextPrimary));
    bpmLabel_.setColour (juce::Label::backgroundColourId,
                         juce::Colour (Theme::kBackground));
    bpmLabel_.setColour (juce::Label::outlineColourId,
                         juce::Colour (Theme::kBorder));
    bpmLabel_.setFont   (juce::FontOptions (12.0f).withStyle ("Bold"));
    bpmLabel_.onTextChange = [this]
    {
        const double val = bpmLabel_.getText().getDoubleValue();
        const double clamped = juce::jlimit (40.0, 280.0, val > 0.0 ? val : bpm_.load());
        bpm_.store (clamped);
        bpmLabel_.setText (juce::String ((int)clamped) + " BPM",
                           juce::dontSendNotification);
        if (onBpmChanged) onBpmChanged (clamped);
    };

    resetBtn_.onClick       = [this] { if (onReset)       onReset();       };
    playBtn_.onClick        = [this] { if (onPlay)        onPlay();        };
    stopBtn_.onClick        = [this] { if (onStop)        onStop();        };
    renderVocalBtn_.onClick = [this] { if (onRenderVocal) onRenderVocal(); };
    renderBacksBtn_.onClick = [this] { if (onRenderBacks) onRenderBacks(); };

    // Primary action button — cyan fill
    renderVocalBtn_.setColour (juce::TextButton::buttonColourId,
                                juce::Colour (Theme::kAccentCyan));
    renderVocalBtn_.setColour (juce::TextButton::buttonOnColourId,
                                juce::Colour (Theme::kAccentCyan).brighter());
    renderVocalBtn_.setColour (juce::TextButton::textColourOffId,
                                juce::Colour (Theme::kBackground));

    renderBacksBtn_.setColour (juce::TextButton::buttonColourId,
                                juce::Colour (Theme::kSuccess));
    renderBacksBtn_.setColour (juce::TextButton::buttonOnColourId,
                                juce::Colour (Theme::kSuccess).brighter());
    renderBacksBtn_.setColour (juce::TextButton::textColourOffId,
                                juce::Colour (Theme::kBackground));

    cpuLoadLabel_.setFont (juce::FontOptions (10.0f).withStyle ("Bold"));
    cpuLoadLabel_.setJustificationType (juce::Justification::centredLeft);
    cpuLoadLabel_.setColour (juce::Label::textColourId,
                              juce::Colour (Theme::kTextSecondary));

    setSize (800, 50);
}

//==============================================================================
void TransportBar::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    g.setColour (juce::Colour (Theme::kSurface));
    g.fillRect  (bounds);

    g.setColour (juce::Colour (Theme::kBorder));
    g.drawHorizontalLine (0, 0.0f, (float)getWidth());

    // CPU load bar — sits after the CPU label
    const auto cpuBarRect = juce::Rectangle<int> (bounds.getX() + 268,
                                                   bounds.getCentreY() - 4,
                                                   80, 8);
    g.setColour (juce::Colour (Theme::kBorder));
    g.drawRect  (cpuBarRect, 1);

    const float cpu     = cpuLoad_.load();
    const auto  fillW   = (int)((float)cpuBarRect.getWidth() * cpu);
    const auto  fill    = cpuBarRect.withWidth (fillW);
    const auto  barCol  = cpu > 0.85f ? juce::Colour (Theme::kError)
                        : cpu > 0.6f  ? juce::Colour (Theme::kWarning)
                                      : juce::Colour (Theme::kAccentCyan);
    g.setColour (barCol);
    g.fillRect  (fill);
}

void TransportBar::resized()
{
    auto bounds = getLocalBounds().reduced (8, 6);

    // Left: transport buttons
    resetBtn_.setBounds (bounds.removeFromLeft (60));
    bounds.removeFromLeft (4);
    playBtn_.setBounds  (bounds.removeFromLeft (60));
    bounds.removeFromLeft (4);
    stopBtn_.setBounds  (bounds.removeFromLeft (60));
    bounds.removeFromLeft (8);

    // CPU label
    cpuLoadLabel_.setBounds (bounds.removeFromLeft (72));

    // Right: SVC / render actions
    renderVocalBtn_.setBounds (bounds.removeFromRight (140));
    bounds.removeFromRight (6);
    renderBacksBtn_.setBounds (bounds.removeFromRight (96));

    // BPM label — right of CPU bar
    bounds.removeFromLeft (4);
    bpmLabel_.setBounds (bounds.removeFromLeft (72));
}

//==============================================================================
void TransportBar::setCpuLoad (float loadZeroToOne)
{
    cpuLoad_.store (juce::jlimit (0.0f, 1.0f, loadZeroToOne));
    repaint();
}

void TransportBar::setBpm (double bpm)
{
    const double clamped = juce::jlimit (40.0, 280.0, bpm);
    bpm_.store (clamped);
    bpmLabel_.setText (juce::String ((int)clamped) + " BPM",
                       juce::dontSendNotification);
}
