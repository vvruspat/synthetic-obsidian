#pragma once

#include "AnnotationDocument.h"

namespace vocal_annotation
{
// Offline/message-thread helpers. Keep source curves locally; the note model only receives pitches.
std::vector<NoteBlock> leadNotesForBackingGeneration(const std::vector<NoteBlock>& sourceNotes,
                                                    double maximumGapSeconds = 0.08,
                                                    double maximumPitchDeltaSemitones = 0.80);
std::vector<NoteBlock> parseBackingNotes(const juce::DynamicObject& root,
                                       const std::vector<NoteBlock>& sourceNotes,
                                       juce::String& error);
}
