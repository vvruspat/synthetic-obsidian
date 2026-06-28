#include "TrackModel.h"

juce::ValueTree TrackModel::toValueTree() const
{
    juce::ValueTree vt("Track");

    vt.setProperty("id", id.toString(), nullptr);
    vt.setProperty("type", static_cast<int>(type), nullptr);
    vt.setProperty("name", name, nullptr);
    vt.setProperty("voicePresetName", voicePresetName, nullptr);
    vt.setProperty("style", static_cast<int>(style), nullptr);
    vt.setProperty("isMuted", isMuted, nullptr);
    vt.setProperty("isSolo", isSolo, nullptr);
    vt.setProperty("volume", volume, nullptr);
    vt.setProperty("audioFilePath", audioFile.getFullPathName(), nullptr);

    return vt;
}

TrackModel TrackModel::fromValueTree(const juce::ValueTree& vt)
{
    TrackModel track;

    if (vt.hasType("Track"))
    {
        // Parse UUID, or generate a new one if missing/invalid
        juce::String idStr = vt.getProperty("id", "").toString();
        if (idStr.isNotEmpty())
        {
            track.id = juce::Uuid(idStr);
        }
        else
        {
            track.id = juce::Uuid();
        }

        // Parse type
        int typeInt = vt.getProperty("type", static_cast<int>(TrackType::BackVox));
        if (typeInt >= 0 && typeInt <= 2)
            track.type = static_cast<TrackType>(typeInt);

        // Parse basic properties
        track.name = vt.getProperty("name", "").toString();
        track.voicePresetName = vt.getProperty("voicePresetName", "").toString();

        // Parse style
        int styleInt = vt.getProperty("style", static_cast<int>(BackVocalStyle::None));
        if (styleInt >= 0 && styleInt <= 6)
            track.style = static_cast<BackVocalStyle>(styleInt);

        // Parse boolean and float properties
        track.isMuted = vt.getProperty("isMuted", false);
        track.isSolo = vt.getProperty("isSolo", false);
        track.volume = vt.getProperty("volume", 1.0f);

        // Parse audio file path
        juce::String audioPath = vt.getProperty("audioFilePath", "").toString();
        if (audioPath.isNotEmpty())
        {
            track.audioFile = juce::File(audioPath);
        }
    }

    return track;
}

TrackModel TrackModel::makeGuide()
{
    TrackModel track;
    track.id = juce::Uuid();
    track.type = TrackType::Guide;
    track.name = "Guide";
    track.voicePresetName = "";
    track.style = BackVocalStyle::None;
    track.isMuted = false;
    track.isSolo = false;
    track.volume = 1.0f;
    return track;
}

TrackModel TrackModel::makeMainVox()
{
    TrackModel track;
    track.id = juce::Uuid();
    track.type = TrackType::MainVox;
    track.name = "Main Vox";
    track.voicePresetName = "";
    track.style = BackVocalStyle::None;
    track.isMuted = false;
    track.isSolo = false;
    track.volume = 1.0f;
    return track;
}

TrackModel TrackModel::makeBackVox(const juce::String& name, BackVocalStyle style)
{
    TrackModel track;
    track.id = juce::Uuid();
    track.type = TrackType::BackVox;
    track.name = name.isNotEmpty() ? name : "Back Vox";
    track.voicePresetName = "";
    track.style = style;
    track.isMuted = false;
    track.isSolo = false;
    track.volume = 1.0f;
    return track;
}
