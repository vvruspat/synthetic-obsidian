#pragma once
#include <JuceHeader.h>
#include "TrackModel.h"

// ProjectState — non-automatable session state (tracks, presets, etc.)
// Lives in PluginProcessor, accessed from message thread and UI.
// NOT accessed from audio thread.
class ProjectState : private juce::ValueTree::Listener
{
public:
    ProjectState();
    ~ProjectState() override;

    // Serialization
    juce::ValueTree toValueTree() const;
    void            fromValueTree(const juce::ValueTree& vt);

    // Track management
    const juce::Array<TrackModel>& getTracks() const { return tracks_; }
    void addTrack(TrackModel track);
    void removeTrack(const juce::Uuid& id);
    void updateTrack(const TrackModel& track);
    TrackModel* findTrack(const juce::Uuid& id);

    // Key/tempo info (set by AnalysisModule after guide track analysis)
    void setDetectedKey(const juce::String& key);      // e.g. "C MIN"
    void setDetectedBpm(double bpm);
    void setDetectedTimeSignature(int numerator, int denominator);

    juce::String getDetectedKey()  const { return detectedKey_; }
    double       getDetectedBpm()  const { return detectedBpm_; }
    int          getTimeSigNum()   const { return timeSigNum_; }
    int          getTimeSigDen()   const { return timeSigDen_; }

    // Change listener support for UI updates
    void addChangeListener(juce::ChangeListener* listener);
    void removeChangeListener(juce::ChangeListener* listener);

private:
    void notifyChanged();

    juce::Array<TrackModel> tracks_;

    juce::String detectedKey_  { "C MIN" };
    double       detectedBpm_  { 120.0 };
    int          timeSigNum_   { 4 };
    int          timeSigDen_   { 4 };

    juce::ListenerList<juce::ChangeListener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectState)
};
