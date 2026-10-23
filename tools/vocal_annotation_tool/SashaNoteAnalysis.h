#pragma once

#include "../native_note_annotator/NoteBoundaryModel.h"
#include "SibilantModel.h"

namespace vocal_annotation
{

// Owned by the application, used only by its offline analysis worker.
class SashaNoteAnalysis final
{
public:
    struct Draft
    {
        bool succeeded = false;
        double duration = 0.0;
        std::vector<NoteBlock> notes;
        juce::String modelId;
        juce::String modelName;
        juce::String error;
        juce::String warning;
        juce::String sibilantModelId;
        int sibilantMerges = 0;
    };

    Draft analyze(const juce::File& audioFile, const juce::File& modelFile,
                  const std::atomic<bool>& cancelled, const juce::File& sibilantModelFile = {});

    // Pitch observations may enrich a note but never change or remove its boundaries.
    static int attachPitchCurves(std::vector<NoteBlock>& notes,
                                 const std::vector<NoteBlock>& pitchNotes);
    static void applyParts(AnnotationDocument& document, Draft draft);
    static Draft partsForPitch(const AnnotationDocument& document);
    static juce::String inputSignature(const AnnotationDocument& document);

private:
    native_annotation::NoteBoundaryModel model_;
    juce::File loadedModel_;
    juce::Time modelModified_;
    juce::int64 modelSize_ = 0;
    SibilantModel sibilantModel_;
    juce::File loadedSibilantModel_;
    juce::Time sibilantModified_;
    juce::int64 sibilantSize_ = 0;
};

} // namespace vocal_annotation
