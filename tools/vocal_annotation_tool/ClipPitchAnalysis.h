#pragma once
#include "AnnotationDocument.h"

namespace vocal_annotation
{
// Offline worker only. Enriches source-file-local notes without changing their boundaries or IDs.
juce::Result enrichClipPitch(const ArrangementClip&, std::vector<NoteBlock>&,
                             const std::atomic<bool>& cancelled);
}
