#include "ArrangementPlayback.h"
#include "AnnotationJson.h"
#include "BackingVocalNotes.h"
#include "../../Source/web/AudioFileTransfer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

using namespace vocal_annotation;
namespace
{
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}
void testAudioFileTransfer(const juce::File& directory)
{
    using synthetic_obsidian::AudioFileTransfer;
    AudioFileTransfer transfer(directory.getChildFile("imports"));
    auto request = [](std::initializer_list<std::pair<juce::Identifier, juce::var>> fields)
    {
        auto* object = new juce::DynamicObject();
        for (const auto& [key, value] : fields) object->setProperty(key, value);
        return juce::var(object);
    };
    auto send = [&](std::initializer_list<std::pair<juce::Identifier, juce::var>> fields)
    { return transfer.process(request(fields)); };
    require(send({ { "action", "begin" }, { "name", "bad.exe" }, { "size", 4 } }).hasProperty("error"), "reject nonaudio drops");
    const auto token = send({ { "action", "begin" }, { "name", "take.wav" }, { "size", 4 } })["token"];
    require(token.toString().isNotEmpty(), "begin audio transfer");
    require(send({ { "action", "finish" }, { "token", token } }).hasProperty("error"), "cannot finish partial transfer");
    require(send({ { "action", "append" }, { "token", token }, { "offset", 1 }, { "data", "AAE=" } }).hasProperty("error"), "reject out of order chunks");
    require(static_cast<bool>(send({ { "action", "append" }, { "token", token }, { "offset", 0 }, { "data", "AAE=" } })["ok"]), "write first binary chunk");
    require(send({ { "action", "append" }, { "token", token }, { "offset", 0 }, { "data", "AAE=" } }).hasProperty("error"), "reject duplicate chunk");
    require(static_cast<bool>(send({ { "action", "append" }, { "token", token }, { "offset", 2 }, { "data", "Av8=" } })["ok"]), "write second binary chunk");
    const juce::File result(send({ { "action", "finish" }, { "token", token } })["path"].toString());
    juce::MemoryBlock data;
    const unsigned char expected[] { 0, 1, 2, 255 };
    require(result.loadFileAsData(data) && data.getSize() == 4 && std::memcmp(data.getData(), expected, 4) == 0, "transfer preserves binary file");
    require(result.getFileName() == "take.wav", "keep original clip name");
    const auto abort = send({ { "action", "begin" }, { "name", "cancel.wav" }, { "size", 4 } })["token"];
    require(static_cast<bool>(send({ { "action", "abort" }, { "token", abort } })["ok"]), "abort import");
    require(! directory.getChildFile("imports").getChildFile(abort.toString()).exists(), "abort removes partial assets");
}
juce::File makeAudio(const juce::File& directory)
{
    const auto file = directory.getChildFile("source.wav");
    auto stream = file.createOutputStream();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), 44100.0, 1, 24, {}, 0));
    require(writer != nullptr, "create fixture WAV");
    stream.release();
    juce::AudioBuffer<float> audio(1, 44100);
    for (int i = 0; i < audio.getNumSamples(); ++i) audio.setSample(0, i, i < 22050 ? 0.1f : 0.3f);
    require(writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()), "write fixture WAV");
    return file;
}
float sampleAt(const juce::File& file, double time)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    require(reader != nullptr, "read rendered WAV");
    juce::AudioBuffer<float> sample(2, 1);
    reader->read(&sample, 0, 1, static_cast<juce::int64>(time * reader->sampleRate), true, true);
    return sample.getSample(0, 0);
}
AnnotationDocument fixture(const juce::File& audio)
{
    AnnotationDocument document;
    document.hasArrangement = true;
    document.duration = 1.0;
    ArrangementTrack first;
    first.id = first.name = "Voice Main";
    first.notesMuted = true;
    first.clips.push_back({ "one", "First", audio, 0.25, 0.6, 0.2 });
    first.clips.push_back({ "two", "Second", audio, 0.7, 0.0, 0.2 });
    ArrangementTrack other;
    other.id = other.name = "other";
    document.tracks = { first, other };
    NoteBlock note;
    note.id = "note";
    note.clipId = "one";
    note.start = note.voicedStart = 0.26;
    note.end = note.voicedEnd = 0.4;
    note.curve = { { 0.26, 60, 1 }, { 0.4, 62, 1 } };
    document.notes.push_back(note);
    return document;
}
void testInstrumentalTrack(const juce::File& directory, const juce::File& audio)
{
    auto document = fixture(audio);
    document.instrumentalFile = audio;
    const auto json = directory.getChildFile("instrumental-project.json");
    require(AnnotationJson::save(document, json).wasOk(), "save project missing instrumental row");
    require(AnnotationJson::load(json, document).wasOk(), "restore missing instrumental row");
    require(document.tracks.size() == 3 && document.tracks.front().id == "Instrumental",
            "instrumental is the first track in an existing arrangement");
    auto& instrumental = document.tracks.front();
    require(instrumental.clips.size() == 1 && instrumental.clips.front().audioFile == audio,
            "recover the existing instrumental file when its row is missing");
    instrumental.clips.front().start = 3.0;
    instrumental.muted = true;
    instrumental.parentId = "other";
    std::rotate(document.tracks.begin(), std::next(document.tracks.begin()), document.tracks.end());
    require(AnnotationJson::save(document, json).wasOk(), "save displaced instrumental");
    require(AnnotationJson::load(json, document).wasOk(), "reload displaced instrumental");
    const auto& restored = document.tracks.front();
    require(document.tracks.size() == 3 && restored.id == "Instrumental" && restored.parentId.isEmpty(),
            "instrumental stays first and outside backing groups without duplication");
    require(restored.muted && std::abs(restored.clips.front().start - 3.0) < 0.00001,
            "restoring instrumental placement preserves its clips and mixer state");
    require(! arrangement::removeTrack(document, "Instrumental") && document.tracks.size() == 3,
            "instrumental track cannot be deleted");
    const auto clipId = restored.clips.front().id;
    require(arrangement::removeClip(document, "Instrumental", clipId), "instrumental clips can be deleted");
    require(AnnotationJson::save(document, json).wasOk(), "save empty instrumental");
    require(AnnotationJson::load(json, document).wasOk(), "reload empty instrumental");
    arrangement::migrate(document);
    require(document.tracks.size() == 3 && document.tracks.front().clips.empty(),
            "restoring instrumental never resurrects a deleted clip");
}
void testBackingPitchCurves(const juce::File& directory)
{
    NoteBlock lead;
    lead.id = "source-clip:lead";
    lead.start = lead.voicedStart = 1.0;
    lead.end = lead.voicedEnd = 2.0;
    lead.pitch = 60; lead.pitchExact = 60.2; lead.syllableId = "word-a";
    lead.curve = { {1.0, 60.2, 0.8}, {1.5, 60.5, 0.9}, {2.0, 59.85, 1.0} };
    auto other = lead;
    other.id = "other"; other.start = other.voicedStart = 3.0; other.end = other.voicedEnd = 4.0;
    other.pitch = 67; other.pitchExact = 67.1; other.syllableId = "word-b";
    other.curve = { {3.0, 67.1, 1.0}, {4.0, 66.8, 1.0} };
    const auto sources = leadNotesForBackingGeneration({lead, other});
    require(sources.size() == 2 && sources[0].curve.size() == 3, "retain lead contour before model generation");
    const auto response = juce::JSON::parse(R"({"notes":[
        {"id":"back-b","source_lead_ref":"lead_0002","start":3,"end":4,"midi_note":55},
        {"id":"back-a","source_lead_ref":"lead_0001","start":1,"end":2,"midi_note":64},
        {"id":"no-source","source_lead_ref":"missing","start":5,"duration":1,"midi_note":72}
    ]})");
    juce::String error;
    auto notes = parseBackingNotes(*response.getDynamicObject(), sources, error);
    require(error.isEmpty() && notes.size() == 3, "parse generated notes in arbitrary result order");
    require(std::abs(notes[0].curve[0].midi - 55.1) < 1e-9, "resolve normalized worker references independently of result order and document IDs");
    require(notes[1].curve.size() == 3 && std::abs(notes[1].curve[1].midi - 64.5) < 1e-9,
            "transpose lead expression relative to the generated note");
    require(std::abs(notes[1].curve[1].time - 1.5) < 1e-9 && std::abs(notes[1].curve[0].confidence - 0.8) < 1e-9,
            "inherit contour timing and confidence");
    require(notes[1].flags.contains("explicit_pitch_curve"), "inherited contour takes precedence in audio rendering");
    require(notes[2].end == 6.0 && notes[2].curve.size() == 2 && std::abs(notes[2].curve[0].midi - 72.0) < 1e-9
            && ! notes[2].flags.contains("explicit_pitch_curve"), "unknown sources keep the legacy flat fallback");
    require(lead.curve[1].midi == 60.5 && sources[0].curve[1].midi == 60.5, "generation does not mutate source curves");

    auto plateau = lead;
    plateau.id = "plateau"; plateau.start = plateau.voicedStart = 2.0; plateau.end = plateau.voicedEnd = 2.4;
    plateau.curve = { {2.1, 60.3, 1.0}, {2.4, 60.0, 1.0} };
    const auto merged = leadNotesForBackingGeneration({lead, plateau});
    require(merged.size() == 1 && merged[0].curve.size() == 5 && merged[0].curve.front().time == 1.0,
            "merging syllable plateaus retains both the initial and trailing contours");

    auto shiftedResponse = response.clone();
    auto* shifted = shiftedResponse["notes"][1].getDynamicObject();
    shifted->setProperty("start", 10.0); shifted->setProperty("end", 12.0);
    const auto shiftedNotes = parseBackingNotes(*shiftedResponse.getDynamicObject(), sources, error);
    require(shiftedNotes[1].curve[1].time == 11.0, "remap expression when target timing differs");

    auto sparse = lead;
    sparse.curve = { {1.5, 60.4, 0.7}, {0.5, 30.0, 1.0}, {1.7, std::numeric_limits<double>::quiet_NaN(), 1.0} };
    const auto sparseNotes = parseBackingNotes(*response.getDynamicObject(), leadNotesForBackingGeneration({sparse}), error);
    require(sparseNotes[1].curve.size() == 3 && sparseNotes[1].curve.front().time == 1.0
            && sparseNotes[1].curve.back().time == 2.0, "filter invalid/out-of-note points and extend sparse contours to boundaries");

    auto noisy = lead;
    noisy.curve.clear();
    for (int index = 0; index <= 100; ++index)
    {
        const auto time = 1.0 + static_cast<double>(index) * 0.01;
        const auto expression = 0.2 * (time - 1.0) + 0.35 * std::sin(2.0 * juce::MathConstants<double>::pi * 5.0 * time);
        const auto outlier = index == 0 || index == 100 || (index >= 30 && index <= 72);
        noisy.curve.push_back({time, outlier ? (index == 0 ? 40.8 : 71.213) : 60.0 + expression, 0.99});
    }
    const auto clean = leadNotesForBackingGeneration({noisy});
    require(clean[0].curve.size() == noisy.curve.size(), "repair detector dropouts without decimating vibrato");
    for (size_t index = 0; index < noisy.curve.size(); ++index)
    {
        const auto original = noisy.curve[index].midi;
        const auto repaired = clean[0].curve[index].midi;
        require(std::abs(repaired - 60.0) < 0.6, "high-confidence harmonic plateaus and boundary spikes are not expression");
        if (std::abs(original - 60.0) < 3.0)
            require(std::abs(repaired - original) < 1e-9, "preserve reliable drift and vibrato samples exactly");
    }
    require(noisy.curve[30].midi > 71.0, "cleaning does not rewrite the source analysis");
    noisy.flags.add("explicit_pitch_curve");
    require(leadNotesForBackingGeneration({noisy})[0].curve[30].midi > 71.0,
            "deliberately authored large pitch moves are not filtered");
    plateau.pitch = 72; plateau.pitchExact = 72.0;
    plateau.curve = { {2.1, 72.3, 1.0}, {2.4, 72.0, 1.0} };
    require(leadNotesForBackingGeneration({lead, plateau})[0].curve.back().midi > 71.0,
            "filter each source note before merging to preserve real melodic changes");
    auto unusable = lead;
    unusable.curve = { {1.0, 40.0, 0.99}, {2.0, 83.0, 0.99} };
    const auto fallback = parseBackingNotes(*response.getDynamicObject(), leadNotesForBackingGeneration({unusable}), error);
    require(!fallback[1].flags.contains("explicit_pitch_curve") && std::abs(fallback[1].curve[0].midi - 64.0) < 1e-9,
            "entirely unreliable source curves use the flat fallback");

    AnnotationDocument document;
    document.duration = 6.0;
    BackingVocalTrack track;
    track.id = "backing"; track.styleId = "THIRD_UP"; track.styleName = "Third Above";
    track.notes = notes;
    document.backingTracks.push_back(track);
    const auto file = directory.getChildFile("backing-curves.json");
    require(AnnotationJson::save(document, file).wasOk(), "save inherited backing contours");
    AnnotationDocument loaded;
    require(AnnotationJson::load(file, loaded).wasOk(), "load inherited backing contours");
    const auto& restored = loaded.backingTracks[0].notes[1];
    require(restored.curve.size() == 3 && std::abs(restored.curve[1].midi - 64.5) < 1e-9
            && restored.flags.contains("explicit_pitch_curve"), "project round trip preserves contour and render priority");
}

