#include "BackingVocalNotes.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace vocal_annotation
{
namespace
{
std::vector<PitchCurvePoint> expressionCurveForBacking(const NoteBlock& note)
{
    // Raw F0 can jump to another harmonic even at high detector confidence.
    // Transfer expression near each original note, before merging syllables;
    // leave deliberately edited contours and valid vibrato samples untouched.
    constexpr double maximumDetectedDeviationSemitones = 3.0;
    const auto authored = note.flags.contains("explicit_pitch_curve");
    std::vector<PitchCurvePoint> curve;
    for (const auto& point : note.curve)
        if (std::isfinite(point.time) && std::isfinite(point.midi) && std::isfinite(point.confidence)
            && point.time >= note.start && point.time <= note.end
            && point.midi >= 0.0 && point.midi <= 127.0 && point.confidence > 0.0)
            curve.push_back(point);
    std::stable_sort(curve.begin(), curve.end(), [](const auto& left, const auto& right)
    {
        return left.time < right.time;
    });

    std::vector<size_t> reliable;
    for (size_t index = 0; index < curve.size(); ++index)
        if (authored || std::abs(curve[index].midi - note.pitchExact) <= maximumDetectedDeviationSemitones)
            reliable.push_back(index);
    if (reliable.empty())
        return {};

    size_t next = 0;
    for (size_t index = 0; index < curve.size(); ++index)
    {
        while (next + 1 < reliable.size() && reliable[next] < index)
            ++next;
        if (reliable[next] == index)
            continue;
        const auto& left = curve[reliable[next > 0 ? next - 1 : 0]];
        const auto& right = curve[reliable[next]];
        const auto progress = right.time > left.time
            ? juce::jlimit(0.0, 1.0, (curve[index].time - left.time) / (right.time - left.time)) : 0.0;
        curve[index].midi = left.midi + progress * (right.midi - left.midi);
        curve[index].confidence = juce::jmin(left.confidence, right.confidence);
    }
    return curve;
}

bool canMergeLeadNotesForBacking(const NoteBlock& previous,
                                const NoteBlock& current,
                                double maximumGapSeconds,
                                double maximumPitchDeltaSemitones)
{
    if (previous.end <= previous.start || current.end <= current.start)
        return false;

    if (previous.flags.contains("non_vocal_trimmed_end")
        || current.flags.contains("non_vocal_trimmed_start"))
    {
        return false;
    }

    const auto gap = current.start - previous.end;
    if (gap > maximumGapSeconds)
        return false;

    if (previous.syllableId.isNotEmpty() && current.syllableId.isNotEmpty())
        return previous.syllableId == current.syllableId;

    if (std::abs(current.pitchExact - previous.pitchExact) >= maximumPitchDeltaSemitones)
        return false;

    // Some pitch plateaus at the edge of a detected syllable are intentionally
    // left without text/id. Treat an adjacent unlabelled plateau as a continuation,
    // but never merge two explicitly different syllables.
    if (previous.syllableId.isNotEmpty() || current.syllableId.isNotEmpty())
    {
        const auto& unlabelled = previous.syllableId.isEmpty() ? previous : current;
        return unlabelled.lyric.trim().isEmpty() && gap <= 0.03;
    }

    if (previous.lyric.trim().isNotEmpty() || current.lyric.trim().isNotEmpty())
        return previous.lyric.trim() == current.lyric.trim();

    return gap <= 0.03;
}

void mergeLeadNoteForBacking(NoteBlock& target, const NoteBlock& source)
{
    target.end = juce::jmax(target.end, source.end);
    target.voicedStart = juce::jmin(target.voicedStart, source.voicedStart);
    target.voicedEnd = juce::jmax(target.voicedEnd, source.voicedEnd);

    if (target.lyric.trim().isEmpty())
        target.lyric = source.lyric;
    if (target.syllableId.isEmpty())
        target.syllableId = source.syllableId;

    // Keep the articulation at the beginning of the merged note, but inherit
    // the outgoing legato state from its final absorbed plateau.
    target.flags.removeString("legato_to_next");
    if (source.flags.contains("legato_to_next"))
        target.flags.addIfNotAlreadyThere("legato_to_next");
    if (source.flags.contains("melisma_continuation"))
        target.flags.addIfNotAlreadyThere("melisma_continuation");

    target.curve.insert(target.curve.end(), source.curve.begin(), source.curve.end());
}

void inheritPitchCurve(NoteBlock& target, const NoteBlock& source)
{
    const auto sourceDuration = source.end - source.start;
    if (! std::isfinite(sourceDuration) || sourceDuration <= 0.0 || ! std::isfinite(source.pitchExact))
        return;

    // Transpose in semitones so drift/vibrato depth stays the same in every register.
    const auto pitchDelta = target.pitchExact - source.pitchExact;
    std::vector<PitchCurvePoint> curve;
    curve.reserve(source.curve.size());
    for (const auto& point : source.curve)
    {
        if (! std::isfinite(point.time) || ! std::isfinite(point.midi) || ! std::isfinite(point.confidence)
            || point.time < source.start || point.time > source.end)
            continue;
        const auto progress = (point.time - source.start) / sourceDuration;
        curve.push_back({ target.start + progress * (target.end - target.start),
                          juce::jlimit(0.0, 127.0, point.midi + pitchDelta),
                          juce::jlimit(0.0, 1.0, point.confidence) });
    }
    if (curve.empty())
        return;
    std::stable_sort(curve.begin(), curve.end(), [](const auto& left, const auto& right)
    {
        return left.time < right.time;
    });
    if (curve.front().time > target.start)
        curve.insert(curve.begin(), { target.start, curve.front().midi, curve.front().confidence });
    if (curve.back().time < target.end)
        curve.push_back({ target.end, curve.back().midi, curve.back().confidence });
    target.curve = std::move(curve);
    target.flags.addIfNotAlreadyThere("explicit_pitch_curve");
}
}

std::vector<NoteBlock> leadNotesForBackingGeneration(const std::vector<NoteBlock>& sourceNotes,
                                                    double maximumGapSeconds,
                                                    double maximumPitchDeltaSemitones)
{
    std::vector<NoteBlock> notes;
    std::vector<double> anchorDurations;
    notes.reserve(sourceNotes.size());
    anchorDurations.reserve(sourceNotes.size());

    for (const auto& note : sourceNotes)
    {
        if (note.end <= note.start)
            continue;

        auto canonical = note;
        canonical.curve = expressionCurveForBacking(note);
        canonical.pitchExact = static_cast<double>(canonical.pitch);
        if (! notes.empty()
            && canMergeLeadNotesForBacking(notes.back(), note, maximumGapSeconds, maximumPitchDeltaSemitones))
        {
            const auto sourceDuration = juce::jmax(0.001, note.voicedEnd - note.voicedStart);
            if (sourceDuration > anchorDurations.back())
            {
                notes.back().pitch = note.pitch;
                notes.back().pitchExact = static_cast<double>(note.pitch);
                anchorDurations.back() = sourceDuration;
            }
            mergeLeadNoteForBacking(notes.back(), canonical);
            continue;
        }

        // Keep the cleaned contour locally, including the first merged plateau.
        notes.push_back(std::move(canonical));
        anchorDurations.push_back(juce::jmax(0.001, note.voicedEnd - note.voicedStart));
    }

    return notes;
}

std::vector<NoteBlock> parseBackingNotes(const juce::DynamicObject& root,
                                       const std::vector<NoteBlock>& sourceNotes,
                                       juce::String& error)
{
    error.clear();
    auto* notes = root.getProperty("notes").getArray();
    if (notes == nullptr || notes->isEmpty())
    {
        error = "Backing vocal model returned no notes.";
        return {};
    }

    const auto numberProperty = [](const juce::DynamicObject& object, const juce::Identifier& name, double fallback)
    {
        const auto value = object.getProperty(name);
        return value.isDouble() || value.isInt() ? static_cast<double>(value) : fallback;
    };
    const auto stringProperty = [](const juce::DynamicObject& object, const juce::Identifier& name, const juce::String& fallback = {})
    {
        const auto value = object.getProperty(name);
        return value.isString() ? value.toString() : fallback;
    };
    std::vector<NoteBlock> generatedNotes;
    generatedNotes.reserve(static_cast<size_t>(notes->size()));
    std::map<juce::String, const NoteBlock*> sources;
    // backing_vocal_music.normalize_lead assigns these IDs before sorting/windows.
    // Document IDs may differ (including after syllable merges or clip trimming).
    for (size_t index = 0; index < sourceNotes.size(); ++index)
        sources.emplace("lead_" + juce::String(static_cast<int>(index) + 1).paddedLeft('0', 4), &sourceNotes[index]);
    for (const auto& value : *notes)
    {
        auto* object = value.getDynamicObject();
        if (object == nullptr)
            continue;

        NoteBlock note;
        note.id = stringProperty(*object, "id", "bv_" + juce::String(static_cast<int>(generatedNotes.size()) + 1).paddedLeft('0', 3));
        note.start = numberProperty(*object, "start", 0.0);
        note.end = numberProperty(*object, "end", note.start + numberProperty(*object, "duration", 0.25));
        const auto pitch = numberProperty(*object, "midi_note", 60.0);
        if (! std::isfinite(note.start) || ! std::isfinite(note.end) || note.end <= note.start || ! std::isfinite(pitch))
            continue;
        note.pitch = static_cast<int>(juce::jlimit(0.0, 127.0, pitch));
        note.pitchExact = static_cast<double>(note.pitch);
        note.voicedStart = note.start;
        note.voicedEnd = note.end;
        note.lyric = stringProperty(
            *object,
            "lyric",
            stringProperty(*object, "strategy", "BV"));
        note.syllableId = stringProperty(*object, "syllable_id");
        note.flags.add("backing_vocal");
        if (static_cast<bool>(object->getProperty("legato_from_previous")))
            note.flags.add("legato_from_previous");
        if (static_cast<bool>(object->getProperty("legato_to_next")))
            note.flags.add("legato_to_next");
        if (static_cast<bool>(object->getProperty("melisma_continuation")))
            note.flags.add("melisma_continuation");
        note.curve.push_back({ note.start, note.pitchExact, 1.0 });
        note.curve.push_back({ note.end, note.pitchExact, 1.0 });
        const auto source = sources.find(stringProperty(*object, "source_lead_ref"));
        if (source != sources.end())
            inheritPitchCurve(note, *source->second);

        if (note.end > note.start)
            generatedNotes.push_back(std::move(note));
    }

    if (generatedNotes.empty())
        error = "Backing vocal model returned only invalid notes.";

    return generatedNotes;
}

}
