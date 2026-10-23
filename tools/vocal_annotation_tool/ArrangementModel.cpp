#include "ArrangementModel.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace vocal_annotation::arrangement
{
ArrangementTrack* findTrack(AnnotationDocument& document, const juce::String& id)
{
    for (auto& track : document.tracks)
        if (track.id == id) return &track;
    return nullptr;
}
const ArrangementTrack* findTrack(const AnnotationDocument& document, const juce::String& id)
{
    for (const auto& track : document.tracks)
        if (track.id == id) return &track;
    return nullptr;
}
std::vector<NoteBlock>& notes(AnnotationDocument& document, const juce::String& id)
{
    if (id == "Voice Main") return document.notes;
    for (auto& track : document.backingTracks)
        if (track.id == id) return track.notes;
    // Instrumental tracks also need a note container when clips move between tracks.
    BackingVocalTrack track;
    track.id = id;
    document.backingTracks.push_back(std::move(track));
    return document.backingTracks.back().notes;
}
const std::vector<NoteBlock>& notes(const AnnotationDocument& document, const juce::String& id)
{
    if (id == "Voice Main") return document.notes;
    for (const auto& track : document.backingTracks)
        if (track.id == id) return track.notes;
    static const std::vector<NoteBlock> empty;
    return empty;
}
static ArrangementClip legacyClip(const juce::String& id, const juce::File& file, double length)
{
    if (file.existsAsFile())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader != nullptr && reader->sampleRate > 0.0)
            length = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    }
    return { "clip-" + id, file.existsAsFile() ? file.getFileNameWithoutExtension() : id,
             file, 0.0, 0.0, juce::jmax(0.001, length) };
}
void syncGeneratedTracks(AnnotationDocument& document)
{
    for (const auto& backing : document.backingTracks)
    {
        if (backing.id == "Instrumental" || backing.id == "Voice Main") continue;
        auto* track = findTrack(document, backing.id);
        const auto createdTrack = track == nullptr;
        if (track == nullptr)
        {
            ArrangementTrack created;
            created.id = backing.id;
            created.name = backing.styleName;
            created.parentId = "Voice Main";
            document.tracks.push_back(std::move(created));
            track = &document.tracks.back();
        }
        if (createdTrack && (! backing.notes.empty() || backing.audioFile.existsAsFile()))
            track->clips.push_back(legacyClip(backing.id, backing.audioFile, document.duration));
    }
    assignClipIds(document);
}
void migrate(AnnotationDocument& document)
{
    if (document.hasArrangement) return;
    document.hasArrangement = true;
    for (const auto& id : { juce::String("Instrumental"), juce::String("Voice Main") })
    {
        ArrangementTrack track;
        track.id = track.name = id;
        const auto& file = id == "Instrumental" ? document.instrumentalFile : document.audioFile;
        if (file.existsAsFile()) track.clips.push_back(legacyClip(id, file, document.duration));
        document.tracks.push_back(std::move(track));
    }
    syncGeneratedTracks(document);
}
void assignClipIds(AnnotationDocument& document)
{
    for (const auto& track : document.tracks)
    {
        if (track.clips.empty() || notes(std::as_const(document), track.id).empty()) continue;
        for (auto& note : notes(document, track.id))
        {
            if (note.sourcePitchExact < 0.0) note.sourcePitchExact = note.pitchExact;
            if (note.clipId.isEmpty())
                for (const auto& clip : track.clips)
                    if (note.start >= clip.start && note.start < clip.start + clip.duration)
                    {
                        note.clipId = clip.id;
                        break;
                    }
        }
    }
}
double duration(const AnnotationDocument& document)
{
    auto end = juce::jmax(1.0, document.duration);
    for (const auto& track : document.tracks)
        for (const auto& clip : track.clips)
            end = juce::jmax(end, clip.start + clip.duration);
    return end;
}
void shiftNote(NoteBlock& note, double delta)
{
    note.start += delta;
    note.end += delta;
    note.voicedStart += delta;
    note.voicedEnd += delta;
    for (auto& point : note.curve) point.time += delta;
}
bool moveClip(AnnotationDocument& document, const juce::String& trackId, const juce::String& clipId,
              const juce::String& targetTrackId, double start)
{
    if (! std::isfinite(start) || start < 0.0) return false;
    auto* source = findTrack(document, trackId);
    auto* target = findTrack(document, targetTrackId);
    if (source == nullptr || target == nullptr) return false;
    const auto item = std::find_if(source->clips.begin(), source->clips.end(),
                                  [&](const auto& clip) { return clip.id == clipId; });
    if (item == source->clips.end()) return false;
    const auto delta = start - item->start;
    auto moved = *item;
    moved.start = start;
    if (trackId == "Voice Main")
    {
        if (targetTrackId == trackId)
        {
            for (auto& region : document.regions)
                if (region.start >= item->start && region.end <= item->start + item->duration)
                { region.start += delta; region.end += delta; }
        }
        else
            std::erase_if(document.regions, [&](const auto& region)
            { return region.start >= item->start && region.end <= item->start + item->duration; });
    }
    // Ensure both containers exist before taking references (the backing vector can grow).
    notes(document, targetTrackId);
    auto& sourceNotes = notes(document, trackId);
    for (auto& note : sourceNotes)
        if (note.clipId == clipId)
        {
            shiftNote(note, delta);
        }
    if (trackId != targetTrackId)
    {
        auto& targetNotes = notes(document, targetTrackId);
        for (const auto& note : sourceNotes)
            if (note.clipId == clipId) targetNotes.push_back(note);
        std::erase_if(sourceNotes, [&](const auto& note) { return note.clipId == clipId; });
        source->clips.erase(item);
        target->clips.push_back(std::move(moved));
    }
    else *item = std::move(moved);
    return true;
}
bool removeClip(AnnotationDocument& document, const juce::String& trackId, const juce::String& clipId)
{
    auto* track = findTrack(document, trackId);
    if (track == nullptr) return false;
    if (std::erase_if(track->clips, [&](const auto& clip) { return clip.id == clipId; }) == 0) return false;
    if (trackId == "Voice Main")
        for (const auto& note : document.notes)
            if (note.clipId == clipId)
                std::erase_if(document.regions, [&](const auto& region) { return region.noteId == note.id; });
    std::erase_if(notes(document, trackId), [&](const auto& note) { return note.clipId == clipId; });
    return true;
}
bool removeTrack(AnnotationDocument& document, const juce::String& trackId)
{
    if (findTrack(document, trackId) == nullptr) return false;
    std::set<juce::String> removed { trackId };
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto& track : document.tracks)
            if (removed.contains(track.parentId)) changed |= removed.insert(track.id).second;
    }
    std::erase_if(document.tracks, [&](const auto& track) { return removed.contains(track.id); });
    std::erase_if(document.backingTracks, [&](const auto& track) { return removed.contains(track.id); });
    if (removed.contains("Voice Main"))
    { document.notes.clear(); document.regions.clear(); document.boundaries.clear(); document.audioFile = juce::File(); }
    if (removed.contains("Instrumental")) document.instrumentalFile = juce::File();
    return true;
}
juce::var toJson(const AnnotationDocument& document, const juce::File& baseDirectory)
{
    juce::Array<juce::var> tracks;
    for (const auto& track : document.tracks)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty("id", track.id);
        object->setProperty("name", track.name);
        object->setProperty("parentId", track.parentId);
        object->setProperty("collapsed", track.collapsed);
        object->setProperty("mute", track.muted);
        object->setProperty("solo", track.solo);
        object->setProperty("audioMuted", track.audioMuted);
        object->setProperty("notesMuted", track.notesMuted);
        juce::Array<juce::var> clips;
        for (const auto& clip : track.clips)
        {
            auto* value = new juce::DynamicObject();
            value->setProperty("id", clip.id);
            value->setProperty("name", clip.name);
            value->setProperty("audio", clip.audioFile == juce::File() ? juce::String()
                : clip.audioFile.isAChildOf(baseDirectory) ? clip.audioFile.getRelativePathFrom(baseDirectory)
                : clip.audioFile.getFullPathName());
            value->setProperty("start", clip.start);
            value->setProperty("offset", clip.offset);
            value->setProperty("duration", clip.duration);
            clips.add(value);
        }
        object->setProperty("clips", clips);
        tracks.add(object);
    }
    return tracks;
}
juce::Result fromJson(const juce::var& value, AnnotationDocument& document, const juce::File& baseDirectory)
{
    const auto* items = value.getArray();
    if (items == nullptr) return juce::Result::fail("Arrangement tracks must be an array.");
    std::set<juce::String> trackIds, clipIds;
    std::vector<ArrangementTrack> tracks;
    for (const auto& item : *items)
    {
        ArrangementTrack track;
        track.id = item["id"].toString();
        track.name = item["name"].toString();
        track.parentId = item["parentId"].toString();
        if (track.id.isEmpty() || ! trackIds.insert(track.id).second)
            return juce::Result::fail("Arrangement track IDs must be unique.");
        track.collapsed = item["collapsed"];
        track.muted = item["mute"];
        track.solo = item["solo"];
        track.audioMuted = item["audioMuted"];
        track.notesMuted = item["notesMuted"];
        if (const auto* clips = item["clips"].getArray())
            for (const auto& entry : *clips)
            {
                ArrangementClip clip;
                clip.id = entry["id"].toString();
                clip.name = entry["name"].toString();
                clip.start = entry["start"];
                clip.offset = entry["offset"];
                clip.duration = entry["duration"];
                const auto path = entry["audio"].toString();
                if (path.isNotEmpty()) clip.audioFile = juce::File::isAbsolutePath(path)
                    ? juce::File(path) : baseDirectory.getChildFile(path);
                if (clip.id.isEmpty() || ! clipIds.insert(clip.id).second
                    || ! std::isfinite(clip.start) || clip.start < 0.0
                    || ! std::isfinite(clip.offset) || clip.offset < 0.0
                    || ! std::isfinite(clip.duration) || clip.duration <= 0.0)
                    return juce::Result::fail("Invalid arrangement clip.");
                track.clips.push_back(std::move(clip));
            }
        tracks.push_back(std::move(track));
    }
    for (const auto& track : tracks)
    {
        std::set<juce::String> ancestors { track.id };
        auto parent = track.parentId;
        while (parent.isNotEmpty())
        {
            if (! trackIds.contains(parent) || ! ancestors.insert(parent).second)
                return juce::Result::fail("Invalid arrangement parent or cycle.");
            const auto found = std::find_if(tracks.begin(), tracks.end(), [&](const auto& t) { return t.id == parent; });
            parent = found->parentId;
        }
    }
    document.tracks = std::move(tracks);
    document.hasArrangement = true;
    assignClipIds(document);
    return juce::Result::ok();
}
}
