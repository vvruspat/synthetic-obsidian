#include "AnnotationDocument.h"

namespace vocal_annotation
{

namespace
{
juce::String idWithPrefix(const char* prefix, int index)
{
    return juce::String(prefix) + juce::String(index).paddedLeft('0', 3);
}
} // namespace

void AnnotationDocument::clear()
{
    audioFile = juce::File();
    instrumentalFile = juce::File();
    backingAudioFile = juce::File();
    sampleRate = 0;
    duration = 0.0;
    bpm = 120.0;
    key = "C major";
    notes.clear();
    backingNotes.clear();
    backingStyleId.clear();
    backingStyleName.clear();
    backingTracks.clear();
    boundaries.clear();
    regions.clear();
    tempoSegments.clear();
    timeSignatures.clear();
    chords.clear();
}

juce::String AnnotationDocument::nextNoteId() const
{
    for (int index = 1; index < 100000; ++index)
    {
        const auto candidate = idWithPrefix("n", index);
        if (findNoteIndex(candidate) == std::nullopt)
            return candidate;
    }

    return idWithPrefix("n", static_cast<int>(notes.size()) + 1);
}

juce::String AnnotationDocument::nextBoundaryId() const
{
    for (int index = 1; index < 100000; ++index)
    {
        const auto candidate = idWithPrefix("b", index);
        if (findBoundaryIndex(candidate) == std::nullopt)
            return candidate;
    }

    return idWithPrefix("b", static_cast<int>(boundaries.size()) + 1);
}

std::optional<int> AnnotationDocument::findNoteIndex(const juce::String& noteId) const
{
    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
        if (notes[static_cast<size_t>(i)].id == noteId)
            return i;

    return std::nullopt;
}

std::optional<int> AnnotationDocument::findBoundaryIndex(const juce::String& boundaryId) const
{
    for (int i = 0; i < static_cast<int>(boundaries.size()); ++i)
        if (boundaries[static_cast<size_t>(i)].id == boundaryId)
            return i;

    return std::nullopt;
}

juce::String toString(BoundaryKind kind)
{
    switch (kind)
    {
        case BoundaryKind::syllable: return "syllable";
        case BoundaryKind::rearticulation: return "rearticulation";
        case BoundaryKind::legato: return "legato";
        case BoundaryKind::breath: return "breath";
        case BoundaryKind::noise: return "noise";
        case BoundaryKind::pause: return "pause";
        case BoundaryKind::ignore: return "ignore";
    }

    return "syllable";
}

BoundaryKind boundaryKindFromString(const juce::String& text)
{
    if (text == "rearticulation")
        return BoundaryKind::rearticulation;
    if (text == "legato")
        return BoundaryKind::legato;
    if (text == "breath")
        return BoundaryKind::breath;
    if (text == "noise")
        return BoundaryKind::noise;
    if (text == "pause")
        return BoundaryKind::pause;
    if (text == "ignore")
        return BoundaryKind::ignore;

    return BoundaryKind::syllable;
}

juce::StringArray allBoundaryKindNames()
{
    return { "syllable", "rearticulation", "legato", "breath", "noise", "pause", "ignore" };
}

} // namespace vocal_annotation
