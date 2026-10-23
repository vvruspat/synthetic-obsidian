#include "DatasetExport.h"
#include "NoteEditing.h"
#include "NoteBoundaryModel.h"
#include "PitchStructureFeatures.h"

#include "AnnotationJson.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}

juce::AudioBuffer<float> fixture(double rate, bool vibrato = false)
{
    juce::AudioBuffer<float> audio(2, static_cast<int>(rate * 2.0));
    audio.clear();
    double phase = 0.0;
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto time = i / rate;
        if (time < 0.2 || (time >= 0.75 && time < 0.9) || time >= 1.6) continue;
        const auto midi = (time < 0.9 ? 69.0 : 72.0) + (vibrato ? 0.6 * std::sin(time * 6.0 * juce::MathConstants<double>::twoPi) : 0.0);
        phase += juce::MathConstants<double>::twoPi * 440.0 * std::pow(2.0, (midi - 69.0) / 12.0) / rate;
        const auto value = static_cast<float>(0.2 * std::sin(phase) + 0.05 * std::sin(phase * 2.0));
        audio.setSample(0, i, value);
        audio.setSample(1, i, value * 0.7f);
    }
    return audio;
}

native_annotation::NoteAnalysis analyze(const juce::AudioBuffer<float>& audio, double rate)
{
    std::atomic<bool> cancelled { false };
    std::atomic<float> progress { 0.0f };
    return native_annotation::NoteAnalysis::analyze(audio, rate, cancelled, progress);
}

void analysisChecks()
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto result = analyze(fixture(rate), rate);
        require(result.notes.size() == 2, "Tone/gap fixture must produce two notes");
        require(result.notes[0].pitch == 69 && result.notes[1].pitch == 72, "Detected MIDI pitch differs");
        require(std::abs(result.notes[0].start - 0.2) <= 0.03, "First attack is misplaced");
        require(std::abs(result.notes[0].end - 0.75) <= 0.05, "First offset is misplaced");
        require(std::abs(result.notes[1].start - 0.9) <= 0.03, "Second attack is misplaced");
        require(std::abs(result.notes[1].end - 1.6) <= 0.05, "Second offset is misplaced");
    }
    const auto vibrato = analyze(fixture(48000.0, true), 48000.0);
    require(vibrato.notes.size() == 2, "Vibrato must not create extra notes");
    auto inverted = fixture(48000);
    inverted.applyGain(1, 0, inverted.getNumSamples(), -1.0f / 0.7f);
    require(analyze(inverted, 48000).notes.size() == 2, "Inverted stereo must not cancel analysis");
    juce::AudioBuffer<float> silence(1, 48000);
    silence.clear();
    require(analyze(silence, 48000).notes.empty(), "Silence must remain unlabelled");
    std::atomic<bool> cancelled { true };
    std::atomic<float> progress { 0.0f };
    require(native_annotation::NoteAnalysis::analyze(fixture(48000), 48000, cancelled, progress).frames.empty(), "Cancellation must stop analysis");

    // Connected pitches need separate notes even without silence.
    auto legato = fixture(48000);
    for (int i = 36000; i < 43200; ++i)
    {
        const auto value = static_cast<float>(0.2 * std::sin(juce::MathConstants<double>::twoPi * 440.0 * i / 48000.0));
        legato.setSample(0, i, value);
        legato.setSample(1, i, value);
    }
    const auto connected = analyze(legato, 48000);
    require(connected.notes.size() == 2 && connected.notes[1].pitch == 72, "Sustained pitch transition must split");

    auto attacked = fixture(48000);
    juce::Random random(42);
    for (int i = 7200; i < 9600; ++i)
    {
        const auto noise = (random.nextFloat() - 0.5f) * 0.08f;
        attacked.setSample(0, i, noise);
        attacked.setSample(1, i, noise);
    }
    const auto attack = analyze(attacked, 48000);
    require(attack.notes.size() == 2 && attack.notes[0].start <= 0.17, "Note onset must include the attack before stable F0");
    require(attack.notes[0].voicedStart > attack.notes[0].start, "Acoustic and voiced onsets must remain distinct");
}

