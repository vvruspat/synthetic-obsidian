#include "ArrangementPlayback.h"
#include "AnnotationJson.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace vocal_annotation;
namespace
{
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
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
}
int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("obsidian-arrangement-tests-" + juce::Uuid().toString());
    directory.createDirectory();
    try
    {
        const auto audio = makeAudio(directory);
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
        require(loaded.tracks.size() == 42, "no 26-track limit on restoration");
        require(loaded.tracks[1].collapsed && loaded.tracks[2].parentId == "other", "hierarchy and collapse persist");
        require(loaded.tracks[1].clips[0].audioFile == audio, "relative clip source resolves");
        require(arrangement::removeClip(loaded, "other", "one"), "delete clip");
        require(arrangement::notes(std::as_const(loaded), "other").empty(), "deleted clip cannot leave sounding notes");
        require(arrangement::removeTrack(loaded, "other") && loaded.tracks.size() == 1, "delete descendants only");
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
