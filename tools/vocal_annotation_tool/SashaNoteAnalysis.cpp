#include "SashaNoteAnalysis.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vocal_annotation
{

juce::String SashaNoteAnalysis::inputSignature(const AnnotationDocument& document)
{
    // Tempo/chord analysis is independent of vocal annotations and may complete concurrently.
    juce::MemoryOutputStream state;
    state.writeString(document.audioFile.getFullPathName());
    const auto writeNotes = [&state](const std::vector<NoteBlock>& notes)
    {
        state.writeInt(static_cast<int>(notes.size()));
        for (const auto& note : notes)
        {
            state.writeString(note.id);
            state.writeDouble(note.start); state.writeDouble(note.end);
            state.writeDouble(note.pitchExact); state.writeInt(note.pitch);
            state.writeDouble(note.gainDb); state.writeString(note.lyric);
            state.writeString(note.flags.joinIntoString("\n"));
            state.writeInt(static_cast<int>(note.curve.size()));
            for (const auto& point : note.curve)
            {
                state.writeDouble(point.time); state.writeDouble(point.midi); state.writeDouble(point.confidence);
            }
        }
    };
    writeNotes(document.notes);
    writeNotes(document.backingNotes);
    state.writeString(document.backingStyleId);
    state.writeInt(static_cast<int>(document.backingTracks.size()));
    for (const auto& track : document.backingTracks)
    {
        state.writeString(track.id); state.writeString(track.audioFile.getFullPathName());
        writeNotes(track.notes); writeNotes(track.renderedNotes);
    }
    state.writeInt(static_cast<int>(document.boundaries.size()));
    for (const auto& boundary : document.boundaries)
    {
        state.writeString(boundary.id); state.writeDouble(boundary.time);
        state.writeInt(static_cast<int>(boundary.kind));
        state.writeString(boundary.text); state.writeString(boundary.source);
    }
    state.writeInt(static_cast<int>(document.regions.size()));
    for (const auto& region : document.regions)
    {
        state.writeDouble(region.start); state.writeDouble(region.end);
        state.writeString(region.kind); state.writeString(region.noteId);
    }
    return juce::SHA256(state.getData(), state.getDataSize()).toHexString();
}

SashaNoteAnalysis::Draft SashaNoteAnalysis::analyze(
    const juce::File& audioFile, const juce::File& modelFile,
    const std::atomic<bool>& cancelled, const juce::File& sibilantModelFile)
{
    Draft draft;
    if (cancelled.load(std::memory_order_relaxed))
        return draft;
    if (! modelFile.existsAsFile())
    {
        draft.error = "Sasha note model is not installed.";
        return draft;
    }
    if (modelFile != loadedModel_ || modelFile.getLastModificationTime() != modelModified_
        || modelFile.getSize() != modelSize_)
    {
        loadedModel_ = juce::File();
        if (const auto loaded = model_.load(modelFile); loaded.failed())
        {
            draft.error = loaded.getErrorMessage();
            return draft;
        }
        loadedModel_ = modelFile;
        modelModified_ = modelFile.getLastModificationTime();
        modelSize_ = modelFile.getSize();
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audioFile));
    if (reader == nullptr || ! std::isfinite(reader->sampleRate)
        || reader->sampleRate < 8000.0 || reader->sampleRate > 192000.0
        || reader->numChannels < 1 || reader->numChannels > 2
        || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max()
        || static_cast<juce::uint64>(reader->lengthInSamples) * reader->numChannels * sizeof(float) > 512ULL * 1024ULL * 1024ULL)
    {
        draft.error = "Sasha analysis needs finite mono/stereo audio at 8–192 kHz, below 512 MB decoded.";
        return draft;
    }
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    for (int offset = 0; offset < audio.getNumSamples(); offset += 65536)
    {
        if (cancelled.load(std::memory_order_relaxed))
            return draft;
        const auto count = juce::jmin(65536, audio.getNumSamples() - offset);
        if (! reader->read(&audio, offset, count, offset, true, true))
        {
            draft.error = "Could not decode audio for Sasha analysis.";
            return draft;
        }
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int i = offset; i < offset + count; ++i)
                if (! std::isfinite(audio.getSample(channel, i)))
                {
                    draft.error = "Audio contains non-finite samples.";
                    return draft;
                }
    }

    std::atomic<float> progress { 0.0f };
    auto analysis = native_annotation::NoteAnalysis::analyze(audio, reader->sampleRate, cancelled, progress);
    if (cancelled.load(std::memory_order_relaxed))
        return draft;
    auto prediction = model_.predict(audio, reader->sampleRate, cancelled, progress, &analysis);
    if (cancelled.load(std::memory_order_relaxed))
        return draft;
    if (prediction.probabilities.empty())
    {
        draft.error = "Sasha note inference failed.";
        return draft;
    }
    model_.apply(analysis, prediction);
    draft.succeeded = true;
    draft.duration = analysis.duration;
    draft.notes = std::move(analysis.notes);
    draft.modelId = model_.id();
    draft.modelName = model_.name();
    if (sibilantModelFile != juce::File())
    {
        if (sibilantModelFile != loadedSibilantModel_ || sibilantModelFile.getLastModificationTime() != sibilantModified_
            || sibilantModelFile.getSize() != sibilantSize_)
        {
            loadedSibilantModel_ = juce::File();
            if (const auto loaded = sibilantModel_.load(sibilantModelFile); loaded.failed())
            {
                draft.warning = loaded.getErrorMessage();
                return draft;
            }
            loadedSibilantModel_ = sibilantModelFile;
            sibilantModified_ = sibilantModelFile.getLastModificationTime();
            sibilantSize_ = sibilantModelFile.getSize();
        }
        const auto sibilants = sibilantModel_.predict(audio, reader->sampleRate, cancelled);
        if (cancelled.load(std::memory_order_relaxed)) return {};
        if (sibilants.probabilities.empty())
            draft.warning = "Sibilant inference failed; raw v11 intervals retained.";
        else
        {
            draft.sibilantModelId = sibilantModel_.id();
            draft.sibilantMerges = SibilantModel::mergeToNearest(draft.notes, sibilants.probabilities, sibilantModel_.detectionThreshold());
            draft.modelName += " + sibilants v1";
            for (auto& note : draft.notes)
                if (note.flags.contains("sibilant_attached")) note.flags.addIfNotAlreadyThere("sibilants:" + draft.sibilantModelId);
        }
    }
    return draft;
}

