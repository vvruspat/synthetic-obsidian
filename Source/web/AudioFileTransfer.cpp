#include "AudioFileTransfer.h"

#include <cmath>

namespace synthetic_obsidian
{
namespace
{
juce::var response(const juce::Identifier& key, const juce::var& value)
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty(key, value);
    return juce::var(object.release());
}
}

AudioFileTransfer::AudioFileTransfer(juce::File directory) : directory_(std::move(directory)) {}

AudioFileTransfer::~AudioFileTransfer()
{
    for (const auto& [token, file] : pending_)
    {
        juce::ignoreUnused(token);
        file.partial.deleteFile();
    }
}

juce::var AudioFileTransfer::process(const juce::var& request)
{
    const auto fail = [](const juce::String& message) { return response("error", message); };
    const auto action = request["action"].toString();
    if (action == "begin")
    {
        const auto name = request["name"].toString();
        const auto size = static_cast<double>(request["size"]);
        const auto extension = name.fromLastOccurrenceOf(".", false, false).toLowerCase();
        const juce::StringArray supported { "wav", "aif", "aiff", "flac", "mp3", "ogg" };
        if (! supported.contains(extension) || ! std::isfinite(size) || size <= 0.0
            || size > 9007199254740991.0 || std::floor(size) != size)
            return fail("Drop a WAV, AIFF, FLAC, MP3 or OGG audio file.");
        if (directory_.createDirectory().failed()) return fail("Could not create the audio import directory.");
        const auto token = juce::Uuid().toString();
        const auto destination = directory_.getChildFile(token).getChildFile(juce::File::createLegalFileName(name));
        if (destination.getParentDirectory().createDirectory().failed()) return fail("Could not create the audio import directory.");
        PendingFile file { destination.getSiblingFile(destination.getFileName() + ".part"), destination,
                           static_cast<juce::int64>(size), 0 };
        if (file.partial.create().failed()) return fail("Could not write the imported audio file.");
        pending_.emplace(token, std::move(file));
        return response("token", token);
    }
    const auto it = pending_.find(request["token"].toString());
    if (it == pending_.end()) return fail("Audio import expired. Drop the file again.");
    auto& file = it->second;
    if (action == "abort")
    {
        file.partial.deleteFile();
        file.partial.getParentDirectory().deleteFile();
        pending_.erase(it);
        return response("ok", true);
    }
    if (action == "append")
    {
        const auto encoded = request["data"].toString();
        const auto offset = static_cast<double>(request["offset"]);
        if (! std::isfinite(offset) || (offset < static_cast<double>(file.written) || offset > static_cast<double>(file.written))
            || encoded.length() > 350000)
            return fail("Invalid audio chunk position or size.");
        juce::MemoryOutputStream bytes;
        if (! juce::Base64::convertFromBase64(bytes, encoded) || bytes.getDataSize() == 0
            || bytes.getDataSize() > 262144 || static_cast<juce::int64>(bytes.getDataSize()) > file.size - file.written)
            return fail("Invalid audio chunk.");
        juce::FileOutputStream stream(file.partial);
        if (! stream.openedOk() || ! stream.write(bytes.getData(), bytes.getDataSize()))
            return fail("Could not write imported audio. Check available disk space.");
        stream.flush();
        if (stream.getStatus().failed()) return fail(stream.getStatus().getErrorMessage());
        file.written += static_cast<juce::int64>(bytes.getDataSize());
        return response("ok", true);
    }
    if (action == "finish")
    {
        if (file.written != file.size) return fail("Audio file transfer is incomplete.");
        if (! file.partial.moveFileTo(file.destination)) return fail("Could not finish the audio import.");
        const auto path = file.destination.getFullPathName();
        pending_.erase(it);
        return response("path", path);
    }
    return fail("Unknown audio import operation.");
}
}
