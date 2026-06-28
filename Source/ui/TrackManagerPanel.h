#pragma once
#include <JuceHeader.h>
#include <set>
#include "SyntheticObsidianLookAndFeel.h"
#include "../state/ProjectState.h"
#include "../state/TrackModel.h"
#include "../analysis/AudioAnalysisEngine.h"

//==============================================================================
/** Single track row.
 *  - Left-click          → select track
 *  - Double-click name   → inline rename
 *  - Right-click         → context menu (Voice / Style / Load Audio / Duplicate / Remove)
 *  - Drag audio file     → loads into track
 */
class TrackRow final : public juce::Component,
                       public juce::FileDragAndDropTarget
{
public:
    explicit TrackRow (const TrackModel& model);

    void paint            (juce::Graphics&)             override;
    void resized          ()                             override;
    void mouseDown        (const juce::MouseEvent& e)   override;
    void mouseDoubleClick (const juce::MouseEvent& e)   override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files)           override;
    void fileDragEnter          (const juce::StringArray&, int, int)       override;
    void fileDragExit           (const juce::StringArray&)                 override;
    void filesDropped           (const juce::StringArray& files, int, int) override;

    //==========================================================================
    std::function<void()>                    onSelected;
    std::function<void()>                    onRemove;
    std::function<void()>                    onDuplicate;
    std::function<void()>                    onVoiceClicked;
    std::function<void()>                    onStyleClicked;
    std::function<void()>                    onLoadAudioRequested; ///< Panel handles file chooser
    std::function<void(const juce::String&)> onNameChanged;
    std::function<void(const juce::File&)>   onAudioFileChanged;   ///< drag-and-drop path
    std::function<void(bool)>                onMuteChanged;
    std::function<void(bool)>                onSoloChanged;
    std::function<void(bool)>                onMidiAuditionChanged;
    std::function<void(float)>               onVolumeChanged;

    void setSelected  (bool s);
    void setAnalyzing (bool a) { isAnalyzing_ = a; repaint(); }
    void updateModel  (const TrackModel& m);
    void setThumbnail (juce::AudioThumbnail* t);   ///< May be nullptr
    void setMidiAuditionEnabled (bool enabled);

    /** Called by panel after file chooser selects a file (updates display only). */
    void setAudioFile (const juce::File& f);

    bool              isSelected  () const noexcept { return isSelected_; }
    const juce::Uuid& getTrackId  () const noexcept { return model_.id; }

    static constexpr int kAccentBarW = 4;
    static constexpr int kBtnSize    = 20;
    static constexpr int kRowH       = 78;   ///< canonical row height

private:
    void showContextMenu ();
    void startNameEdit   ();
    void loadAudioFile   (const juce::File& f);   ///< used by drag-and-drop

    TrackModel             model_;
    bool                   isSelected_ { false };
    bool                   isDragOver_ { false };
    bool                   isAnalyzing_{ false };  ///< show spinner while analysing

    juce::TextButton       muteBtn_       { "M" };
    juce::TextButton       soloBtn_       { "S" };
    juce::TextButton       midiBtn_       { "M" };
    juce::Slider           volumeSlider_;
    juce::TextEditor       nameEditor_;
    juce::AudioThumbnail*  thumbnail_     { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackRow)
};

//==============================================================================
/** Left-side track list panel.
 *  Owns the AudioAnalysisEngine, AudioFormatManager and AudioThumbnailCache.
 */
class TrackManagerPanel final : public juce::Component,
                                public juce::ChangeListener
{
public:
    explicit TrackManagerPanel (ProjectState& state);
    ~TrackManagerPanel() override;

    void paint   (juce::Graphics&) override;
    void resized ()                override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    //==========================================================================
    // Callbacks fired on the message thread

    /** Fired when selection changes OR when analysis completes for the selected track.
     *  Piano roll should update to show these notes + thumbnail. */
    std::function<void (const juce::Uuid& id,
                        const std::vector<MidiNote>& notes,
                        juce::AudioThumbnail* thumb)> onTrackChanged;

    /** Fired whenever an audio file is loaded onto any track (drag or file chooser).
     *  Processor should load the file into its playback engine. */
    std::function<void (const juce::Uuid& id, const juce::File& file)> onAudioLoaded;

    /** Fired when a track is removed. Processor should clean up its player. */
    std::function<void (const juce::Uuid& id)> onTrackRemoved;

    /** Fired when analysis starts (true) or finishes (false) for the selected track. */
    std::function<void (bool isAnalyzing)> onAnalyzingChanged;

    /** Fired whenever a track's volume slider changes. */
    std::function<void (const juce::Uuid& id, float volume)> onTrackVolumeChanged;

    /** Fired whenever a track's mute button toggles. */
    std::function<void (const juce::Uuid& id, bool muted)> onTrackMuteChanged;

    /** Fired whenever a track's solo button toggles. */
    std::function<void (const juce::Uuid& id, bool soloed)> onTrackSoloChanged;

    /** Fired whenever MIDI audition for detected notes toggles. */
    std::function<void (const juce::Uuid& id, bool enabled)> onMidiAuditionChanged;

    /** Fired whenever analysis produces notes for a track. */
    std::function<void (const juce::Uuid& id, const std::vector<MidiNote>& notes)> onTrackNotesChanged;

    /** Mark a track row as "analysing" (spinner) or done. */
    void setTrackAnalyzing (const juce::Uuid& id, bool analysing);

    /** Update note detection sensitivity and reanalyse the selected loaded track. */
    void setDetectionSensitivity (float sensitivity);

    /** Add a generated audio file as a Back Vox track and trigger analysis. */
    juce::Uuid addGeneratedAudioTrack (const juce::String& name,
                                       BackVocalStyle style,
                                       const juce::File& file,
                                       const juce::String& voicePresetName);

private:
    void rebuildTrackRows ();
    void selectTrack      (const juce::Uuid& id);
    void triggerAnalysis  (const juce::Uuid& trackId, const juce::File& f);
    void openFileChooserForTrack (const juce::Uuid& trackId);

    ProjectState&              state_;
    juce::OwnedArray<TrackRow> trackRows_;
    juce::TextButton           addTrackBtn_ { "+ ADD NEW TRACK" };
    juce::Uuid                 selectedId_;

    // Audio analysis infrastructure
    AudioAnalysisEngine                                    analysisEngine_;
    juce::AudioThumbnailCache                              thumbnailCache_ { 16 };
    std::map<juce::Uuid, std::unique_ptr<juce::AudioThumbnail>> thumbnails_;

    // Per-track detected notes (keyed by track UUID)
    std::map<juce::Uuid, std::vector<MidiNote>> trackNotes_;

    // Tracks currently being analysed
    std::set<juce::Uuid> analyzingTracks_;
    float detectionSensitivity_ { 0.72f };
    std::set<juce::Uuid> midiAuditionTracks_;

    // File chooser lives on the panel so it survives row rebuilds
    std::unique_ptr<juce::FileChooser> fileChooser_;

    static constexpr int kRowH    = TrackRow::kRowH;
    static constexpr int kHeaderH = 28;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackManagerPanel)
};
