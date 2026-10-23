#pragma once

#include "AnnotationDocument.h"

namespace native_annotation
{
struct DatasetMetadata
{
    juce::String sourceHash;
    juce::String audioHash;
    juce::String singer;
    juce::String session;
    juce::String modelId;
    juce::String modelHash;
    bool reviewed = false;
};

namespace DatasetExport
{
juce::Result validate(const vocal_annotation::AnnotationDocument& document);
juce::Result saveProject(const vocal_annotation::AnnotationDocument& document,
                         const DatasetMetadata& metadata, const juce::File& file);
DatasetMetadata readMetadata(const juce::File& file);
// Worker-thread only. Publish the complete bundle by renaming a staging directory.
juce::Result write(const vocal_annotation::AnnotationDocument& document,
                   const DatasetMetadata& metadata, const juce::AudioBuffer<float>& audio,
                   const juce::File& destination, juce::File& publishedDirectory);
}
}