int SashaNoteAnalysis::attachPitchCurves(std::vector<NoteBlock>& notes,
                                        const std::vector<NoteBlock>& pitchNotes)
{
    std::vector<PitchCurvePoint> points;
    for (const auto& note : pitchNotes)
        for (const auto& point : note.curve)
            if (std::isfinite(point.time) && std::isfinite(point.midi) && std::isfinite(point.confidence)
                && point.midi >= 0.0 && point.midi <= 127.0 && point.confidence > 0.0)
                points.push_back(point);
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b)
    {
        return a.time < b.time;
    });

    int enriched = 0;
    for (auto& note : notes)
    {
        std::vector<PitchCurvePoint> curve;
        auto point = std::lower_bound(points.begin(), points.end(), note.start,
                                     [](const auto& value, double time) { return value.time < time; });
        for (; point != points.end() && point->time < note.end; ++point)
        {
            if (! curve.empty() && std::abs(curve.back().time - point->time) < 1.0e-6)
            {
                if (point->confidence > curve.back().confidence)
                    curve.back() = *point;
            }
            else
                curve.push_back(*point);
        }
        if (curve.empty())
            continue;
        note.curve = std::move(curve);
        note.voicedStart = note.curve.front().time;
        note.voicedEnd = juce::jmin(note.end, note.curve.back().time + 0.012);
        note.flags.removeString("pitch_uncertain");
        note.flags.add("pitch:rmvpe");
        ++enriched;
    }
    return enriched;
}

void SashaNoteAnalysis::applyParts(AnnotationDocument& document, Draft draft)
{
    if (! draft.succeeded)
        return;
    document.boundaries.erase(std::remove_if(document.boundaries.begin(), document.boundaries.end(),
        [](const auto& boundary)
        {
            const auto text = boundary.text.trim();
            if (text.isNotEmpty() && text != toString(boundary.kind))
                return false;
            return boundary.source.startsWith("sasha_notes:")
                || boundary.source.startsWith("boundary_tcn") || boundary.source.startsWith("gtsinger_tcn")
                || boundary.source == "energy_draft" || boundary.source == "pyin_draft";
        }), document.boundaries.end());
    document.regions.erase(std::remove_if(document.regions.begin(), document.regions.end(),
        [](const auto& region)
        {
            return region.kind == "breath" || region.kind == "noise" || region.kind == "pause"
                || region.kind == "slide_in" || region.kind == "slide_out";
        }), document.regions.end());
    document.notes = std::move(draft.notes);
    const auto addBoundary = [&](double time, BoundaryKind kind)
    {
        if (std::any_of(document.boundaries.begin(), document.boundaries.end(),
            [time](const auto& boundary) { return std::abs(boundary.time - time) < 0.001; }))
            return;
        BoundaryMarker boundary;
        boundary.id = document.nextBoundaryId();
        boundary.time = time;
        boundary.kind = kind;
        boundary.confidence = 0.55;
        boundary.source = "sasha_notes:" + draft.modelId;
        document.boundaries.push_back(std::move(boundary));
    };
    if (document.notes.empty() || document.notes.front().start > 0.0)
        addBoundary(0.0, BoundaryKind::pause);
    for (size_t i = 0; i < document.notes.size(); ++i)
    {
        const auto& note = document.notes[i];
        const auto connected = i > 0 && note.start - document.notes[i - 1].end <= 0.03;
        addBoundary(note.start, connected ? BoundaryKind::legato : BoundaryKind::syllable);
        if (i + 1 == document.notes.size() || document.notes[i + 1].start > note.end + 1.0e-6)
            addBoundary(note.end, BoundaryKind::pause);
    }
    std::stable_sort(document.boundaries.begin(), document.boundaries.end(),
                     [](const auto& a, const auto& b) { return a.time < b.time; });
}

SashaNoteAnalysis::Draft SashaNoteAnalysis::partsForPitch(const AnnotationDocument& document)
{
    Draft draft;
    const auto model = std::find_if(document.boundaries.begin(), document.boundaries.end(),
        [](const auto& boundary) { return boundary.source.startsWith("sasha_notes:"); });
    if (model == document.boundaries.end())
        return draft;
    draft.succeeded = true;
    draft.duration = document.duration;
    draft.modelId = model->source.fromFirstOccurrenceOf("sasha_notes:", false, false);
    draft.modelName = "AI Parts / Sasha v11";
    if (std::any_of(document.notes.begin(), document.notes.end(), [](const auto& note) { return note.flags.contains("sibilant_attached"); }))
        draft.modelName += " + sibilants v1";
    // Piano-roll edits change notes directly. Reconstructing them from annotation
    // markers would undo splits, joins, moves, deletions, and deliberate gaps.
    draft.notes = document.notes;
    return draft;
}

} // namespace vocal_annotation
