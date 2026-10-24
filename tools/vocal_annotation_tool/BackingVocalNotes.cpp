#include "BackingVocalNotes.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace vocal_annotation
{
namespace
{
constexpr double kBoundaryMarginSemitones = 1.0;

PitchCurvePoint curvePointAt(const std::vector<PitchCurvePoint>& curve, double time)
{
    const auto right = std::lower_bound(curve.begin(), curve.end(), time,
        [](const auto& point, double value) { return point.time < value; });
    if (right == curve.begin()) return { time, right->midi, right->confidence };
    if (right == curve.end()) return { time, curve.back().midi, curve.back().confidence };
    const auto& left = *(right - 1);
    const auto fraction = (time - left.time) / juce::jmax(1e-9, right->time - left.time);
    return { time, left.midi + fraction * (right->midi - left.midi),
             left.confidence + fraction * (right->confidence - left.confidence) };
}

bool repairCurveEdge(NoteBlock& note, bool atStart, double lowerPitch, double upperPitch)
{
    const auto duration = note.end - note.start;
    if (note.curve.empty() || !std::isfinite(duration) || duration <= 0.0)
        return false;
    if (std::any_of(note.curve.begin(), note.curve.end(), [](const auto& point)
    {
        return !std::isfinite(point.time) || !std::isfinite(point.midi) || !std::isfinite(point.confidence);
    }))
        return false;
    auto source = note.curve;
    std::stable_sort(source.begin(), source.end(), [](const auto& left, const auto& right) { return left.time < right.time; });
    const auto window = juce::jmin(0.22, duration * 0.45);
    const auto guard = juce::jmin(0.08, window * 0.5);
    const auto distanceFromEdge = [&](double time) { return atStart ? time - note.start : note.end - time; };
    const auto lowerCentre = lowerPitch + kBoundaryMarginSemitones;
    const auto upperCentre = upperPitch - kBoundaryMarginSemitones;
    const auto edgePitch = atStart ? source.front().midi : source.back().midi;
    const auto guardPitch = curvePointAt(source, atStart ? note.start + guard : note.end - guard).midi;
    auto maximumBelow = juce::jmax(0.0, lowerCentre - juce::jmin(edgePitch, guardPitch));
    auto maximumAbove = juce::jmax(0.0, juce::jmax(edgePitch, guardPitch) - upperCentre);
    for (const auto& point : source)
    {
        const auto distance = distanceFromEdge(point.time);
        if (distance >= 0.0 && distance <= guard)
        {
            maximumBelow = juce::jmax(maximumBelow, lowerCentre - point.midi);
            maximumAbove = juce::jmax(maximumAbove, point.midi - upperCentre);
        }
    }
    if (maximumBelow <= kBoundaryMarginSemitones + 1e-7 && maximumAbove <= kBoundaryMarginSemitones + 1e-7)
        return false;
    const auto lowerScale = kBoundaryMarginSemitones / juce::jmax(kBoundaryMarginSemitones, maximumBelow);
    const auto upperScale = kBoundaryMarginSemitones / juce::jmax(kBoundaryMarginSemitones, maximumAbove);

    // Add local samples so even a sparse saved contour recovers smoothly to
    // its original curve. Never smooth or decimate the sustained vibrato.
    for (double distance = 0.0; distance < window; distance += 0.01)
        note.curve.push_back(curvePointAt(source, atStart ? note.start + distance : note.end - distance));
    for (const auto distance : {guard, window})
        note.curve.push_back(curvePointAt(source, atStart ? note.start + distance : note.end - distance));
    std::stable_sort(note.curve.begin(), note.curve.end(), [](const auto& left, const auto& right) { return left.time < right.time; });
    note.curve.erase(std::unique(note.curve.begin(), note.curve.end(), [](const auto& left, const auto& right)
    {
        return std::abs(left.time - right.time) < 1e-9;
    }), note.curve.end());
    for (auto& point : note.curve)
    {
        const auto distance = distanceFromEdge(point.time);
        if (distance < 0.0 || distance >= window)
            continue;
        const auto progress = juce::jlimit(0.0, 1.0, (distance - guard) / (window - guard));
        const auto weight = 1.0 - progress * progress * (3.0 - 2.0 * progress);
        // Scale the whole offending excursion about the musical interval.
        // Sample-wise clipping would flatten the attack into a horizontal shelf.
        const auto shaped = point.midi < lowerCentre ? lowerCentre + (point.midi - lowerCentre) * lowerScale
                          : point.midi > upperCentre ? upperCentre + (point.midi - upperCentre) * upperScale
                          : point.midi;
        point.midi += (shaped - point.midi) * weight;
    }
    note.flags.addIfNotAlreadyThere("explicit_pitch_curve");
    return true;
}

bool connectRepairedEdges(NoteBlock& previous, NoteBlock& current)
{
    for (const auto* note : {&previous, &current})
        if (note->curve.empty() || std::any_of(note->curve.begin(), note->curve.end(), [](const auto& point)
        {
            return !std::isfinite(point.time) || !std::isfinite(point.midi) || !std::isfinite(point.confidence);
        }))
            return false;
    const auto start = previous.end - juce::jmin(0.08, (previous.end - previous.start) * 0.20);
    const auto end = current.start + juce::jmin(0.08, (current.end - current.start) * 0.20);
    for (auto* note : {&previous, &current})
        std::stable_sort(note->curve.begin(), note->curve.end(), [](const auto& left, const auto& right) { return left.time < right.time; });
    const auto from = curvePointAt(previous.curve, start).midi;
    const auto to = curvePointAt(current.curve, end).midi;
    for (auto* note : {&previous, &current})
    {
        const auto source = note->curve;
        const auto begin = juce::jmax(start, note->start), finish = juce::jmin(end, note->end);
        for (double time = begin; time < finish; time += 0.01)
            note->curve.push_back(curvePointAt(source, time));
        note->curve.push_back(curvePointAt(source, finish));
        for (auto& point : note->curve)
            if (point.time >= begin && point.time <= finish)
            {
                const auto progress = juce::jlimit(0.0, 1.0, (point.time - start) / (end - start));
                point.midi = from + (to - from) * progress * progress * (3.0 - 2.0 * progress);
            }
        std::stable_sort(note->curve.begin(), note->curve.end(), [](const auto& left, const auto& right) { return left.time < right.time; });
        note->curve.erase(std::unique(note->curve.begin(), note->curve.end(), [](const auto& left, const auto& right)
        { return std::abs(left.time - right.time) < 1e-9; }), note->curve.end());
        note->flags.addIfNotAlreadyThere("explicit_pitch_curve");
    }
    return true;
}

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

int repairBackingPitchTransitions(std::vector<NoteBlock>& notes)
{
    std::vector<size_t> order;
    for (size_t index = 0; index < notes.size(); ++index)
        if (std::isfinite(notes[index].start) && std::isfinite(notes[index].end)
            && std::isfinite(notes[index].pitchExact) && notes[index].end > notes[index].start)
            order.push_back(index);
    std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) { return notes[left].start < notes[right].start; });
    std::vector<bool> changed(notes.size(), false);
    for (size_t index = 1; index < order.size(); ++index)
    {
        auto& previous = notes[order[index - 1]];
        auto& current = notes[order[index]];
        const auto gap = current.start - previous.end;
        // Do not join overlapping voices, separate clips or rearticulations
        // after a pause. The bounds follow the generated harmony, not the lead.
        if (gap < -0.001 || gap > 0.08
            || (previous.clipId.isNotEmpty() && current.clipId.isNotEmpty() && previous.clipId != current.clipId))
            continue;
        const auto lower = juce::jmin(previous.pitchExact, current.pitchExact) - kBoundaryMarginSemitones;
        const auto upper = juce::jmax(previous.pitchExact, current.pitchExact) + kBoundaryMarginSemitones;
        const auto previousChanged = repairCurveEdge(previous, false, lower, upper);
        const auto currentChanged = repairCurveEdge(current, true, lower, upper);
        if (previousChanged || currentChanged)
        {
            // Limiting the overshoot alone still leaves an instantaneous jump
            // between notes. Use the same smooth transition on both sides.
            const auto connected = connectRepairedEdges(previous, current);
            if (previousChanged || connected) changed[order[index - 1]] = true;
            if (currentChanged || connected) changed[order[index]] = true;
        }
    }
    return static_cast<int>(std::count(changed.begin(), changed.end(), true));
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

    repairBackingPitchTransitions(generatedNotes);
    return generatedNotes;
}

}