void editingAndExportChecks()
{
    const auto temporary = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("native-annotation-test-" + juce::Uuid().toString());
    require(temporary.createDirectory().wasOk(), "Could not create test directory");
    const auto cleanup = juce::ScopeGuard([&] { temporary.deleteRecursively(); });
    const auto audio = fixture(48000);
    const auto analysis = analyze(audio, 48000);
    vocal_annotation::AnnotationDocument document;
    document.audioFile = temporary.getChildFile("source.wav");
    require(document.audioFile.replaceWithText("source fixture identity"), "Could not create source identity");
    document.duration = 2.0;
    document.sampleRate = 48000;
    document.notes = analysis.notes;
    const auto originalEnd = document.notes[0].end;
    require(native_annotation::NoteEditing::insert(document, analysis, 0.4), "Cmd click must split existing note");
    require(document.notes.size() == 3 && std::abs(document.notes[0].end - document.notes[1].start) < 1.0e-9, "Split must preserve adjacency");
    require(std::abs(document.notes[1].end - originalEnd) < 1.0e-9, "Split must preserve inferred end");
    require(! native_annotation::NoteEditing::insert(document, analysis, 0.401), "Avoid tiny fragments near edges");
    const auto snapshot = document.notes;
    require(native_annotation::NoteEditing::resize(document, 0, false, 1.5), "Could not resize edge");
    require(document.notes[0].end <= document.notes[1].start, "Edge resize must not overlap next note");
    document.notes = snapshot;
    require(native_annotation::NoteEditing::resize(document, 1, true, -10.0), "Could not resize start");
    require(document.notes[1].start >= document.notes[0].end, "Edge resize must not overlap previous note");
    require(native_annotation::DatasetExport::validate(document).wasOk(), "Edits produced invalid notes");
    document.notes = analysis.notes;
    document.notes.erase(document.notes.begin());
    require(native_annotation::NoteEditing::insert(document, analysis, 0.3), "Could not create note in gap");
    require(std::abs(document.notes[0].end - originalEnd) < 0.001, "New note must infer acoustic end");
    require(document.notes[0].pitch == 69, "New note must infer pitch");

    native_annotation::DatasetMetadata metadata;
    metadata.sourceHash = juce::SHA256(document.audioFile).toHexString();
    metadata.singer = "singer-01";
    metadata.session = "song-01";
    metadata.reviewed = true;
    juce::File published;
    require(native_annotation::DatasetExport::write(document, metadata, audio, temporary, published).wasOk(), "Dataset export failed");
    const auto stem = "vocal_" + metadata.sourceHash.substring(0, 24);
    require(published.getChildFile(stem + ".wav").existsAsFile(), "WAV missing");
    const auto jsonFile = published.getChildFile(stem + ".annotation.json");
    vocal_annotation::AnnotationDocument restored;
    require(vocal_annotation::AnnotationJson::load(jsonFile, restored).wasOk(), "JSON cannot be reloaded");
    require(restored.audioFile.existsAsFile() && restored.notes.size() == document.notes.size(), "JSON/audio pairing failed");
    require(native_annotation::DatasetExport::readMetadata(jsonFile).reviewed, "Review state was lost");
    require(native_annotation::DatasetExport::readMetadata(jsonFile).singer == metadata.singer, "Singer ID was lost");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(restored.audioFile));
    require(reader != nullptr && reader->lengthInSamples == audio.getNumSamples() && reader->sampleRate == 48000.0 && reader->numChannels == 2, "Export must preserve timing and channels");
    const auto lines = juce::StringArray::fromLines(published.getChildFile(stem + ".notes.txt").loadFileAsString().trim());
    require(lines.size() == static_cast<int>(document.notes.size()), "mir_eval table row count differs");
    const auto values = juce::StringArray::fromTokens(lines[0], "\t", "");
    require(values.size() == 3 && std::abs(values[2].getDoubleValue() - 440.0) < 0.001, "mir_eval pitch must be Hertz");
    require(published.getChildFile(stem + ".notes.csv").loadFileAsString().contains("14400"), "CSV must contain sample positions");
    juce::File second;
    require(native_annotation::DatasetExport::write(document, metadata, audio, temporary, second).wasOk() && second != published, "Repeated exports must not overwrite");
    // Windows cannot move the bundle while its audio file is open.
    reader.reset();
    const auto moved = temporary.getChildFile("moved-bundle");
    require(published.moveFileTo(moved), "Could not move bundle");
    require(vocal_annotation::AnnotationJson::load(moved.getChildFile(stem + ".annotation.json"), restored).wasOk()
            && restored.audioFile.existsAsFile(), "Bundle must be portable");
    document.notes[0].start = std::numeric_limits<double>::quiet_NaN();
    require(native_annotation::DatasetExport::validate(document).failed(), "Reject non-finite annotation");
}

