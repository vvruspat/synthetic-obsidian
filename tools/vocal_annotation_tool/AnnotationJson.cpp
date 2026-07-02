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
    loaded.sampleRate = static_cast<int>(numberProperty(*root, "sample_rate", 0.0));
    loaded.duration = numberProperty(*root, "duration", 0.0);
    loaded.bpm = numberProperty(*root, "bpm", 120.0);
    loaded.key = stringProperty(*root, "key", "C major");

    if (auto* notes = root->getProperty("notes").getArray())
    {
        for (const auto& value : *notes)
        {
            auto* noteObject = value.getDynamicObject();
            if (noteObject == nullptr)
                continue;

            NoteBlock note;
            note.id = stringProperty(*noteObject, "id", loaded.nextNoteId());
            note.start = numberProperty(*noteObject, "start", 0.0);
            note.end = numberProperty(*noteObject, "end", note.start + 0.25);
            note.pitch = static_cast<int>(numberProperty(*noteObject, "pitch", 60.0));
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
            loaded.notes.push_back(std::move(note));
        }
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
    root->setProperty("audio", document.audioFile.existsAsFile() ? document.audioFile.getFileName() : juce::String());
    root->setProperty("instrumental", document.instrumentalFile.existsAsFile() ? document.instrumentalFile.getFileName() : juce::String());
    root->setProperty("sample_rate", document.sampleRate);
    root->setProperty("duration", document.duration);
    root->setProperty("bpm", document.bpm);
    root->setProperty("key", document.key);

    juce::Array<juce::var> notes;
    for (const auto& note : document.notes)
        notes.add(makeNoteVar(note));
    root->setProperty("notes", notes);

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
    constexpr int channel = 1;
    constexpr int pitchBendRangeSemitones = 12;
    juce::MidiMessageSequence sequence;
    addPitchBendRangeSetup(sequence, channel, pitchBendRangeSemitones);

    const auto secondsToTicks = [bpm = document.bpm](double seconds)
    {
        return juce::roundToInt(seconds * bpm / 60.0 * static_cast<double>(ticksPerQuarterNote));
    };

    for (const auto& note : document.notes)
    {
        const auto startTick = secondsToTicks(note.start);
        const auto endTick = juce::jmax(startTick + 1, secondsToTicks(note.end));
        const auto pitch = juce::jlimit(0, 127, note.pitch);
        sequence.addEvent(juce::MidiMessage::pitchWheel(channel, 8192), startTick);
        sequence.addEvent(juce::MidiMessage::noteOn(channel, pitch, juce::uint8(96)), startTick);

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
            sequence.addEvent(juce::MidiMessage::pitchWheel(channel, wheel), tick);
        }

        sequence.addEvent(juce::MidiMessage::noteOff(channel, pitch), endTick);
        sequence.addEvent(juce::MidiMessage::pitchWheel(channel, 8192), endTick + 1);
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
