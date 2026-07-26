#include "AnnotationJson.h"

#include <algorithm>
#include <cmath>

namespace vocal_annotation
{

namespace
{
double numberProperty(const juce::DynamicObject& object, const juce::Identifier& name, double fallback)
{
    const auto value = object.getProperty(name);
    return value.isDouble() || value.isInt() ? static_cast<double>(value) : fallback;
}

juce::String stringProperty(const juce::DynamicObject& object, const juce::Identifier& name, const juce::String& fallback = {})
{
    const auto value = object.getProperty(name);
    return value.isString() ? value.toString() : fallback;
}

double representativePitchExact(const NoteBlock& note)
{
    std::vector<double> values;
    values.reserve(note.curve.size());
    for (const auto& point : note.curve)
        if (point.time >= note.start - 0.001 && point.time <= note.end + 0.001 && std::isfinite(point.midi))
            values.push_back(point.midi);

    if (values.empty())
        return static_cast<double>(note.pitch);

    std::sort(values.begin(), values.end());
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    return juce::jlimit(0.0, 127.0, *middle);
}

juce::var makeCurvePointVar(const PitchCurvePoint& point)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("time", point.time);
    object->setProperty("midi", point.midi);
    object->setProperty("confidence", point.confidence);
    return object;
}

juce::var makeNoteVar(const NoteBlock& note)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("id", note.id);
    object->setProperty("start", note.start);
    object->setProperty("end", note.end);
    object->setProperty("pitch", note.pitch);
    object->setProperty("pitch_exact", note.pitchExact);
    object->setProperty("gain_db", note.gainDb);
    object->setProperty("voiced_start", note.voicedStart);
    object->setProperty("voiced_end", note.voicedEnd);
    object->setProperty("lyric", note.lyric);
    if (note.syllableId.isNotEmpty())
        object->setProperty("syllable_id", note.syllableId);

    juce::Array<juce::var> flags;
    for (const auto& flag : note.flags)
        flags.add(flag);
    object->setProperty("flags", flags);

    juce::Array<juce::var> curve;
    for (const auto& point : note.curve)
        curve.add(makeCurvePointVar(point));
    object->setProperty("curve", curve);

    return object;
}

std::optional<NoteBlock> readNoteBlock(const juce::var& value, AnnotationDocument& loaded)
{
    auto* noteObject = value.getDynamicObject();
    if (noteObject == nullptr)
        return std::nullopt;

    NoteBlock note;
    note.id = stringProperty(*noteObject, "id", loaded.nextNoteId());
    note.start = numberProperty(*noteObject, "start", 0.0);
    note.end = numberProperty(*noteObject, "end", note.start + 0.25);
    note.pitch = static_cast<int>(numberProperty(*noteObject, "pitch", 60.0));
    note.gainDb = juce::jlimit(-24.0, 12.0, numberProperty(*noteObject, "gain_db", 0.0));
    note.voicedStart = numberProperty(*noteObject, "voiced_start", note.start);
    note.voicedEnd = numberProperty(*noteObject, "voiced_end", note.end);
    note.lyric = stringProperty(*noteObject, "lyric");
    note.syllableId = stringProperty(*noteObject, "syllable_id");

    if (auto* flags = noteObject->getProperty("flags").getArray())
        for (const auto& flag : *flags)
            note.flags.add(flag.toString());

    if (auto* curve = noteObject->getProperty("curve").getArray())
    {
        for (const auto& pointValue : *curve)
        {
            auto* pointObject = pointValue.getDynamicObject();
            if (pointObject == nullptr)
                continue;

            note.curve.push_back({
                numberProperty(*pointObject, "time", note.start),
                numberProperty(*pointObject, "midi", static_cast<double>(note.pitch)),
                numberProperty(*pointObject, "confidence", 1.0)
            });
        }
    }

    note.pitchExact = numberProperty(*noteObject, "pitch_exact", representativePitchExact(note));
    return note;
}

juce::var makeBoundaryVar(const BoundaryMarker& boundary)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("id", boundary.id);
    object->setProperty("time", boundary.time);
    object->setProperty("kind", toString(boundary.kind));
    object->setProperty("text", boundary.text);
    object->setProperty("confidence", boundary.confidence);
    if (boundary.source.isNotEmpty())
        object->setProperty("source", boundary.source);
    return object;
}

