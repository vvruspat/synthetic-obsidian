#include "NoteEditing.h"

#include <algorithm>
#include <cmath>

namespace native_annotation::NoteEditing
{
void trim(vocal_annotation::NoteBlock& note)
{
    note.voicedStart = juce::jlimit(note.start, note.end, note.voicedStart);
    note.voicedEnd = juce::jlimit(note.voicedStart, note.end, note.voicedEnd);
    std::erase_if(note.curve, [&](const auto& point) { return point.time < note.start || point.time > note.end; });
    note.flags.addIfNotAlreadyThere("manual_edit");
}

bool insert(vocal_annotation::AnnotationDocument& document, const NoteAnalysis& analysis, double start)
{
    if (! std::isfinite(start) || start < 0.0 || start + kMinLength > document.duration)
        return false;
    auto limit = document.duration;
    for (size_t i = 0; i < document.notes.size(); ++i)
    {
        auto& note = document.notes[i];
        if (start >= note.start && start < note.end)
        {
            if (start - note.start < kMinLength || note.end - start < kMinLength)
                return false;
            auto right = note;
            right.id = document.nextNoteId();
            right.start = start;
            note.end = start;
            trim(note);
            trim(right);
            document.notes.insert(document.notes.begin() + static_cast<std::ptrdiff_t>(i + 1), std::move(right));
            return true;
        }
        if (note.start > start)
            limit = juce::jmin(limit, note.start);
    }
    if (limit - start < kMinLength)
        return false;
    auto note = analysis.suggest(start, limit);
    note.id = document.nextNoteId();
    trim(note);
    document.notes.push_back(std::move(note));
    std::sort(document.notes.begin(), document.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return true;
}

bool resize(vocal_annotation::AnnotationDocument& document, size_t index, bool startEdge, double time)
{
    if (index >= document.notes.size() || ! std::isfinite(time))
        return false;
    auto& note = document.notes[index];
    if (startEdge)
    {
        if (index > 0 && std::abs(document.notes[index - 1].end - note.start) < 1.0e-6)
        {
            auto& previous = document.notes[index - 1];
            if (previous.start + kMinLength > note.end - kMinLength) return false;
            note.start = juce::jlimit(previous.start + kMinLength, note.end - kMinLength, time);
            previous.end = note.start;
            trim(previous);
            trim(note);
            return true;
        }
        const auto lower = index > 0 ? document.notes[index - 1].end : 0.0;
        note.start = juce::jlimit(lower, juce::jmax(lower, note.end - kMinLength), time);
    }
    else
    {
        if (index + 1 < document.notes.size() && std::abs(note.end - document.notes[index + 1].start) < 1.0e-6)
        {
            auto& next = document.notes[index + 1];
            if (note.start + kMinLength > next.end - kMinLength) return false;
            note.end = juce::jlimit(note.start + kMinLength, next.end - kMinLength, time);
            next.start = note.end;
            trim(next);
            trim(note);
            return true;
        }
        const auto upper = index + 1 < document.notes.size() ? document.notes[index + 1].start : document.duration;
        note.end = juce::jlimit(juce::jmin(upper, note.start + kMinLength), upper, time);
    }
    trim(note);
    return true;
}
}