void modelChecks()
{
    native_annotation::NoteBoundaryModel model;
    require(model.load(juce::File("/no-such-native-note-model.json")).failed(), "Missing model must fail gracefully");
    native_annotation::NoteBoundaryModel::Prediction prediction;
    prediction.probabilities.resize(150, { 0.0f, 0.0f, 0.0f });
    for (size_t i = 10; i < 50; ++i) prediction.probabilities[i][2] = 1.0f;
    for (size_t i = 70; i < 130; ++i) prediction.probabilities[i][2] = 1.0f;
    for (const size_t i : { 10U, 70U, 100U }) prediction.probabilities[i][0] = 1.0f;
    for (const size_t i : { 50U, 100U, 130U }) prediction.probabilities[i][1] = 1.0f;
    const auto intervals = model.decode(prediction, 1.5);
    require(intervals.size() == 3, "Decoder must retain connected notes and pauses");
    require(std::abs(intervals[1].first - 0.7) < 1e-9 && std::abs(intervals[1].second - 1.0) < 1e-9, "Decoder boundaries differ");
    require(model.decode({}, 1.0).empty(), "Empty prediction must remain empty");

    native_annotation::NoteAnalysis mixed;
    mixed.duration = 1.5;
    for (int i = 0; i < 150; ++i)
        mixed.frames.push_back({ i * 0.01, 69.0, 0.95, 0.1f,
                                 (i >= 76 && i < 98) || (i >= 104 && i < 128) });
    model.apply(mixed, prediction);
    require(mixed.notes.size() == intervals.size(), "Model boundaries must survive missing pitch evidence");
    for (size_t i = 0; i < mixed.notes.size(); ++i)
    {
        const auto& note = mixed.notes[i];
        require(std::abs(note.start - intervals[i].first) < 1e-9 && std::abs(note.end - intervals[i].second) < 1e-9,
                "Pitch estimation must not alter model boundaries");
        require(note.id == "n" + juce::String(static_cast<int>(i) + 1).paddedLeft('0', 4), "Drafts must retain unique IDs");
    }
    require(mixed.notes[0].flags.contains("pitch_uncertain") && mixed.notes[0].curve.empty(),
            "Missing pitch must remain explicitly marked");
    require(mixed.notes[1].voicedStart > mixed.notes[1].start, "Unvoiced attacks must remain part of a pitched note");
    auto noise = mixed;
    for (auto& frame : noise.frames) frame.voiced = false;
    model.apply(noise, prediction);
    require(noise.notes.size() == intervals.size(), "Pitch uncertainty must not suppress model intervals");
    for (const auto& note : noise.notes)
        require(note.flags.contains("pitch_uncertain"), "Uncertain pitch must not be presented as a confident estimate");
    auto silence = noise;
    for (auto& frame : silence.frames) frame.rms = 0.0f;
    model.apply(silence, prediction);
    require(silence.notes.empty(), "Silent audio must remain empty");

    vocal_annotation::AnnotationDocument manual;
    manual.duration = mixed.duration;
    require(native_annotation::NoteEditing::insert(manual, mixed, 0.2), "Explicit manual insertion must remain available");
    require(manual.notes.size() == 1 && manual.notes[0].flags.contains("manual_edit"), "Manual notes must not be filtered");

    prediction.probabilities[10][0] = 0.7f;
    prediction.probabilities[15][0] = 0.9f;
    prediction.probabilities[30][0] = 0.8f;
    const auto ranked = model.decode(prediction, 1.5);
    require(! ranked.empty() && std::abs(ranked.front().first - 0.15) < 1e-9,
            "Nearby candidates must retain the higher-confidence onset");

    const auto temporary = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("note-model-test-" + juce::Uuid().toString() + ".json");
    const auto cleanup = juce::ScopeGuard([&] { temporary.deleteFile(); });
    const auto zeroArray = [](int count)
    {
        juce::Array<juce::var> values;
        values.ensureStorageAllocated(count);
        for (int i = 0; i < count; ++i) values.add(0.0);
        return values;
    };
    auto* root = new juce::DynamicObject();
    const juce::var document(root);
    root->setProperty("schema", "synthetic-obsidian-note-boundary-tcn-v1");
    root->setProperty("feature_contract", "strongest-linear-hann-slaney-clipnorm-v1");
    root->setProperty("model_id", "unit-fixture");
    root->setProperty("name", "Fixture");
    root->setProperty("sample_rate", 16000);
    root->setProperty("hop_length", 160);
    root->setProperty("n_fft", 512);
    root->setProperty("n_mels", 64);
    root->setProperty("channels", 64);
    root->setProperty("dilations", juce::Array<juce::var> { 1, 2, 4, 8 });
    root->setProperty("heads", juce::Array<juce::var> { "onset", "offset", "activity" });
    root->setProperty("thresholds", juce::Array<juce::var> { 0.5, 0.5, 0.5 });
    root->setProperty("mel_filters", zeroArray(64 * 257));
    auto* weights = new juce::DynamicObject();
    const auto layer = [&](const juce::String& name, int outputs, int kernel)
    {
        weights->setProperty(juce::Identifier(name + ".weight"), zeroArray(outputs * 64 * kernel));
        weights->setProperty(juce::Identifier(name + ".bias"), zeroArray(outputs));
    };
    layer("input_projection.0", 64, 1);
    for (int i = 0; i < 4; ++i)
    {
        layer("temporal_blocks." + juce::String(i) + ".temporal", 64, 3);
        layer("temporal_blocks." + juce::String(i) + ".mix", 64, 1);
    }
    layer("output_heads", 3, 1);
    weights->setProperty("output_heads.bias", juce::Array<juce::var> { -6.0, -6.0, 6.0 });
    root->setProperty("layers", weights);
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write model fixture");
    require(model.load(temporary).wasOk(), "Valid model fixture must load");
    std::atomic<bool> cancelled { false };
    std::atomic<float> progress { 0.0f };
    const auto predicted = model.predict(fixture(48000), 48000, cancelled, progress);
    require(predicted.probabilities.size() == 201 && predicted.features.size() == 201 * 64, "Native feature timing differs");
    require(predicted.probabilities.front()[2] > 0.99f && predicted.probabilities.front()[0] < 0.01f, "Native output logits differ");
    cancelled.store(true);
    require(model.predict(fixture(48000), 48000, cancelled, progress).probabilities.empty(), "Model cancellation must stop prediction");
    root->setProperty("schema", "synthetic-obsidian-note-boundary-tcn-v2");
    root->setProperty("feature_contract", "strongest-linear-multiscale-noise-clipnorm-v2");
    root->setProperty("input_features", 131);
    root->setProperty("long_n_fft", 2048);
    root->setProperty("long_mel_filters", zeroArray(64 * 1025));
    root->setProperty("extra_features", juce::Array<juce::var> { "spectral_flatness", "high_frequency_ratio", "log_energy" });
    root->setProperty("dilations", juce::Array<juce::var> { 1, 2, 4, 8, 16, 16 });
    weights->setProperty("input_projection.0.weight", zeroArray(64 * 131));
    for (int i = 4; i < 6; ++i)
    {
        layer("temporal_blocks." + juce::String(i) + ".temporal", 64, 3);
        layer("temporal_blocks." + juce::String(i) + ".mix", 64, 1);
    }
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write multiscale fixture");
    require(model.load(temporary).wasOk(), "Multiscale model must load");
    cancelled.store(false);
    const auto multiscale = model.predict(fixture(48000), 48000, cancelled, progress);
    require(multiscale.features.size() == 201 * 131 && multiscale.probabilities.size() == 201,
            "Multiscale feature dimensions differ");
    require(multiscale.probabilities.front()[2] > 0.99f, "Multiscale output layer differs");
    for (const auto value : multiscale.features) require(std::isfinite(value), "Multiscale features must be finite");
    root->setProperty("schema", "synthetic-obsidian-note-boundary-tcn-v3");
    root->setProperty("feature_contract", "strongest-linear-multiscale-yin-relative-v3");
    root->setProperty("yin_contract", "native-yin-8k-40ms-hop10ms-v1");
    root->setProperty("input_features", 138);
    root->setProperty("extra_features", juce::Array<juce::var> { "spectral_flatness", "high_frequency_ratio", "log_energy",
        "yin_confidence", "yin_voiced", "voiced_context_210ms", "relative_pitch_810ms", "pitch_delta_100ms",
        "pitch_delta_support", "pitch_spread_210ms" });
    weights->setProperty("input_projection.0.weight", zeroArray(64 * 138));
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write relative-pitch fixture");
    require(model.load(temporary).wasOk(), "Relative-pitch model must load");
    auto pitchAnalysis = analyze(fixture(48000), 48000);
    const auto relative = model.predict(fixture(48000), 48000, cancelled, progress, &pitchAnalysis);
    require(relative.features.size() == 201 * 138, "Relative-pitch feature dimensions differ");
    for (const auto value : relative.features) require(std::isfinite(value), "Relative-pitch features must be finite");
    require(relative.features == model.predict(fixture(48000), 48000, cancelled, progress).features,
            "Reusing worker analysis must match standalone prediction");
    require(relative.probabilities.front()[2] > 0.99f, "Relative-pitch output layer differs");
    root->setProperty("schema", "synthetic-obsidian-note-boundary-tcn-v4");
    root->setProperty("feature_contract", "strongest-linear-multiscale-yin-structure-v4");
    root->setProperty("pitch_structure_contract", "native-yin-centres-difference-acf-810ms-v1");
    root->setProperty("input_features", 152);
    auto extras = *root->getProperty("extra_features").getArray();
    for (const auto* feature : { "pitch_center_delta_200ms", "pitch_center_delta_400ms", "pitch_center_support_200ms", "pitch_center_support_400ms",
        "pitch_trend_past_200ms", "pitch_trend_future_200ms", "pitch_stability_past_200ms", "pitch_stability_future_200ms",
        "pitch_persistence_past_200ms", "pitch_persistence_future_200ms", "pitch_modulation_regularity_810ms",
        "pitch_modulation_frequency_810ms", "pitch_modulation_extent_810ms", "pitch_modulation_support_810ms" }) extras.add(feature);
    root->setProperty("extra_features", extras);
    weights->setProperty("input_projection.0.weight", zeroArray(64 * 152));
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write pitch-structure fixture");
    require(model.load(temporary).wasOk(), "Pitch-structure model must load");
    const auto structured = model.predict(fixture(48000), 48000, cancelled, progress, &pitchAnalysis);
    require(structured.features.size() == 201 * 152, "Pitch-structure feature dimensions differ");
    for (const auto value : structured.features) require(std::isfinite(value), "Pitch-structure features must be finite");
    require(structured.probabilities.front()[2] > 0.99f, "Pitch-structure output layer differs");
    root->setProperty("pitch_structure_contract", "unknown-structure");
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write invalid structure fixture");
    require(model.load(temporary).failed(), "Reject incompatible pitch-structure contracts");
    root->setProperty("pitch_structure_contract", "native-yin-centres-difference-acf-810ms-v1");
    root->setProperty("yin_contract", "unknown-yin");
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write invalid YIN fixture");
    require(model.load(temporary).failed(), "Reject incompatible native YIN contracts");
    root->setProperty("schema", "synthetic-obsidian-note-boundary-tcn-v2");
    root->setProperty("feature_contract", "strongest-linear-multiscale-noise-clipnorm-v2");
    root->setProperty("extra_features", juce::Array<juce::var> { "spectral_flatness", "high_frequency_ratio", "log_energy" });
    weights->setProperty("input_projection.0.weight", zeroArray(64 * 131));
    root->setProperty("input_features", 132);
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write malformed multiscale fixture");
    require(model.load(temporary).failed(), "Reject mismatched multiscale feature count");
    root->setProperty("input_features", 131);
    root->setProperty("sample_rate", 1.0e100);
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot write invalid model fixture");
    require(model.load(temporary).failed(), "Reject invalid scalar metadata before integer conversion");
    cancelled.store(false);
    require(model.predict(fixture(48000), 48000, cancelled, progress).probabilities.empty(), "Failed model must not retain readiness");
    root->setProperty("schema", "synthetic-obsidian-note-boundary-tcn-v1");
    root->setProperty("feature_contract", "strongest-linear-hann-slaney-clipnorm-v1");
    root->setProperty("sample_rate", 16000);
    root->setProperty("dilations", juce::Array<juce::var> { 1, 2, 4, 8 });
    weights->setProperty("input_projection.0.weight", zeroArray(64 * 64));
    require(temporary.replaceWithText(juce::JSON::toString(document, true)), "Cannot restore legacy fixture");
    require(model.load(temporary).wasOk(), "Switching back to legacy models must remain supported");
    require(model.predict(fixture(48000), 48000, cancelled, progress).features.size() == 201 * 64,
            "Legacy reload must restore its input dimensions");
}

