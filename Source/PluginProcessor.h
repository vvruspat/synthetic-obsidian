#pragma once
#include <JuceHeader.h>
#include "dsp/AudioEngine.h"
#include "dsp/PitchCorrectionModule.h"
#include "dsp/WORLDResynthesizer.h"
#include "ai/RVCPythonBridge.h"
#include "ai/SeedVCBridge.h"
#include "state/ProjectState.h"
#include "ui/PianoRollEditor.h"   // for MidiNote

class SyntheticObsidianProcessor final : public juce::AudioProcessor
{
public:
    SyntheticObsidianProcessor();
    ~SyntheticObsidianProcessor() override;

    // AudioProcessor interface
    void prepareToPlay   (double sampleRate, int samplesPerBlock) override;
    void releaseResources ()                                      override;
    void processBlock    (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor () override;
    bool hasEditor () const override { return true; }

    const juce::String getName () const override { return JucePlugin_Name; }

    bool acceptsMidi  () const override { return false; }
    bool producesMidi () const override { return false; }
    bool isMidiEffect () const override { return false; }
    double getTailLengthSeconds () const override { return 0.0; }

    int getNumPrograms () override                               { return 1; }
    int getCurrentProgram () override                           { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS () { return apvts_; }
    ProjectState& getProjectState () { return projectState_; }

    // ── Playback transport ──────────────────────────────────────────────────
    void   setPlaying            (bool shouldPlay);
    void   resetPlaybackPosition ();
    double getPlaybackPosition   () const noexcept { return playbackPos_.load(); }
    bool   isPlayingNow          () const noexcept { return isPlaying_.load(); }
    float  getCpuLoad            () const noexcept { return cpuLoad_.load(); }

    // ── Per-track audio file players ────────────────────────────────────────
    void loadTrackAudio   (const juce::Uuid& id, const juce::File& file);
    void removeTrackAudio (const juce::Uuid& id);

    // ── Per-track mix state (safe to call from message thread) ───────────────
    void setTrackVolume (const juce::Uuid& id, float vol);
    void setTrackMuted  (const juce::Uuid& id, bool muted);
    void setTrackSoloed (const juce::Uuid& id, bool soloed);
    void setTrackDetectedNotes (const juce::Uuid& id, const std::vector<MidiNote>& notes);
    void setTrackMidiAuditionEnabled (const juce::Uuid& id, bool enabled);

    /** Render every loaded track to a separate WAV in outputDir.
     *  Blocking — run from a background thread.
     *  @return  Number of files written. */
    int renderTracks (const juce::File& outputDir);

    /** Apply per-note pitch correction to a track.
     *  Uses RVC resynthesis when a preset is loaded (ADR-006), falls back to
     *  phase vocoder otherwise.
     *  Launches a background thread; calls back onComplete (message thread)
     *  when the corrected audio is loaded into the transport. */
    void applyPitchCorrection (const juce::Uuid&              trackId,
                               const std::vector<MidiNote>&   editedNotes,
                               std::function<void()>          onComplete = {});

    // ── Voice preset (RVC) ──────────────────────────────────────────────────
    /** Load an RVC voice preset from a model file.
     *  Runs on a background thread; calls onComplete on the message thread.
     *  @return  false if a load is already in progress. */
    bool loadVoicePreset (const juce::File&       modelFile,
                          std::function<void (bool success, const juce::String& error)> onComplete = {});

    void unloadVoicePreset();

    /** True when an RVC preset is loaded and resynthesis is available. */
    bool hasVoicePreset() const noexcept { return rvcBridge_.isAvailable(); }

    // ── Seed-VC offline backing vocals ───────────────────────────────────────
    using SeedVCRenderedFiles = SeedVCBridge::RenderedFiles;

    /** Render Seed-VC backing vocals for a track on a background thread.
     *  Callback fires on the message thread. */
    void renderSeedVCBackVocals (const juce::Uuid& trackId,
                                 std::function<void (bool success,
                                                     const SeedVCRenderedFiles& files,
                                                     const juce::String& error)> onComplete);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts_;
    ProjectState         projectState_;
    AudioEngine          audioEngine_;
    WORLDResynthesizer   worldResynthesizer_;  ///< Primary pitch correction (pyworld)
    RVCPythonBridge      rvcBridge_;           ///< Phase 2: voice conversion / back vocals
    SeedVCBridge         seedVCBridge_;         ///< Research SVC backend for offline backing vocals

    // ── Track file playback ──────────────────────────────────────────────────
    struct TrackPlayer
    {
        std::unique_ptr<juce::AudioFormatReader>       reader;
        std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
        std::unique_ptr<juce::AudioTransportSource>    transport;
        juce::File file;         ///< Currently playing file (may be a corrected temp)
        juce::File originalFile; ///< User's original audio — never overwritten
        float volume   { 1.0f };
        bool  muted    { false };
        bool  soloed   { false };
        bool  prepared { false };
        bool  midiAuditionEnabled { false };
        std::vector<MidiNote> detectedNotes;
    };

    juce::AudioFormatManager               playerFormatManager_;
    juce::TimeSliceThread                  readAheadThread_ { "so-readahead" };
    std::map<juce::Uuid, std::unique_ptr<TrackPlayer>> trackPlayers_;
    juce::CriticalSection                  playerLock_;
    juce::AudioBuffer<float>               mixBuf_;

    // ── Transport state ──────────────────────────────────────────────────────
    std::atomic<bool>   isPlaying_   { false };
    std::atomic<double> playbackPos_ { 0.0 };
    std::atomic<float>  cpuLoad_     { 0.0f };
    double              sampleRate_  { 44100.0 };
    int                 maxBlockSize_{ 512 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyntheticObsidianProcessor)
};