juce::var makeRegionVar(const AnnotationRegion& region)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("start", region.start);
    object->setProperty("end", region.end);
    object->setProperty("kind", region.kind);
    object->setProperty("note_id", region.noteId);
    return object;
}

juce::var makeTempoVar(const TempoSegment& tempo)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("start", tempo.start);
    object->setProperty("end", tempo.end);
    object->setProperty("bpm", tempo.bpm);
    object->setProperty("confidence", tempo.confidence);
    return object;
}

juce::var makeTimeSignatureVar(const TimeSignatureSegment& signature)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("start", signature.start);
    object->setProperty("end", signature.end);
    object->setProperty("numerator", signature.numerator);
    object->setProperty("denominator", signature.denominator);
    object->setProperty("confidence", signature.confidence);
    return object;
}

juce::var makeChordVar(const ChordSegment& chord)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("start", chord.start);
    object->setProperty("end", chord.end);
    object->setProperty("name", chord.name);
    object->setProperty("confidence", chord.confidence);
    return object;
}

void addPitchBendRangeSetup(juce::MidiMessageSequence& sequence, int channel, int semitones)
{
    sequence.addEvent(juce::MidiMessage::controllerEvent(channel, 101, 0), 0);
    sequence.addEvent(juce::MidiMessage::controllerEvent(channel, 100, 0), 0);
    sequence.addEvent(juce::MidiMessage::controllerEvent(channel, 6, semitones), 0);
    sequence.addEvent(juce::MidiMessage::controllerEvent(channel, 38, 0), 0);
    sequence.addEvent(juce::MidiMessage::controllerEvent(channel, 101, 127), 1);
    sequence.addEvent(juce::MidiMessage::controllerEvent(channel, 100, 127), 1);
}

int pitchWheelForSemitoneOffset(double semitones, double range)
{
    constexpr auto center = 8192;
    const auto normalized = juce::jlimit(-1.0, 1.0, semitones / range);
    return juce::jlimit(0, 16383, center + juce::roundToInt(normalized * static_cast<double>(center - 1)));
}

juce::String projectPathForFile(const juce::File& file, const juce::File& jsonFile)
{
    if (! file.existsAsFile())
        return {};

    const auto projectDirectory = jsonFile.getParentDirectory();
    if (file.isAChildOf(projectDirectory))
        return file.getRelativePathFrom(projectDirectory);

    return file.getFullPathName();
}
} // namespace

