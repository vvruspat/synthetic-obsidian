#include "AnnotationValidator.h"

#include <algorithm>
#include <cmath>

namespace vocal_annotation
{

std::vector<ValidationIssue> AnnotationValidator::validate(const AnnotationDocument& document)
{
    std::vector<ValidationIssue> issues;

    if (! document.audioFile.existsAsFile())
        issues.push_back({ "Audio path does not exist.", true });

    if (document.duration <= 0.0)
        issues.push_back({ "Duration must be greater than zero.", true });

    for (size_t i = 0; i < document.notes.size(); ++i)
    {
        const auto& note = document.notes[i];
        const auto label = note.id.isNotEmpty() ? note.id : juce::String(static_cast<int>(i));

        if (note.end <= note.start)
            issues.push_back({ "Note " + label + " has end <= start.", true });

        if (note.start < 0.0 || note.end > document.duration)
            issues.push_back({ "Note " + label + " is outside the clip duration.", true });

        if (note.pitch < 0 || note.pitch > 127)
            issues.push_back({ "Note " + label + " pitch is outside MIDI range.", true });

        if (note.pitchExact < 0.0 || note.pitchExact > 127.0)
            issues.push_back({ "Note " + label + " exact pitch is outside MIDI range.", true });

        if (! std::isfinite(note.gainDb) || note.gainDb < -24.0 || note.gainDb > 12.0)
            issues.push_back({ "Note " + label + " gain is outside the -24..+12 dB range.", true });

        if (note.voicedStart < note.start || note.voicedEnd > note.end || note.voicedEnd < note.voicedStart)
            issues.push_back({ "Note " + label + " voiced range is outside note bounds.", true });

        if (! std::is_sorted(note.curve.begin(), note.curve.end(), [](const auto& a, const auto& b) { return a.time < b.time; }))
            issues.push_back({ "Note " + label + " curve points are not sorted.", true });

        for (const auto& point : note.curve)
            if (point.time < 0.0 || point.time > document.duration)
                issues.push_back({ "Note " + label + " has a curve point outside the clip.", true });
    }

    auto sortedNotes = document.notes;
    std::sort(sortedNotes.begin(), sortedNotes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    for (size_t i = 1; i < sortedNotes.size(); ++i)
        if (sortedNotes[i - 1].end > sortedNotes[i].start
            && ! sortedNotes[i - 1].flags.contains("allow_overlap")
            && ! sortedNotes[i].flags.contains("allow_overlap"))
        {
            issues.push_back({ "Notes " + sortedNotes[i - 1].id + " and " + sortedNotes[i].id + " overlap.", true });
        }

    for (const auto& boundary : document.boundaries)
        if (boundary.time < 0.0 || boundary.time > document.duration)
            issues.push_back({ "Boundary " + boundary.id + " is outside the clip duration.", true });

    for (const auto& region : document.regions)
        if (region.end <= region.start || region.start < 0.0 || region.end > document.duration)
            issues.push_back({ "Region " + region.kind + " has invalid bounds.", true });

    return issues;
}

juce::String AnnotationValidator::summarize(const std::vector<ValidationIssue>& issues)
{
    if (issues.empty())
        return "Validation passed.";

    juce::String summary;
    for (const auto& issue : issues)
        summary << (issue.isError ? "Error: " : "Warning: ") << issue.message << "\n";

    return summary.trimEnd();
}

} // namespace vocal_annotation
