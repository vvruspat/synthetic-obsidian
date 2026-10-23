#pragma once

#include "NoteAnalysis.h"

namespace native_annotation
{
namespace NoteEditing
{
constexpr double kMinLength = 0.025;
bool insert(vocal_annotation::AnnotationDocument& document, const NoteAnalysis& analysis, double start);
bool resize(vocal_annotation::AnnotationDocument& document, size_t index, bool startEdge, double time);
void trim(vocal_annotation::NoteBlock& note);
}
}