juce::Result AnnotationJson::load(const juce::File& jsonFile, AnnotationDocument& document)
{
    auto parsed = juce::JSON::parse(jsonFile);
    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
        return juce::Result::fail("Annotation JSON root must be an object.");

    AnnotationDocument loaded;
    loaded.version = static_cast<int>(numberProperty(*root, "version", 1.0));
    const auto audioPath = stringProperty(*root, "audio");
    loaded.audioFile = juce::File::isAbsolutePath(audioPath) ? juce::File(audioPath) : jsonFile.getSiblingFile(audioPath);
    const auto instrumentalPath = stringProperty(*root, "instrumental");
    loaded.instrumentalFile = instrumentalPath.isEmpty()
        ? juce::File()
        : (juce::File::isAbsolutePath(instrumentalPath) ? juce::File(instrumentalPath) : jsonFile.getSiblingFile(instrumentalPath));
    const auto backingAudioPath = stringProperty(*root, "backing_audio");
    loaded.backingAudioFile = backingAudioPath.isEmpty()
        ? juce::File()
        : (juce::File::isAbsolutePath(backingAudioPath) ? juce::File(backingAudioPath) : jsonFile.getSiblingFile(backingAudioPath));
    loaded.sampleRate = static_cast<int>(numberProperty(*root, "sample_rate", 0.0));
    loaded.duration = numberProperty(*root, "duration", 0.0);
    loaded.bpm = numberProperty(*root, "bpm", 120.0);
    loaded.key = stringProperty(*root, "key", "C major");
    loaded.backingStyleId = stringProperty(*root, "backing_style_id");
    loaded.backingStyleName = stringProperty(*root, "backing_style_name");

    if (auto* notes = root->getProperty("notes").getArray())
    {
        for (const auto& value : *notes)
            if (auto note = readNoteBlock(value, loaded))
                loaded.notes.push_back(std::move(*note));
    }

    if (auto* backingNotes = root->getProperty("backing_notes").getArray())
    {
        for (const auto& value : *backingNotes)
            if (auto note = readNoteBlock(value, loaded))
                loaded.backingNotes.push_back(std::move(*note));
    }

    if (auto* backingTracks = root->getProperty("backing_tracks").getArray())
    {
        for (const auto& value : *backingTracks)
        {
            auto* trackObject = value.getDynamicObject();
            if (trackObject == nullptr)
                continue;

            BackingVocalTrack track;
            track.styleId = stringProperty(*trackObject, "style_id");
            track.styleName = stringProperty(*trackObject, "style_name");
            const auto trackAudioPath = stringProperty(*trackObject, "audio");
            track.audioFile = trackAudioPath.isEmpty()
                ? juce::File()
                : (juce::File::isAbsolutePath(trackAudioPath)
                       ? juce::File(trackAudioPath)
                       : jsonFile.getSiblingFile(trackAudioPath));

            if (auto* notes = trackObject->getProperty("notes").getArray())
            {
                for (const auto& noteValue : *notes)
                    if (auto note = readNoteBlock(noteValue, loaded))
                        track.notes.push_back(std::move(*note));
            }
            if (auto* renderedNotes = trackObject->getProperty("rendered_notes").getArray())
            {
                for (const auto& noteValue : *renderedNotes)
                    if (auto note = readNoteBlock(noteValue, loaded))
                        track.renderedNotes.push_back(std::move(*note));
            }
            if (track.renderedNotes.empty() && track.audioFile.existsAsFile())
                track.renderedNotes = track.notes;

            if (track.styleId.isNotEmpty() || track.styleName.isNotEmpty())
                loaded.backingTracks.push_back(std::move(track));
        }
    }

    if (loaded.backingTracks.empty()
        && (loaded.backingStyleId.isNotEmpty()
            || loaded.backingStyleName.isNotEmpty()
            || ! loaded.backingNotes.empty()
            || loaded.backingAudioFile.existsAsFile()))
    {
        loaded.backingTracks.push_back({
            loaded.backingStyleId,
            loaded.backingStyleName,
            loaded.backingNotes,
            loaded.backingNotes,
            loaded.backingAudioFile
        });
    }
    else if (! loaded.backingTracks.empty())
    {
        const auto activeTrack = std::find_if(
            loaded.backingTracks.begin(),
            loaded.backingTracks.end(),
            [&loaded](const auto& track)
            {
                return (loaded.backingStyleId.isNotEmpty() && track.styleId == loaded.backingStyleId)
                    || (loaded.backingStyleName.isNotEmpty() && track.styleName == loaded.backingStyleName);
            });
        const auto& track = activeTrack != loaded.backingTracks.end()
            ? *activeTrack
            : loaded.backingTracks.front();
        loaded.backingStyleId = track.styleId;
        loaded.backingStyleName = track.styleName;
        loaded.backingNotes = track.notes;
        loaded.backingAudioFile = track.audioFile;
    }

    if (auto* boundaries = root->getProperty("boundaries").getArray())
    {
        for (const auto& value : *boundaries)
        {
            auto* boundaryObject = value.getDynamicObject();
            if (boundaryObject == nullptr)
                continue;

            BoundaryMarker boundary;
            boundary.id = stringProperty(*boundaryObject, "id", loaded.nextBoundaryId());
            boundary.time = numberProperty(*boundaryObject, "time", 0.0);
            boundary.kind = boundaryKindFromString(stringProperty(*boundaryObject, "kind", "syllable"));
            boundary.text = stringProperty(*boundaryObject, "text");
            boundary.confidence = numberProperty(*boundaryObject, "confidence", 1.0);
            boundary.source = stringProperty(*boundaryObject, "source");
            loaded.boundaries.push_back(std::move(boundary));
        }
    }

    if (auto* regions = root->getProperty("regions").getArray())
    {
        for (const auto& value : *regions)
        {
            auto* regionObject = value.getDynamicObject();
            if (regionObject == nullptr)
                continue;

            loaded.regions.push_back({
                numberProperty(*regionObject, "start", 0.0),
                numberProperty(*regionObject, "end", 0.0),
                stringProperty(*regionObject, "kind"),
                stringProperty(*regionObject, "note_id")
            });
        }
    }

    if (auto* tempos = root->getProperty("tempo_segments").getArray())
    {
        for (const auto& value : *tempos)
        {
            auto* tempoObject = value.getDynamicObject();
            if (tempoObject == nullptr)
                continue;

            loaded.tempoSegments.push_back({
                numberProperty(*tempoObject, "start", 0.0),
                numberProperty(*tempoObject, "end", loaded.duration),
                numberProperty(*tempoObject, "bpm", loaded.bpm),
                numberProperty(*tempoObject, "confidence", 0.0)
            });
        }
    }

    if (auto* signatures = root->getProperty("time_signatures").getArray())
    {
        for (const auto& value : *signatures)
        {
            auto* signatureObject = value.getDynamicObject();
            if (signatureObject == nullptr)
                continue;

            loaded.timeSignatures.push_back({
                numberProperty(*signatureObject, "start", 0.0),
                numberProperty(*signatureObject, "end", loaded.duration),
                static_cast<int>(numberProperty(*signatureObject, "numerator", 4.0)),
                static_cast<int>(numberProperty(*signatureObject, "denominator", 4.0)),
                numberProperty(*signatureObject, "confidence", 0.0)
            });
        }
    }

    if (auto* chords = root->getProperty("chords").getArray())
    {
        for (const auto& value : *chords)
        {
            auto* chordObject = value.getDynamicObject();
            if (chordObject == nullptr)
                continue;

            loaded.chords.push_back({
                numberProperty(*chordObject, "start", 0.0),
                numberProperty(*chordObject, "end", loaded.duration),
                stringProperty(*chordObject, "name"),
                numberProperty(*chordObject, "confidence", 0.0)
            });
        }
    }

    document = std::move(loaded);
    return juce::Result::ok();
}