int predictFile(const juce::String& modelPath, const juce::File& audioFile, const juce::File& output, bool diagnostics)
{
    const auto began = juce::Time::getMillisecondCounterHiRes();
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audioFile));
    require(reader != nullptr && reader->lengthInSamples > 0 && reader->lengthInSamples <= std::numeric_limits<int>::max(), "Cannot read inference input");
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    require(reader->read(&audio, 0, audio.getNumSamples(), 0, true, true), "Cannot decode inference input");
    std::atomic<bool> cancelled { false };
    std::atomic<float> progress { 0.0f };
    auto analysis = native_annotation::NoteAnalysis::analyze(audio, reader->sampleRate, cancelled, progress);
    const auto mathNotes = analysis.notes;
    native_annotation::NoteBoundaryModel::Prediction prediction;
    if (modelPath != "math")
    {
        native_annotation::NoteBoundaryModel model;
        const auto loaded = model.load(juce::File(modelPath));
        if (loaded.failed()) throw std::runtime_error(loaded.getErrorMessage().toStdString());
        prediction = model.predict(audio, reader->sampleRate, cancelled, progress, &analysis);
        require(! prediction.probabilities.empty(), "Empty native model prediction");
        model.apply(analysis, prediction);
    }
    const auto noteArray = [](const std::vector<vocal_annotation::NoteBlock>& notes)
    {
        juce::Array<juce::var> array;
        for (const auto& note : notes)
        {
            juce::Array<juce::var> values { note.start, note.end, note.pitch };
            array.add(values);
        }
        return array;
    };
    auto* root = new juce::DynamicObject();
    root->setProperty("notes", noteArray(analysis.notes));
    root->setProperty("baseline_notes", noteArray(mathNotes));
    root->setProperty("draft_policy", modelPath == "math" ? "yin_energy_v1" : "raw_boundary_intervals");
    root->setProperty("seconds", analysis.duration);
    root->setProperty("elapsed_ms", juce::Time::getMillisecondCounterHiRes() - began);
    if (diagnostics)
    {
        juce::Array<juce::var> probabilities, featureValues, voicedTimes, yinFrames;
        for (const auto& frame : prediction.probabilities)
        {
            juce::Array<juce::var> values { frame[0], frame[1], frame[2] };
            probabilities.add(values);
        }
        for (const auto value : prediction.features) featureValues.add(value);
        for (const auto& frame : analysis.frames)
        {
            if (frame.voiced) voicedTimes.add(frame.time);
            yinFrames.add(juce::Array<juce::var> { frame.midi, frame.confidence, frame.voiced ? 1 : 0 });
        }
        root->setProperty("probabilities", probabilities);
        root->setProperty("features", featureValues);
        root->setProperty("voiced_times", voicedTimes);
        root->setProperty("yin_frames", yinFrames);
    }
    require(output.replaceWithText(juce::JSON::toString(juce::var(root), true)), "Cannot write inference diagnostics");
    return 0;
}
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juce;
    try
    {
        if (argc >= 5 && juce::String(argv[1]) == "--predict")
            return predictFile(argv[2], juce::File(argv[3]), juce::File(argv[4]), argc >= 6);
        analysisChecks();
        editingAndExportChecks();
        modelChecks();
        std::cout << "Native annotation analysis, editing and dataset checks passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
