#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace obsidian::ui
{
// Shared logical-pixel tokens. JUCE applies the platform's display scale.
namespace tokens
{
inline constexpr juce::uint32 background = 0xff1b2023;
inline constexpr juce::uint32 surface = 0xff23292d;
inline constexpr juce::uint32 raised = 0xff2c3237;
inline constexpr juce::uint32 hover = 0xff363d43;
inline constexpr juce::uint32 pressed = 0xff181e22;
inline constexpr juce::uint32 border = 0xff373e44;
inline constexpr juce::uint32 text = 0xffe0e3e8;
inline constexpr juce::uint32 muted = 0xffa8afb9;
inline constexpr juce::uint32 faint = 0xff737d87;
inline constexpr juce::uint32 blue = 0xff91accd;
inline constexpr juce::uint32 olive = 0xffb5c89b;
inline constexpr juce::uint32 pink = 0xffc18ead;
inline constexpr juce::uint32 focus = 0xffc8d8eb;
inline constexpr float radius = 4.0f;
inline constexpr float line = 1.0f;
inline constexpr float fontSmall = 12.0f;
inline constexpr float fontBody = 14.0f;
inline constexpr float fontTitle = 20.0f;
inline constexpr int controlSmall = 24;
inline constexpr int controlHeight = 30;
inline constexpr int controlLarge = 38;
inline constexpr int gap = 8;
inline constexpr int panelPadding = 12;
}

inline juce::Colour colour(juce::uint32 argb) { return juce::Colour(argb); }
inline juce::Font font(float size = tokens::fontBody)
{
    return juce::Font(juce::FontOptions(size));
}

enum class Icon
{
    none, chevronDown, chevronRight, plus, minus, play, stop, record, rewind,
    loop, pointer, pencil, scissors, search, more, waveform, microphone,
    backing, clock, signature, chords, logo, close, check
};

void drawIcon(juce::Graphics&, Icon, juce::Rectangle<float>, juce::Colour);
void drawFocus(juce::Graphics&, const juce::Component&);
}