juce::Result AnnotationJson::save(const AnnotationDocument& document, const juce::File& jsonFile)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("version", document.version);
    root->setProperty("audio", projectPathForFile(document.audioFile, jsonFile));
    root->setProperty("instrumental", projectPathForFile(document.instrumentalFile, jsonFile));
    root->setProperty("backing_audio", projectPathForFile(document.backingAudioFile, jsonFile));
    root->setProperty("sample_rate", document.sampleRate);
    root->setProperty("duration", document.duration);
    root->setProperty("bpm", document.bpm);
    root->setProperty("key", document.key);
    root->setProperty("backing_style_id", document.backingStyleId);
    root->setProperty("backing_style_name", document.backingStyleName);

    juce::Array<juce::var> notes;
    for (const auto& note : document.notes)
        notes.add(makeNoteVar(note));
    root->setProperty("notes", notes);

    juce::Array<juce::var> backingNotes;
    for (const auto& note : document.backingNotes)
        backingNotes.add(makeNoteVar(note));
    root->setProperty("backing_notes", backingNotes);

    juce::Array<juce::var> backingTracks;
    for (const auto& track : document.backingTracks)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty("style_id", track.styleId);
        object->setProperty("style_name", track.styleName);
        object->setProperty(
            "audio",
            projectPathForFile(track.audioFile, jsonFile));

        juce::Array<juce::var> trackNotes;
        for (const auto& note : track.notes)
            trackNotes.add(makeNoteVar(note));
        object->setProperty("notes", trackNotes);

        juce::Array<juce::var> renderedNotes;
        for (const auto& note : track.renderedNotes)
            renderedNotes.add(makeNoteVar(note));
        object->setProperty("rendered_notes", renderedNotes);
        backingTracks.add(object);
    }
    root->setProperty("backing_tracks", backingTracks);

    juce::Array<juce::var> boundaries;
    for (const auto& boundary : document.boundaries)
        boundaries.add(makeBoundaryVar(boundary));
    root->setProperty("boundaries", boundaries);

    juce::Array<juce::var> regions;
    for (const auto& region : document.regions)
        regions.add(makeRegionVar(region));
    root->setProperty("regions", regions);

    juce::Array<juce::var> tempos;
    for (const auto& tempo : document.tempoSegments)
        tempos.add(makeTempoVar(tempo));
    root->setProperty("tempo_segments", tempos);

    juce::Array<juce::var> signatures;
    for (const auto& signature : document.timeSignatures)
        signatures.add(makeTimeSignatureVar(signature));
    root->setProperty("time_signatures", signatures);

    juce::Array<juce::var> chords;
    for (const auto& chord : document.chords)
        chords.add(makeChordVar(chord));
    root->setProperty("chords", chords);

    if (auto stream = jsonFile.createOutputStream())
    {
        stream->setPosition(0);
        stream->truncate();
        juce::JSON::writeToStream(*stream, juce::var(root), true);
        return juce::Result::ok();
    }

    return juce::Result::fail("Could not open annotation JSON for writing.");
}

