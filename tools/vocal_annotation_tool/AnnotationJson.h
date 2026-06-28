#pragma once

#include "AnnotationDocument.h"

namespace vocal_annotation
{

class AnnotationJson
{
public:
    static juce::Result load(const juce::File& jsonFile, AnnotationDocument& document);
    static juce::Result save(const AnnotationDocument& document, const juce::File& jsonFile);
    static juce::Result exportMidi(const AnnotationDocument& document, const juce::File& midiFile);
};

} // namespace vocal_annotation
