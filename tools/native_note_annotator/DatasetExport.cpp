#include "DatasetExport.h"

#include "AnnotationJson.h"
#include "AnnotationValidator.h"

#include <cmath>
#include <set>

namespace native_annotation::DatasetExport
{
juce::Result validate(const vocal_annotation::AnnotationDocument& document)
{
    if (! std::isfinite(document.duration) || document.duration <= 0.0 || document.sampleRate <= 0)
        return juce::Result::fail("Invalid audio duration or sample rate.");
    std::set<juce::String> ids;
    for (const auto& note : document.notes)
    {
        if (note.id.isEmpty() || ! ids.insert(note.id).second)
            return juce::Result::fail("Note IDs must be unique and non-empty.");
        if (! std::isfinite(note.start) || ! std::isfinite(note.end)
            || ! std::isfinite(note.pitchExact) || ! std::isfinite(note.voicedStart) || ! std::isfinite(note.voicedEnd))
            return juce::Result::fail("A note contains a non-finite value.");
        for (const auto& point : note.curve)
            if (! std::isfinite(point.time) || ! std::isfinite(point.midi) || ! std::isfinite(point.confidence)
                || point.midi < 0.0 || point.midi > 127.0 || point.confidence < 0.0 || point.confidence > 1.0)
                return juce::Result::fail("Invalid pitch curve value.");
    }
    const auto issues = vocal_annotation::AnnotationValidator::validate(document);
    for (const auto& issue : issues)
        if (issue.isError)
            return juce::Result::fail(vocal_annotation::AnnotationValidator::summarize(issues));
    return juce::Result::ok();
}

juce::Result saveProject(const vocal_annotation::AnnotationDocument& document,
                         const DatasetMetadata& metadata, const juce::File& file)
{
    if (const auto result = validate(document); result.failed())
        return result;
    juce::TemporaryFile temporary(file);
    if (const auto result = vocal_annotation::AnnotationJson::save(document, temporary.getFile()); result.failed())
        return result;
    auto json = juce::JSON::parse(temporary.getFile());
    auto* root = json.getDynamicObject();
    if (root == nullptr)
        return juce::Result::fail("Could not serialize annotation.");
    auto* dataset = new juce::DynamicObject();
    dataset->setProperty("schema", "synthetic-obsidian-vocal-notes-1");
    dataset->setProperty("source_sha256", metadata.sourceHash);
    dataset->setProperty("audio_sha256", metadata.audioHash);
    dataset->setProperty("singer_id", metadata.singer);
    dataset->setProperty("session_id", metadata.session);
    dataset->setProperty("annotation_model_id", metadata.modelId);
    dataset->setProperty("annotation_model_sha256", metadata.modelHash);
    dataset->setProperty("review_status", metadata.reviewed ? "human_reviewed" : "draft");
    dataset->setProperty("time_unit", "seconds");
    dataset->setProperty("pitch_unit", "midi_semitones_a4_440");
    dataset->setProperty("boundary_policy", "musical_notes; vibrato_and_slides_remain_in_curve");
    root->setProperty("dataset", dataset);
    if (! temporary.getFile().replaceWithText(juce::JSON::toString(json, false))
        || ! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not save annotation JSON.");
    return juce::Result::ok();
}

DatasetMetadata readMetadata(const juce::File& file)
{
    DatasetMetadata metadata;
    const auto json = juce::JSON::parse(file);
    const auto dataset = json["dataset"];
    metadata.sourceHash = dataset["source_sha256"].toString();
    metadata.audioHash = dataset["audio_sha256"].toString();
    metadata.singer = dataset["singer_id"].toString();
    metadata.session = dataset["session_id"].toString();
    metadata.modelId = dataset["annotation_model_id"].toString();
    metadata.modelHash = dataset["annotation_model_sha256"].toString();
    metadata.reviewed = dataset["review_status"].toString() == "human_reviewed";
    return metadata;
}

juce::Result write(const vocal_annotation::AnnotationDocument& document,
                   const DatasetMetadata& metadata, const juce::AudioBuffer<float>& audio,
                   const juce::File& destination, juce::File& publishedDirectory)
{
    if (const auto result = validate(document); result.failed())
        return result;
    if (audio.getNumChannels() < 1 || audio.getNumSamples() < 1
        || std::abs(audio.getNumSamples() / static_cast<double>(document.sampleRate) - document.duration) > 1.0 / document.sampleRate)
        return juce::Result::fail("Audio and annotation durations do not match.");
    if (metadata.sourceHash.length() != 64 || ! metadata.sourceHash.containsOnly("0123456789abcdef"))
        return juce::Result::fail("Missing audio identity. Reopen the original audio.");
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const auto value = audio.getSample(channel, i);
            if (! std::isfinite(value) || std::abs(value) > 1.0f)
                return juce::Result::fail("PCM WAV export requires finite samples within -1..1. Adjust the source audio level first.");
        }
    if (const auto result = destination.createDirectory(); result.failed())
        return result;
    const auto stem = "vocal_" + metadata.sourceHash.substring(0, 24);
    const auto staging = destination.getChildFile(".vocal-export-" + juce::Uuid().toString());
    if (const auto result = staging.createDirectory(); result.failed())
        return result;
    const auto cleanup = juce::ScopeGuard([&] { staging.deleteRecursively(); });
    const auto audioFile = staging.getChildFile(stem + ".wav");
    auto stream = audioFile.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail("Could not create dataset audio.");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(), document.sampleRate,
        static_cast<unsigned int>(audio.getNumChannels()), 24, {}, 0));
    if (writer == nullptr)
        return juce::Result::fail("Could not create WAV writer.");
    stream.release(); // The writer owns the output stream after successful creation.
    if (! writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()) || ! writer->flush())
        return juce::Result::fail("Could not write dataset WAV.");
    writer.reset();

    auto exported = document;
    exported.audioFile = audioFile;
    auto exportedMetadata = metadata;
    exportedMetadata.audioHash = juce::SHA256(audioFile).toHexString();
    if (const auto result = saveProject(exported, exportedMetadata, staging.getChildFile(stem + ".annotation.json")); result.failed())
        return result;
    juce::String csv = "onset_seconds,offset_seconds,pitch_midi,pitch_hz,onset_sample,offset_sample\n";
    juce::String intervals;
    for (const auto& note : document.notes)
    {
        const auto hz = 440.0 * std::pow(2.0, (note.pitch - 69.0) / 12.0);
        csv << juce::String(note.start, 9) << ',' << juce::String(note.end, 9) << ','
            << note.pitch << ',' << juce::String(hz, 9) << ','
            << juce::String(static_cast<juce::int64>(std::llround(note.start * document.sampleRate))) << ','
            << juce::String(static_cast<juce::int64>(std::llround(note.end * document.sampleRate))) << '\n';
        intervals << juce::String(note.start, 9) << '\t' << juce::String(note.end, 9) << '\t' << juce::String(hz, 9) << '\n';
    }
    if (! staging.getChildFile(stem + ".notes.csv").replaceWithText(csv)
        || ! staging.getChildFile(stem + ".notes.txt").replaceWithText(intervals))
        return juce::Result::fail("Could not write note tables.");
    const auto target = destination.getNonexistentChildFile(stem, {}, true);
    if (! staging.moveFileTo(target))
        return juce::Result::fail("Could not publish dataset bundle.");
    publishedDirectory = target;
    return juce::Result::ok();
}
}