juce::Result AnnotationJson::exportMidi(const AnnotationDocument& document, const juce::File& midiFile)
{
    constexpr int ticksPerQuarterNote = 960;
    constexpr int leadChannel = 1;
    constexpr int backingChannel = 2;
    constexpr int pitchBendRangeSemitones = 12;
    juce::MidiMessageSequence sequence;
    addPitchBendRangeSetup(sequence, leadChannel, pitchBendRangeSemitones);
    addPitchBendRangeSetup(sequence, backingChannel, pitchBendRangeSemitones);

    const auto secondsToTicks = [bpm = document.bpm](double seconds)
    {
        return juce::roundToInt(seconds * bpm / 60.0 * static_cast<double>(ticksPerQuarterNote));
    };

    for (const auto& note : document.notes)
    {
        const auto startTick = secondsToTicks(note.start);
        const auto endTick = juce::jmax(startTick + 1, secondsToTicks(note.end));
        const auto pitch = juce::jlimit(0, 127, note.pitch);
        sequence.addEvent(juce::MidiMessage::pitchWheel(leadChannel, 8192), startTick);
        sequence.addEvent(juce::MidiMessage::noteOn(leadChannel, pitch, juce::uint8(96)), startTick);

        auto curve = note.curve;
        if (curve.empty())
            curve.push_back({ note.start, note.pitchExact, 1.0 });
        std::sort(curve.begin(), curve.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        for (const auto& point : curve)
        {
            if (point.time < note.start || point.time > note.end)
                continue;

            const auto tick = secondsToTicks(point.time);
            const auto wheel = pitchWheelForSemitoneOffset(point.midi - static_cast<double>(pitch),
                                                           static_cast<double>(pitchBendRangeSemitones));
            sequence.addEvent(juce::MidiMessage::pitchWheel(leadChannel, wheel), tick);
        }

        sequence.addEvent(juce::MidiMessage::noteOff(leadChannel, pitch), endTick);
        sequence.addEvent(juce::MidiMessage::pitchWheel(leadChannel, 8192), endTick + 1);
    }

    for (const auto& note : document.backingNotes)
    {
        if (note.end <= note.start)
            continue;

        const auto startTick = secondsToTicks(note.start);
        const auto endTick = juce::jmax(startTick + 1, secondsToTicks(note.end));
        const auto pitch = juce::jlimit(0, 127, note.pitch);
        sequence.addEvent(juce::MidiMessage::noteOn(backingChannel, pitch, juce::uint8(84)), startTick);
        sequence.addEvent(juce::MidiMessage::noteOff(backingChannel, pitch), endTick);
    }

    sequence.updateMatchedPairs();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(ticksPerQuarterNote);
    midi.addTrack(sequence);

    if (auto stream = midiFile.createOutputStream())
    {
        stream->setPosition(0);
        stream->truncate();
        midi.writeTo(*stream);
        return juce::Result::ok();
    }

    return juce::Result::fail("Could not open MIDI file for writing.");
}

} // namespace vocal_annotation
