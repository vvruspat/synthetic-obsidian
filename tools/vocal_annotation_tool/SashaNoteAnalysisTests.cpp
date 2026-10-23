#include "SashaNoteAnalysis.h"
#include "AnnotationJson.h"
#include "RuntimeManager.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace vocal_annotation;

void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}

void requireCurrentParts(const AnnotationDocument& document)
{
    const auto parts = SashaNoteAnalysis::partsForPitch(document);
    require(parts.succeeded && parts.notes.size() == document.notes.size(),
            "Pitch analysis must keep the current edited note count");
    for (size_t i = 0; i < document.notes.size(); ++i)
    {
        const auto& original = document.notes[i];
        const auto& result = parts.notes[i];
        require(result.id == original.id && std::abs(result.start - original.start) < 1.0e-12
                    && std::abs(result.end - original.end) < 1.0e-12
                    && result.lyric == original.lyric && std::abs(result.gainDb - original.gainDb) < 1.0e-12,
                "Pitch analysis must preserve note IDs, timing, and manual annotations exactly");
    }
}

void checks()
{
    {
        const std::vector<float> p { 0.0f, 0.49f, 0.0f, 0.95f, 0.98f, 0.0f, 0.8f };
        const auto ranges = SibilantModel::detectedRanges(p, 0.48f, 0.065);
        require(ranges.size() == 3 && std::abs(ranges[0].start - 0.01) < 1e-12
                    && std::abs(ranges[0].end - 0.02) < 1e-12,
                "The overlay must keep an isolated 10 ms detection below the attachment threshold");
        require(std::abs(ranges[1].start - 0.03) < 1e-12 && std::abs(ranges[1].end - 0.05) < 1e-12
                    && std::abs(ranges[2].end - 0.065) < 1e-12,
                "The overlay must preserve negative gaps and clip the final positive frame to audio duration");
        require(SibilantModel::detectedRanges({}, 0.48f, 1.0).empty()
                    && SibilantModel::detectedRanges({ std::numeric_limits<float>::quiet_NaN() }, 0.48f, 1.0).empty(),
                "Empty or invalid overlay predictions must not produce highlights");
        std::atomic<bool> cancelled { true };
        const auto inspection = SibilantModel::inspectFile({}, {}, cancelled);
        require(inspection.ranges.empty() && inspection.error.isEmpty(), "Cancelled overlays must stop without errors");
        cancelled.store(false);
        require(SibilantModel::inspectFile({}, {}, cancelled).error.isNotEmpty(), "Unavailable overlay models must report an error");
    }
    {
        std::vector<NoteBlock> notes(3);
        notes[0].id = "left"; notes[0].start = 0.0; notes[0].end = 0.4; notes[0].pitch = 42;
        notes[1].id = "ess"; notes[1].start = 0.4; notes[1].end = 0.6;
        notes[2].id = "right"; notes[2].start = 0.6; notes[2].end = 1.0;
        const auto original = notes;
        std::vector<float> p(100, 0.0f);
        std::fill(p.begin() + 40, p.begin() + 60, 1.0f);
        require(SibilantModel::mergeToNearest(notes, p, 0.89f) == 1, "Whole sibilants must merge");
        require(notes.size() == 2 && notes[0].id == "left" && notes[0].pitch == 42
                    && std::abs(notes[0].end - 0.6) < 1e-9 && notes[0].flags.contains("sibilant_attached"),
                "A tie must prefer the previous note and preserve its pitch and the full sound interval");
        notes = original; notes[0].end = 0.30; notes[2].start = 0.61;
        require(SibilantModel::mergeToNearest(notes, p, 0.89f) == 1 && notes[1].id == "right"
                    && std::abs(notes[1].start - 0.4) < 1e-9, "A closer following note must receive the sibilant attack");
        notes = original; std::fill(p.begin() + 50, p.begin() + 60, 0.0f);
        require(SibilantModel::mergeToNearest(notes, p, 0.89f) == 0 && notes.size() == 3,
                "Exactly 50 percent sibilant time must retain a note");
        notes = original; std::fill(p.begin(), p.end(), 0.0f);
        std::fill(p.begin() + 40, p.begin() + 51, 0.6f);
        require(SibilantModel::mergeToNearest(notes, p, 0.48f) == 1 && notes[0].id == "left",
                "A note with 55 percent white time must attach even below the old 0.89 threshold");
        notes = original;
        for (int i = 52; i < 60; ++i) notes[1].curve.push_back({ i / 100.0, 60.0 + (i - 52) * 0.5, 0.95 });
        require(SibilantModel::hasVoicedCore(notes[1], p, 0.48f)
                    && SibilantModel::mergeToNearest(notes, p, 0.48f) == 0 && notes.size() == 3,
                "A periodic vocal core must protect a majority-sibilant note, including moving pitch");
        for (auto& point : notes[1].curve) point.confidence = 0.6;
        require(! SibilantModel::hasVoicedCore(notes[1], p, 0.48f)
                    && SibilantModel::mergeToNearest(notes, p, 0.48f) == 1,
                "Unreliable pitch observations alone must not protect a sibilant fragment");
        notes = original; notes[1].end = notes[2].start = 0.5;
        std::fill(p.begin(), p.end(), 0.0f); std::fill(p.begin() + 40, p.begin() + 46, 0.6f);
        for (int i = 46; i < 50; ++i) notes[1].curve.push_back({ i / 100.0, 60.0, 0.95 });
        require(SibilantModel::mergeToNearest(notes, p, 0.48f) == 0,
                "Short notes must retain a proportionally shorter voiced core");
        std::fill(p.begin() + 46, p.begin() + 50, 0.6f);
        require(SibilantModel::mergeToNearest(notes, p, 0.48f) == 1,
                "Pitch observations fully inside sibilants must not create a false vocal core");
        notes = original; notes[0].end = 0.4; notes[1].start = 0.409; notes[1].end = 0.421; notes[2].start = 0.421;
        std::fill(p.begin(), p.end(), 0.0f); p[41] = 0.6f;
        require(SibilantModel::mergeToNearest(notes, p, 0.48f) == 1 && notes[1].id == "right",
                "Partial edge frames must be measured by actual overlap duration");
        notes = original; std::fill(p.begin(), p.end(), 1.0f);
        require(SibilantModel::mergeToNearest(notes, p, 0.89f) == 0, "A clip containing only sibilants must retain its notes");
        std::fill(p.begin(), p.end(), 0.0f); std::fill(p.begin() + 40, p.begin() + 60, 1.0f);
        notes = { original[0], original[1] }; notes[0].end = 0.1;
        require(SibilantModel::mergeToNearest(notes, p, 0.89f) == 0, "A long pause must not be bridged");
        p[40] = std::numeric_limits<float>::quiet_NaN();
        require(SibilantModel::mergeToNearest(notes, p, 0.89f) == 0, "Invalid predictions must not edit notes");
        SibilantModel missing;
        require(missing.load({}).failed(), "Missing sibilant weights must fail safely");
    }
    SashaNoteAnalysis analyzer;
    std::atomic<bool> cancelled { true };
    require(! analyzer.analyze({}, {}, cancelled).succeeded, "Cancellation must not return a draft");
    cancelled.store(false);
    require(! analyzer.analyze({}, {}, cancelled).succeeded, "Missing models must fail without modifying notes");

    SashaNoteAnalysis::Draft draft;
    draft.succeeded = true;
    draft.modelId = "test-model";
    draft.duration = 1.5;
    NoteBlock first;
    first.id = "n1"; first.start = 0.1; first.end = 0.4; first.pitch = 42;
    first.flags.add("pitch_uncertain");
    auto second = first; second.id = "n2"; second.start = 0.4; second.end = 0.7;
    auto third = first; third.id = "n3"; third.start = 1.0; third.end = 1.2;
    draft.notes = { first, second, third };
    AnnotationDocument document;
    document.duration = 1.5;
    BoundaryMarker lyric;
    lyric.id = "manual"; lyric.time = 0.1; lyric.text = "ah";
    document.boundaries.push_back(lyric);
    SashaNoteAnalysis::applyParts(document, draft);
    require(document.notes.size() == 3, "AI Parts must retain pitch-uncertain notes");
    auto parts = SashaNoteAnalysis::partsForPitch(document);
    require(parts.succeeded && parts.notes.size() == 3, "AI Parts must preserve pauses and connected transitions");
    for (size_t i = 0; i < draft.notes.size(); ++i)
        require(std::abs(parts.notes[i].start - draft.notes[i].start) < 1e-9
                && std::abs(parts.notes[i].end - draft.notes[i].end) < 1e-9,
                "Analyze must use the exact AI Parts boundaries");
    require(std::any_of(document.boundaries.begin(), document.boundaries.end(), [](const auto& boundary)
                { return boundary.id == "manual" && boundary.text == "ah"; }),
            "AI Parts must retain existing lyric text");
    const auto inputSignature = SashaNoteAnalysis::inputSignature(document);
    document.bpm = 94.0;
    document.key = "Bb minor";
    document.chords.push_back({ 0.0, 1.5, "Bbm", 0.9 });
    require(SashaNoteAnalysis::inputSignature(document) == inputSignature,
            "Concurrent musical-context completion must not discard vocal analysis");
    document.backingNotes.push_back(first);
    require(SashaNoteAnalysis::inputSignature(document) != inputSignature,
            "A concurrently created backing track must be protected from stale analysis");
    document.backingNotes.clear();
    for (auto& boundary : document.boundaries)
        if (std::abs(boundary.time - 0.4) < 1e-9) boundary.time = 0.45;
    require(SashaNoteAnalysis::inputSignature(document) != inputSignature,
            "Manual vocal edits must still invalidate stale analysis");
    requireCurrentParts(document);
    document.notes[0].end = 0.45;
    document.notes[1].start = 0.45;
    document.notes[1].lyric = "ah";
    parts = SashaNoteAnalysis::partsForPitch(document);
    require(std::abs(parts.notes[0].end - 0.45) < 1e-9 && std::abs(parts.notes[1].start - 0.45) < 1e-9,
            "Manual splitter changes must remain authoritative for pitch analysis");

    NoteBlock pitch;
    pitch.curve = { { 0.2, 42.0, 0.8 }, { 0.45, 43.0, 0.7 }, { 0.5, 43.1, 0.6 },
                    { 0.5, 43.2, 0.9 }, { 0.55, std::numeric_limits<double>::quiet_NaN(), 1.0 } };
    require(SashaNoteAnalysis::attachPitchCurves(parts.notes, { pitch }) == 2, "Pitch should enrich only supported parts");
    require(parts.notes.size() == 3 && parts.notes[2].flags.contains("pitch_uncertain"),
            "No pitch estimate must not delete an AI part");
    require(parts.notes[1].curve.size() == 2 && std::abs(parts.notes[1].curve[0].time - 0.45) < 1e-9
            && std::abs(parts.notes[1].curve[1].midi - 43.2) < 1e-9, "Pitch points must be clipped and deduplicated per part");
    require(std::abs(parts.notes[0].end - 0.45) < 1e-9 && std::abs(parts.notes[1].end - 0.7) < 1e-9,
            "RMVPE enrichment must not change boundaries");

    // The visible piano roll edits note blocks, leaving annotation markers unchanged.
    auto edited = document;
    auto right = edited.notes[1];
    right.id = "manual-split";
    right.start = 0.55;
    edited.notes[1].end = right.start;
    edited.notes.insert(edited.notes.begin() + 2, right);
    requireCurrentParts(edited);
    edited.notes[1].end = edited.notes[2].end;
    edited.notes.erase(edited.notes.begin() + 2);
    requireCurrentParts(edited);
    edited.notes.erase(edited.notes.begin());
    requireCurrentParts(edited);
    edited.notes.front().start = 0.47;
    edited.notes.front().end = 0.515;
    edited.notes.front().gainDb = -3.0;
    requireCurrentParts(edited);
    edited.notes.clear();
    requireCurrentParts(edited);

    const auto notesBeforeFailure = document.notes.size();
    SashaNoteAnalysis::applyParts(document, {});
    require(document.notes.size() == notesBeforeFailure, "Failed AI Parts must preserve existing work");
    draft.notes.clear();
    document.boundaries.clear();
    SashaNoteAnalysis::applyParts(document, draft);
    require(document.notes.empty() && SashaNoteAnalysis::partsForPitch(document).notes.empty(),
            "A successful empty AI draft must stay empty instead of falling back to old notes");
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    try
    {
        checks();
        if (argc == 3 && juce::String::fromUTF8(argv[1]) == "--check-pitch-input")
        {
            AnnotationDocument document;
            const auto input = juce::File(juce::String::fromUTF8(argv[2]));
            const auto loaded = AnnotationJson::load(input, document);
            require(loaded.wasOk(), loaded.getErrorMessage().toRawUTF8());
            requireCurrentParts(document);
            std::cout << "Saved session: " << document.notes.size()
                      << " note intervals and IDs preserved exactly for RMVPE\n";
        }
        if (argc == 5 && juce::String::fromUTF8(argv[1]) == "--sibilants")
        {
            SibilantModel model;
            const auto loaded = model.load(juce::File(juce::String::fromUTF8(argv[2])));
            require(loaded.wasOk(), loaded.getErrorMessage().toRawUTF8());
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(juce::String::fromUTF8(argv[3]))));
            require(reader != nullptr && reader->lengthInSamples > 0 && reader->lengthInSamples < 100000000,
                    "Invalid sibilant parity input");
            juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
            require(reader->read(&audio, 0, audio.getNumSamples(), 0, true, true), "Cannot decode parity input");
            std::atomic<bool> cancelled { false };
            const auto prediction = model.predict(audio, reader->sampleRate, cancelled, true);
            require(! prediction.probabilities.empty(), "Sibilant prediction failed");
            auto* data = new juce::DynamicObject();
            juce::Array<juce::var> probabilities, features;
            for (auto v : prediction.probabilities) probabilities.add(v);
            for (auto v : prediction.features) features.add(v);
            data->setProperty("probabilities", probabilities); data->setProperty("features", features);
            data->setProperty("model_id", model.id()); data->setProperty("threshold", model.threshold());
            require(juce::File(juce::String::fromUTF8(argv[4])).replaceWithText(juce::JSON::toString(juce::var(data), true)),
                    "Cannot write sibilant parity output");
        }
        if (argc == 5 && juce::String::fromUTF8(argv[1]) == "--inspect-sibilants")
        {
            std::atomic<bool> cancelled { false };
            const auto result = SibilantModel::inspectFile(juce::File(juce::String::fromUTF8(argv[3])),
                                                         juce::File(juce::String::fromUTF8(argv[2])), cancelled);
            require(result.error.isEmpty(), result.error.toRawUTF8());
            juce::Array<juce::var> ranges;
            for (const auto& range : result.ranges)
            {
                auto* value = new juce::DynamicObject();
                value->setProperty("start", range.start); value->setProperty("end", range.end);
                ranges.add(value);
            }
            require(juce::File(juce::String::fromUTF8(argv[4])).replaceWithText(juce::JSON::toString(ranges, true)),
                    "Cannot write sibilant overlay output");
            std::cout << result.ranges.size() << " sibilant overlay regions\n";
        }
        if (argc == 4 || (argc == 5 && ! juce::String::fromUTF8(argv[1]).startsWith("--")))
        {
            const auto model = juce::File(juce::String::fromUTF8(argv[1]));
            const auto audio = juce::File(juce::String::fromUTF8(argv[2]));
            const auto output = juce::File(juce::String::fromUTF8(argv[3]));
            SashaNoteAnalysis analyzer;
            std::atomic<bool> cancelled { false };
            auto draft = analyzer.analyze(audio, model, cancelled,
                argc == 5 ? juce::File(juce::String::fromUTF8(argv[4])) : juce::File());
            require(draft.succeeded, draft.error.toRawUTF8());
            AnnotationDocument document;
            document.audioFile = audio;
            document.duration = draft.duration;
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audio));
            document.sampleRate = static_cast<int>(reader->sampleRate);
            const auto modelId = draft.modelId;
            SashaNoteAnalysis::applyParts(document, std::move(draft));
            auto parts = SashaNoteAnalysis::partsForPitch(document);
            require(parts.notes.size() == document.notes.size(), "AI Parts round trip lost notes");
            requireCurrentParts(document);
            require(AnnotationJson::save(document, output).wasOk(), "Could not save integration fixture");
            std::cout << modelId.toStdString() << ": " << document.notes.size() << " AI parts; default model "
                      << RuntimeManager::noteBoundaryModelFile().getFullPathName().toStdString() << '\n';
        }
        std::cout << "Sasha AI Parts checks passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
