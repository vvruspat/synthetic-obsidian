#pragma once

#include "AnnotationDocument.h"

namespace vocal_annotation
{

struct ValidationIssue
{
    juce::String message;
    bool isError = true;
};

class AnnotationValidator
{
public:
    static std::vector<ValidationIssue> validate(const AnnotationDocument& document);
    static juce::String summarize(const std::vector<ValidationIssue>& issues);
};

} // namespace vocal_annotation