void testBackingPitchTransitions()
{
    const auto pair = []
    {
        std::vector<NoteBlock> notes(2);
        for (size_t index = 0; index < notes.size(); ++index)
        {
            auto& note = notes[index];
            note.id = "note-" + juce::String(static_cast<int>(index));
            note.clipId = "one-clip";
            note.start = note.voicedStart = static_cast<double>(index);
            note.end = note.voicedEnd = note.start + 1.0;
            note.pitch = 61 + static_cast<int>(index) * 2;
            note.pitchExact = static_cast<double>(note.pitch);
            note.flags.add("explicit_pitch_curve");
            for (int sample = 0; sample <= 100; ++sample)
            {
                const auto time = note.start + static_cast<double>(sample) * 0.01;
                auto offset = 0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 5.0 * time);
                if (index == 0 && sample >= 95) offset = -1.6;
                if (index == 1 && sample < 15) offset = 2.43 - static_cast<double>(sample) * 0.13;
                note.curve.push_back({time, note.pitchExact + offset, 0.95});
            }
        }
        return notes;
    };
    auto notes = pair();
    const auto original = notes;
    require(repairBackingPitchTransitions(notes) == 2, "repair opposing tail and attack excursions on an upward step");
    for (size_t index = 0; index < notes.size(); ++index)
    {
        const auto& note = notes[index];
        require(note.id == original[index].id && std::abs(note.start - original[index].start) < 1e-12
                    && std::abs(note.end - original[index].end) < 1e-12
                    && note.pitch == original[index].pitch, "boundary repair does not move or repitch notes");
        for (const auto& point : note.curve)
        {
            const auto distance = index == 0 ? note.end - point.time : point.time - note.start;
            if (distance >= 0.0 && distance <= 0.08)
                require(point.midi >= 60.0 - 1e-7 && point.midi <= 64.0 + 1e-7, "allow at most one semitone outside the transition corridor");
        }
        for (const auto& point : original[index].curve)
        {
            const auto distance = index == 0 ? note.end - point.time : point.time - note.start;
            if (distance < 0.22) continue;
            const auto found = std::find_if(note.curve.begin(), note.curve.end(), [&](const auto& value) { return std::abs(value.time - point.time) < 1e-9; });
            require(found != note.curve.end() && std::abs(found->midi - point.midi) < 1e-9, "sustained drift and vibrato are unchanged");
        }
    }
    require(repairBackingPitchTransitions(notes) == 0, "render retries do not accumulate boundary corrections");
    require(std::abs(notes[0].curve.back().midi - notes[1].curve.front().midi) < 1e-9,
            "a repaired connected boundary has no instantaneous F0 jump");
    const auto shoulder = std::find_if(notes[1].curve.begin(), notes[1].curve.end(), [](const auto& point)
    { return std::abs(point.time - 1.10) < 1e-9; });
    require(shoulder != notes[1].curve.end() && shoulder->midi > 63.0 && shoulder->midi < 63.8,
            "reduce the excursion proportionally instead of clipping the attack to a flat ceiling");
    auto lastPitch = -1.0;
    for (const auto& note : notes)
        for (const auto& point : note.curve)
            if (point.time >= 0.92 && point.time <= 1.08)
            {
                require(point.midi >= lastPitch - 1e-7, "the repaired upward transition is monotonic through the boundary");
                lastPitch = point.midi;
            }

    auto down = pair();
    for (auto& note : down)
    {
        note.pitchExact = 124.0 - note.pitchExact;
        note.pitch = juce::roundToInt(note.pitchExact);
        for (auto& point : note.curve) point.midi = 124.0 - point.midi;
    }
    require(repairBackingPitchTransitions(down) == 2, "descending steps have symmetric limits");
    for (size_t index = 0; index < notes.size(); ++index)
    {
        require(down[index].curve.size() == notes[index].curve.size(), "symmetric repairs keep sampling geometry");
        for (size_t sample = 0; sample < notes[index].curve.size(); ++sample)
            require(std::abs(down[index].curve[sample].midi - (124.0 - notes[index].curve[sample].midi)) < 1e-8,
                    "repair direction follows the generated melody");
    }
    auto gentle = pair();
    for (auto& note : gentle)
    {
        note.pitch = 61; note.pitchExact = 61.0;
        for (auto& point : note.curve)
            point.midi = note.pitchExact + 0.99 * std::sin(2.0 * juce::MathConstants<double>::pi * 5.0 * point.time);
    }
    require(repairBackingPitchTransitions(gentle) == 0, "preserve an already continuous pitch curve and normal vibrato exactly");
    auto cleanStep = pair();
    for (auto& note : cleanStep)
        for (auto& point : note.curve)
            point.midi = note.pitchExact + 0.3 * std::sin(2.0 * juce::MathConstants<double>::pi * 5.0 * point.time);
    require(repairBackingPitchTransitions(cleanStep) == 2, "join adjacent notes even without an excessive excursion");
    require(std::abs(cleanStep[0].curve.back().midi - cleanStep[1].curve.front().midi) < 1e-9,
            "ordinary note steps also have a shared continuous F0 curve");
    require(repairBackingPitchTransitions(cleanStep) == 0, "ordinary smooth joins remain stable on render retries");

    auto disconnected = pair();
    disconnected[1].start += 0.2;
    require(repairBackingPitchTransitions(disconnected) == 0, "do not connect across a pause");
    disconnected = pair(); disconnected[1].start -= 0.2;
    require(repairBackingPitchTransitions(disconnected) == 0, "do not connect overlapping voices");
    disconnected = pair(); disconnected[1].clipId = "another-clip";
    require(repairBackingPitchTransitions(disconnected) == 0, "do not connect independent clips");

    auto shortNotes = pair();
    for (auto& note : shortNotes)
    {
        const auto start = note.start;
        note.start = note.voicedStart = start * 0.08;
        note.end = note.voicedEnd = note.start + 0.08;
        for (auto& point : note.curve) point.time *= 0.08;
    }
    require(repairBackingPitchTransitions(shortNotes) == 2, "short notes get bounded non-overlapping edge windows");
    require(repairBackingPitchTransitions(shortNotes) == 0, "short-note repairs are stable on repeated renders");
    auto sparse = pair();
    for (auto& point : sparse[0].curve) point.midi = sparse[0].pitchExact;
    sparse[1].curve = {{1.0, 63.2, 1.0}, {1.07, 63.5, 1.0}, {1.09, 65.0, 1.0}, {2.0, 63.0, 1.0}};
    require(repairBackingPitchTransitions(sparse) == 2, "detect an interpolated excursion at the guard edge of a sparse contour");
    require(repairBackingPitchTransitions(sparse) == 0, "interpolated guard extrema are included in proportional repair");
}

