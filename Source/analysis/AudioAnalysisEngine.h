#pragma once
#include <JuceHeader.h>
#include <optional>
#include <queue>
#include "../ui/PianoRollEditor.h"   // for MidiNote

/**  AudioAnalysisEngine
 *
 *  Runs audio analysis jobs on a background thread.
 *  Current capabilities (Phase 1):
 *    - Read any audio file via JUCE AudioFormatManager
 *    - Detect pitch frame-by-frame using the YIN algorithm
 *    - Merge pitched frames into MidiNote events
 *
 *  Results are posted back to the JUCE message thread via
 *  MessageManager::callAsync, so UI callbacks are always safe.
 *
 *  Phase 2 extensions: pYIN, WORLD vocoder, onset detection, BPM.
 */
class AudioAnalysisEngine : private juce::Thread
{
public:
    /** Callback fired on the message thread when analysis completes.
     *  @param trackId   The track UUID that was analysed.
     *  @param notes     Detected MIDI notes (empty if unvoiced / error).
     */
    using NotesReadyFn = std::function<void (const juce::Uuid&      trackId,
                                             std::vector<MidiNote>  notes)>;

    AudioAnalysisEngine();
    ~AudioAnalysisEngine() override;

    /** Queue an audio file for analysis. Returns immediately. */
    void analyzeFile (const juce::Uuid& trackId,
                      const juce::File& audioFile,
                      NotesReadyFn      callback);

    void setSensitivity (float sensitivity) noexcept;

    /** @returns The AudioFormatManager (caller may register extra formats). */
    juce::AudioFormatManager& getFormatManager() noexcept { return formatManager_; }

private:
    //==========================================================================
    void run() override;

    //==========================================================================
    // YIN pitch estimator — returns Hz, or 0 if unvoiced.
    // outConfidence ∈ [0,1]; threshold is typically 0.15.
    static float yinEstimate (const float* frame,
                               int          frameSize,
                               float        sampleRate,
                               float&       outConfidence) noexcept;

    //==========================================================================
    struct PitchFrame
    {
        double timeSeconds;
        int    midiPitch;    // -1 = unvoiced
        float  confidence;
        float  freqHz;       // 0 if unvoiced; used for cent-offset averaging
        float  rms;          // unwindowed frame RMS, used to bridge voiced gaps
    };

    /** Run YIN on every hop of the mono buffer. */
    std::vector<PitchFrame> detectPitchFrames (const juce::AudioBuffer<float>& mono,
                                               double sampleRate) const;

    /** Fill short unvoiced gaps and smooth frame pitch labels before note merge. */
    static std::vector<PitchFrame> stabilisePitchFrames (const std::vector<PitchFrame>& frames,
                                                         double hopSec);

    /** Merge adjacent same-pitch frames into note events. */
    static std::vector<MidiNote> mergeToNotes (const std::vector<PitchFrame>& frames,
                                               double hopSec);

    /** Preferred offline detector. Uses torchcrepe through local research venv. */
    static std::optional<std::vector<MidiNote>> analyzeWithPythonAi (const juce::File& audioFile,
                                                                     float sensitivity);

    /** Fallback offline detector. Uses librosa.pyin through local research venv. */
    static std::optional<std::vector<MidiNote>> analyzeWithPythonPyin (const juce::File& audioFile,
                                                                       float sensitivity);

    static std::optional<std::vector<MidiNote>> runPythonNoteAnalyzer (const juce::File& audioFile,
                                                                       const juce::String& scriptName,
                                                                       float sensitivity);

    static juce::File findProjectRoot();

    //==========================================================================
    struct Job
    {
        juce::Uuid    trackId;
        juce::File    audioFile;
        NotesReadyFn  callback;
        float         sensitivity { 0.72f };
    };

    juce::AudioFormatManager formatManager_;
    juce::CriticalSection    jobsLock_;
    std::queue<Job>          jobs_;
    juce::WaitableEvent      workReady_;
    std::atomic<float>       sensitivity_ { 0.72f };

    // Analysis parameters
    static constexpr int   kFrameMs    = 50;    ///< analysis frame length (ms)
    static constexpr int   kHopMs      = 10;    ///< hop between frames (ms)
    static constexpr float kYinThresh  = 0.25f; ///< YIN d' absolute threshold (higher = more detections)
    static constexpr float kMinConfid  = 0.055f; ///< minimum per-frame confidence to keep
    static constexpr float kFillGapS   = 0.420f; ///< max energetic unvoiced gap to bridge inside a sung note
    static constexpr float kMergeGapS  = 0.260f; ///< max gap to merge same note (s)
    static constexpr float kMinNoteS   = 0.120f; ///< minimum note duration (s)
    static constexpr float kEnergyBridgeRatio = 0.035f; ///< bridge only gaps with audible vocal energy
    static constexpr int   kMedianRadiusFrames = 3; ///< 7-frame pitch median smooths vibrato crossings
    static constexpr int   kVibratoSemitoneTolerance = 1; ///< keep +/-1 semitone wobble in one note
    static constexpr int   kGapBridgeSemitoneTolerance = 2; ///< allow loose pitch continuity over dropouts

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioAnalysisEngine)
};
