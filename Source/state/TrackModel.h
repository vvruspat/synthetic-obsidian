#pragma once
#include <JuceHeader.h>

enum class TrackType { Guide, MainVox, BackVox };

enum class BackVocalStyle
{
    None, Drone, Third, Fifth, Octave,
    DoubleUp, DoubleDown
};

struct TrackModel
{
    juce::Uuid   id;
    TrackType    type       { TrackType::BackVox };
    juce::String name;
    juce::String voicePresetName;  // RVC preset name, empty if none
    BackVocalStyle style    { BackVocalStyle::None };

    bool isMuted  { false };
    bool isSolo   { false };
    float volume  { 1.0f };

    // Audio file path for recorded/imported audio
    juce::File audioFile;

    // Convert to/from ValueTree for serialization
    juce::ValueTree toValueTree() const;
    static TrackModel fromValueTree(const juce::ValueTree& vt);

    // Factory helpers
    static TrackModel makeGuide();
    static TrackModel makeMainVox();
    static TrackModel makeBackVox(const juce::String& name, BackVocalStyle style = BackVocalStyle::None);
};
