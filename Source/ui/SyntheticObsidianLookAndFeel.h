#pragma once
#include <JuceHeader.h>

// Design tokens — single source of truth for all UI colors and metrics
namespace Theme
{
    // Colors
    inline constexpr juce::uint32 kBackground    = 0xff0a0a12;
    inline constexpr juce::uint32 kSurface       = 0xff0f0f1a;
    inline constexpr juce::uint32 kSurfaceRaised = 0xff14141f;
    inline constexpr juce::uint32 kAccentCyan    = 0xff00d4c8;
    inline constexpr juce::uint32 kAccentPurple  = 0xff8b5cf6;
    inline constexpr juce::uint32 kTextPrimary   = 0xffe2e8f0;
    inline constexpr juce::uint32 kTextSecondary = 0xff64748b;
    inline constexpr juce::uint32 kBorder        = 0x14ffffff;   // rgba(255,255,255,0.08)
    inline constexpr juce::uint32 kGhostBg       = 0x0affffff;   // rgba(255,255,255,0.04)
    inline constexpr juce::uint32 kSuccess       = 0xff22c55e;
    inline constexpr juce::uint32 kWarning       = 0xfffbbf24;
    inline constexpr juce::uint32 kError         = 0xffef4444;

    // Spacing (8px grid)
    inline constexpr int kSpaceXS  = 4;
    inline constexpr int kSpaceS   = 8;
    inline constexpr int kSpaceM   = 16;
    inline constexpr int kSpaceL   = 24;
    inline constexpr int kSpaceXL  = 32;

    // Typography sizes
    inline constexpr float kFontSizeLabel  = 11.0f;
    inline constexpr float kFontSizeBody   = 13.0f;
    inline constexpr float kFontSizeHeader = 16.0f;
    inline constexpr float kFontSizeLogo   = 20.0f;

    // Corner radius
    inline constexpr float kRadius  = 4.0f;
    inline constexpr float kRadiusL = 8.0f;
}

class SyntheticObsidianLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SyntheticObsidianLookAndFeel();

    // Buttons
    void drawButtonBackground(juce::Graphics&, juce::Button&,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawButtonText(juce::Graphics&, juce::TextButton&,
                        bool isHighlighted, bool isDown) override;

    // Rotary slider (knob)
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;

    // Linear slider
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    // Toggle buttons (S/M solo-mute style)
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

    // Helper to get themed font
    juce::Font getLabelFont(juce::Label&) override;

private:
    juce::Font bodyFont_;
    juce::Font headerFont_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SyntheticObsidianLookAndFeel)
};
