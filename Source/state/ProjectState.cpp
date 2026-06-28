#include "ProjectState.h"

ProjectState::ProjectState()
{
    // Initialize with default tracks: Guide and Main Vox
    tracks_.add(TrackModel::makeGuide());
    tracks_.add(TrackModel::makeMainVox());
}

ProjectState::~ProjectState()
{
}

juce::ValueTree ProjectState::toValueTree() const
{
    juce::ValueTree vt("ProjectState");

    vt.setProperty("detectedKey", detectedKey_, nullptr);
    vt.setProperty("detectedBpm", detectedBpm_, nullptr);
    vt.setProperty("timeSigNum", timeSigNum_, nullptr);
    vt.setProperty("timeSigDen", timeSigDen_, nullptr);

    // Serialize all tracks as child ValueTrees
    for (const auto& track : tracks_)
    {
        vt.appendChild(track.toValueTree(), nullptr);
    }

    return vt;
}

void ProjectState::fromValueTree(const juce::ValueTree& vt)
{
    if (!vt.hasType("ProjectState"))
        return;

    // Parse key/tempo properties with graceful defaults
    detectedKey_ = vt.getProperty("detectedKey", "C MIN").toString();
    detectedBpm_ = vt.getProperty("detectedBpm", 120.0);

    int num = vt.getProperty("timeSigNum", 4);
    int den = vt.getProperty("timeSigDen", 4);
    if (num > 0 && den > 0)
    {
        timeSigNum_ = num;
        timeSigDen_ = den;
    }

    // Parse all track child ValueTrees
    tracks_.clear();
    for (int i = 0; i < vt.getNumChildren(); ++i)
    {
        juce::ValueTree trackVt = vt.getChild(i);
        if (trackVt.hasType("Track"))
        {
            tracks_.add(TrackModel::fromValueTree(trackVt));
        }
    }

    notifyChanged();
}

void ProjectState::addTrack(TrackModel track)
{
    tracks_.add(track);
    notifyChanged();
}

void ProjectState::removeTrack(const juce::Uuid& id)
{
    for (int i = 0; i < tracks_.size(); ++i)
    {
        if (tracks_[i].id == id)
        {
            tracks_.remove(i);
            notifyChanged();
            return;
        }
    }
}

void ProjectState::updateTrack(const TrackModel& track)
{
    for (int i = 0; i < tracks_.size(); ++i)
    {
        if (tracks_[i].id == track.id)
        {
            tracks_.set(i, track);
            notifyChanged();
            return;
        }
    }
}

TrackModel* ProjectState::findTrack(const juce::Uuid& id)
{
    for (auto& track : tracks_)
    {
        if (track.id == id)
            return &track;
    }
    return nullptr;
}

void ProjectState::setDetectedKey(const juce::String& key)
{
    detectedKey_ = key;
    notifyChanged();
}

void ProjectState::setDetectedBpm(double bpm)
{
    detectedBpm_ = bpm;
    notifyChanged();
}

void ProjectState::setDetectedTimeSignature(int numerator, int denominator)
{
    if (numerator > 0 && denominator > 0)
    {
        timeSigNum_ = numerator;
        timeSigDen_ = denominator;
        notifyChanged();
    }
}

void ProjectState::addChangeListener(juce::ChangeListener* listener)
{
    listeners_.add(listener);
}

void ProjectState::removeChangeListener(juce::ChangeListener* listener)
{
    listeners_.remove(listener);
}

void ProjectState::notifyChanged()
{
    listeners_.call([](auto& listener) { listener.changeListenerCallback(nullptr); });
}
