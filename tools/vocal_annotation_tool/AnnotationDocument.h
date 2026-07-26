#pragma once

#include <JuceHeader.h>

#include <optional>
#include <vector>

namespace vocal_annotation
{

struct PitchCurvePoint
{
    double time = 0.0;
    double midi = 60.0;
    double confidence = 1.0;
};

struct NoteBlock
{
    juce::String id;
    double start = 0.0;
    double end = 0.25;
    int pitch = 60;
    double pitchExact = 60.0;
    double gainDb = 0.0;
    double voicedStart = 0.0;
    double voicedEnd = 0.25;
    juce::String lyric;
    juce::String syllableId;
    juce::StringArray flags;
    std::vector<PitchCurvePoint> curve;
};

enum class BoundaryKind
{
    syllable,
    rearticulation,
    legato,
    breath,
    noise,
    pause,
    ignore
};

struct BoundaryMarker
{
    juce::String id;
    double time = 0.0;
    BoundaryKind kind = BoundaryKind::syllable;
    juce::String text;
    double confidence = 1.0;
    juce::String source;
};

struct AnnotationRegion
{
    double start = 0.0;
    double end = 0.0;
    juce::String kind;
    juce::String noteId;
};

struct TempoSegment
{
    double start = 0.0;
    double end = 0.0;
    double bpm = 120.0;
    double confidence = 0.0;
};

struct TimeSignatureSegment
{
    double start = 0.0;
    double end = 0.0;
    int numerator = 4;
    int denominator = 4;
    double confidence = 0.0;
};

struct ChordSegment
{
    double start = 0.0;
    double end = 0.0;
    juce::String name;
    double confidence = 0.0;
};

struct BackingVocalTrack
{
    juce::String styleId;
    juce::String styleName;
    std::vector<NoteBlock> notes;
    std::vector<NoteBlock> renderedNotes;
    juce::File audioFile;
};

struct AnnotationDocument
{
    int version = 1;
    juce::File audioFile;
    juce::File instrumentalFile;
    juce::File backingAudioFile;
    int sampleRate = 0;
    double duration = 0.0;
    double bpm = 120.0;
    juce::String key = "C major";
    std::vector<NoteBlock> notes;
    std::vector<NoteBlock> backingNotes;
    juce::String backingStyleId;
    juce::String backingStyleName;
    std::vector<BackingVocalTrack> backingTracks;
    std::vector<BoundaryMarker> boundaries;
    std::vector<AnnotationRegion> regions;
    std::vector<TempoSegment> tempoSegments;
    std::vector<TimeSignatureSegment> timeSignatures;
    std::vector<ChordSegment> chords;

    void clear();
    juce::String nextNoteId() const;
    juce::String nextBoundaryId() const;
    std::optional<int> findNoteIndex(const juce::String& noteId) const;
    std::optional<int> findBoundaryIndex(const juce::String& boundaryId) const;
};

juce::String toString(BoundaryKind kind);
BoundaryKind boundaryKindFromString(const juce::String& text);
juce::StringArray allBoundaryKindNames();

} // namespace vocal_annotation
