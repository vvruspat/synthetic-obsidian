#pragma once
#include "AnnotationDocument.h"

namespace vocal_annotation::arrangement
{
ArrangementTrack* findTrack(AnnotationDocument&, const juce::String&);
const ArrangementTrack* findTrack(const AnnotationDocument&, const juce::String&);
std::vector<NoteBlock>& notes(AnnotationDocument&, const juce::String&);
const std::vector<NoteBlock>& notes(const AnnotationDocument&, const juce::String&);
void migrate(AnnotationDocument&);
void syncGeneratedTracks(AnnotationDocument&);
void assignClipIds(AnnotationDocument&);
double duration(const AnnotationDocument&);
bool moveClip(AnnotationDocument&, const juce::String& trackId, const juce::String& clipId,
              const juce::String& targetTrackId, double start);
bool removeClip(AnnotationDocument&, const juce::String& trackId, const juce::String& clipId);
bool removeTrack(AnnotationDocument&, const juce::String& trackId);
void shiftNote(NoteBlock&, double delta);
juce::var toJson(const AnnotationDocument&, const juce::File& baseDirectory);
juce::Result fromJson(const juce::var&, AnnotationDocument&, const juce::File& baseDirectory);
}