void testNativeNoteTransform()
{
    NoteBlock note;
    note.id = "precise-id"; note.clipId = "audio-clip";
    note.start = 1; note.end = 2; note.voicedStart = 1.1; note.voicedEnd = 1.9;
    note.pitchExact = 60.25; note.pitch = 60; note.sourcePitchExact = 59.8;
    note.curve = { {1, 60.1, 0.8}, {1.5, 60.4, 0.9}, {2, 60.2, 1} };
    std::vector<AnnotationRegion> regions { {1.2, 1.4, "voiced", note.id}, {5, 6, "noise", "other"} };
    arrangement::transformNote(note, 3, 5, 61.25, &regions);
    require(note.id == "precise-id" && note.clipId == "audio-clip", "native drag preserves source identity");
    require(std::abs(note.pitchExact - 61.25) < 0.00001 && std::abs(note.sourcePitchExact - 59.8) < 0.00001, "native drag preserves fractional/source pitch");
    require(std::abs(note.curve[1].time - 4) < 0.00001 && std::abs(note.curve[1].midi - 61.4) < 0.00001, "stretch remaps curve and pitch together");
    require(std::abs(note.voicedStart - 3.2) < 0.00001 && std::abs(note.voicedEnd - 4.8) < 0.00001, "stretch retains voiced bounds");
    require(std::abs(regions[0].start - 3.4) < 0.00001 && std::abs(regions[0].end - 3.8) < 0.00001 && std::abs(regions[1].start - 5) < 0.00001, "stretch remaps only matching regions");
    std::vector<NoteBlock> source { note };
    const auto originalStart = source[0].start, originalEnd = source[0].end;
    const std::vector<arrangement::NoteDelta> fragments {
        { note.id, 0, 0, 62.25 }, { note.id, 0, 0, 62.25 }
    };
    arrangement::transformNotes(source, fragments, 12, &regions);
    require(std::abs(source[0].start - originalStart) < 0.00001 && std::abs(source[0].end - originalEnd) < 0.00001, "pitch drag of voiced fragments keeps full source bounds");
    const std::vector<arrangement::NoteDelta> movedFragments {
        { note.id, 1, 1, 63.25 }, { note.id, 1, 1, 63.25 }
    };
    arrangement::transformNotes(source, movedFragments, 12, &regions);
    require(std::abs(source[0].start - originalStart - 1) < 0.00001 && std::abs(source[0].end - originalEnd - 1) < 0.00001,
            "multiple displayed fragments transform one source only once");

}
void testNoteGainPlayback(const juce::File& audio)
{
    auto document = fixture(audio);
    document.duration = 84.0;
    document.tracks.front().clips = {
        { "one", "First", audio, 81.5, 0.0, 0.5 },
        { "two", "Second", audio, 81.5, 0.0, 0.5 },
        { "pitch-preview", "Preview", audio, 82.0, 0.0, 0.5 }
    };
    document.notes.clear();
    auto addGain = [&](const char* clipId, double start, double end, double gainDb)
    {
        NoteBlock note;
        note.clipId = clipId;
        note.start = note.voicedStart = start;
        note.end = note.voicedEnd = end;
        note.gainDb = gainDb;
        document.notes.push_back(note);
    };
    const auto doubleGainDb = 20.0 * std::log10(2.0);
    addGain("one", 81.52, 81.98, doubleGainDb);
    addGain("one", 81.55, 81.60, -doubleGainDb);
    addGain("two", 81.60, 81.80, -doubleGainDb);
    addGain("one", 81.70, 81.90, doubleGainDb);
    addGain("unrelated", 82.10, 82.30, doubleGainDb);

    ArrangementPlayback playback;
    playback.prepare(48000.0);
    playback.request(document);
    playback.setPlaying(true);
    juce::AudioBuffer<float> block(2, 1024);
    auto sample = [&](double time)
    {
        block.clear();
        playback.seek(time);
        playback.process({ &block, 0, 64 });
        return block.getSample(0, 0);
    };
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (sample(81.65) > 0.0001f) break;
        juce::Thread::sleep(5);
    }
    require(std::abs(sample(81.65) - 0.25f) < 0.0001f,
            "late playback keeps a long gain after a nested gain ends, separately for each clip");
    require(std::abs(sample(81.75) - 0.45f) < 0.0001f, "overlapping note gains multiply");
    require(std::abs(sample(81.99) - 0.20f) < 0.0001f, "expired gains do not affect later audio");
    require(std::abs(sample(81.57) - 0.20f) < 0.0001f, "backward seek restores nested gains");
    require(std::abs(sample(81.5225) - 0.25f) < 0.0001f, "note gain retains its five millisecond fade");
    require(std::abs(sample(82.15) - 0.20f) < 0.0001f, "pitch preview applies gains across clip IDs");

    playback.setLoop(true, 81.65, 81.75);
    playback.seek(81.745);
    block.clear();
    playback.process({ &block, 0, 1024 });
    require(std::abs(block.getSample(0, 0) - 0.45f) < 0.0001f
                && std::abs(block.getSample(0, 1023) - 0.25f) < 0.0001f,
            "gain lookup resets correctly when a loop wraps within an audio block");
}
}
int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("obsidian-arrangement-tests-" + juce::Uuid().toString());
    directory.createDirectory();
    try
    {
        testBackingPitchCurves(directory);
        testBackingPitchTransitions();
        testNativeNoteTransform();
        testAudioFileTransfer(directory);
        const auto audio = makeAudio(directory);
        testInstrumentalTrack(directory, audio);
        testNoteGainPlayback(audio);
        auto document = fixture(audio);
        const auto render = directory.getChildFile("render.wav");
        require(ArrangementPlayback::renderTrack(document, "Voice Main", render).wasOk(), "render arrangement");
        require(std::abs(sampleAt(render, 0.1)) < 0.0001f, "gap is silent");
        require(std::abs(sampleAt(render, 0.3) - 0.3f) < 0.001f, "source offset and sample-rate conversion");
        require(std::abs(sampleAt(render, 0.6)) < 0.0001f, "gap between clips is silent");
        require(std::abs(sampleAt(render, 0.8) - 0.1f) < 0.001f, "second clip plays independently");
        require(arrangement::moveClip(document, "Voice Main", "one", "other", 1.5), "move across tracks");
        require(document.notes.empty(), "source notes removed on move");
        const auto& movedNotes = arrangement::notes(std::as_const(document), "other");
        require(movedNotes.size() == 1 && std::abs(movedNotes[0].start - 1.51) < 0.00001, "notes move with audio");
        require(std::abs(movedNotes[0].curve[1].time - 1.65) < 0.00001, "pitch curve moves with audio");
        require(std::abs(arrangement::duration(document) - 1.7) < 0.00001, "moving beyond end extends project");
        require(! arrangement::moveClip(document, "other", "one", "missing", 0.0), "reject nonexistent destination");
        require(! arrangement::moveClip(document, "other", "one", "other", std::numeric_limits<double>::quiet_NaN()), "reject NaN placement");
        require(ArrangementPlayback::renderTrack(document, "other", render).wasOk(), "render moved clip");
        require(std::abs(sampleAt(render, 1.55) - 0.3f) < 0.001f, "export respects moved position");
        for (int i = 0; i < 40; ++i)
        {
            ArrangementTrack child;
            child.id = "child-" + juce::String(i);
            child.name = "Third Above";
            child.parentId = "other";
            document.tracks.push_back(child);
        }
        document.tracks[1].collapsed = true;
        const auto json = directory.getChildFile("project.json");
        require(AnnotationJson::save(document, json).wasOk(), "save project");
        require(json.loadFileAsString().contains("\"audio\": \"source.wav\""), "portable relative clip paths");
        AnnotationDocument loaded;
        require(AnnotationJson::load(json, loaded).wasOk(), "load project");
        require(loaded.tracks.size() == 43 && loaded.tracks.front().id == "Instrumental", "restore instrumental without limiting track count");
        require(loaded.tracks[2].collapsed && loaded.tracks[3].parentId == "other", "hierarchy and collapse persist");
        require(loaded.tracks[2].clips[0].audioFile == audio, "relative clip source resolves");
        require(arrangement::removeClip(loaded, "other", "one"), "delete clip");
        require(arrangement::notes(std::as_const(loaded), "other").empty(), "deleted clip cannot leave sounding notes");
        require(arrangement::removeTrack(loaded, "other") && loaded.tracks.size() == 2, "delete descendants only");
        auto invalid = arrangement::toJson(document, directory);
        invalid.getArray()->getReference(1).getDynamicObject()->setProperty("parentId", "child-0");
        require(arrangement::fromJson(invalid, loaded, directory).failed(), "reject cyclic project hierarchy");
        AnnotationDocument legacy;
        legacy.audioFile = audio;
        legacy.duration = 1.0;
        legacy.notes = fixture(audio).notes;
        legacy.notes[0].clipId.clear();
        arrangement::migrate(legacy);
        require(legacy.hasArrangement && legacy.tracks.size() == 2 && legacy.tracks[1].clips.size() == 1, "migrate legacy vocal");
        require(! legacy.notes[0].clipId.isEmpty(), "attach legacy notes to clip");
        arrangement::removeClip(legacy, "Voice Main", legacy.tracks[1].clips[0].id);
        arrangement::migrate(legacy);
        require(legacy.tracks[1].clips.empty(), "migration cannot resurrect deleted clips");
        BackingVocalTrack backing;
        backing.id = "generated";
        backing.audioFile = audio;
        legacy.backingTracks.push_back(backing);
        arrangement::syncGeneratedTracks(legacy);
        const auto generatedClip = arrangement::findTrack(legacy, "generated")->clips.front().id;
        arrangement::removeClip(legacy, "generated", generatedClip);
        arrangement::syncGeneratedTracks(legacy);
        require(arrangement::findTrack(legacy, "generated")->clips.empty(), "sync cannot resurrect a deleted generated clip");
        // Exercise the actual callback at different device rates/block sizes, with more than 26 tracks.
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            ArrangementPlayback playback;
            auto many = fixture(audio);
            many.tracks.clear();
            for (int i = 0; i < 40; ++i)
            {
                auto track = fixture(audio).tracks[0];
                track.id = juce::String(i);
                track.clips.resize(1);
                track.clips[0].start = 0.0;
                many.tracks.push_back(std::move(track));
            }
            playback.prepare(rate);
            playback.request(many);
            juce::AudioBuffer<float> block(2, 1024);
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                block.clear(); playback.seek(0.05); playback.setPlaying(true);
                playback.process({ &block, 0, 128 });
                if (block.getMagnitude(0, 128) > 0.1f) break;
                juce::Thread::sleep(5);
            }
            require(std::abs(block.getSample(0, 64) - 12.0f) < 0.02f, "all 40 tracks audible at device rate");
            for (int count : { 64, 128, 256, 512, 1024 })
            {
                block.clear(); playback.seek(0.05); playback.process({ &block, 0, count });
                require(std::isfinite(block.getSample(0, count - 1)), "finite output across block sizes");
            }
            playback.setPlaying(false);
            block.clear(); playback.process({ &block, 0, 1024 });
            require(block.getMagnitude(0, 1024) == 0.0f, "pause is silent");
        }
        std::cout << "Arrangement persistence, hierarchy, edits, exports and playback passed\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        directory.deleteRecursively();
        return 1;
    }
    directory.deleteRecursively();
    return 0;
}
