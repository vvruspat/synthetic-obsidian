#pragma once
#include <JuceHeader.h>
#include "SyntheticObsidianLookAndFeel.h"

//==============================================================================
/** A single pitched note block displayed in the piano roll.
 *  Phase 1: populated by placeholder / demo data.
 *  Phase 2: populated by pYIN pitch detector running on the Guide / VOX tracks. */
struct MidiNote
{
    struct PitchPoint
    {
        float offsetSeconds { 0.0f };
        float midiPitch     { 60.0f };
    };

    static constexpr int kMaxPitchContourPoints = 64;

    int    midiPitch       { 60 };    ///< Current (target) MIDI note — edited by drag
    double startSeconds    { 0.0 };
    double durationSeconds { 0.25 };
    float  confidence      { 1.0f };  ///< Pitch-detector confidence 0..1

    /** Current target cents offset (−50…+50).  Modified by drag. */
    float  centsOffset     { 0.0f };

    /** Original detected MIDI pitch — set by analysis, never changed by drag.
     *  Used to compute the correction ratio: ratio = 2^((target−detected)/12). */
    int    origMidiPitch   { 60 };
    /** Original detected cents offset — same lifetime as origMidiPitch. */
    float  origCentsOffset { 0.0f };
    std::array<PitchPoint, kMaxPitchContourPoints> pitchContour {};
    int    pitchContourSize { 0 };
    bool   syllableStart   { false };
    juce::String lyric;

    juce::Colour colour    { juce::Colour (Theme::kAccentCyan) };
};

//==============================================================================
/** DAW-style piano roll.
 *
 *  Layout (left→right):
 *    [kPianoKeyWidth px | time ruler ]
 *    [ piano keys      | note grid   ]
 *
 *  The note grid shows:
 *   - Row per semitone (black/white key shading)
 *   - Bar + beat vertical grid lines (BPM-aware)
 *   - MidiNote rectangles with confidence alpha
 *   - Optional waveform thumbnail overlay (very subtle, bottom strip)
 *   - Animated playhead with triangle head
 *
 *  Mouse wheel:
 *   - Plain      → horizontal scroll
 *   - Cmd/Ctrl   → horizontal zoom (pivot at mouse X)
 *   - Shift      → vertical scroll (pitch range)
 *   - Alt/Option → vertical zoom (pitch range)
 */
class PianoRollEditor final : public juce::Component,
                              private juce::Timer
{
public:
    PianoRollEditor();
    ~PianoRollEditor() override;

    //==========================================================================
    void paint    (juce::Graphics&) override;
    void resized  () override;
    void mouseMove  (const juce::MouseEvent& e) override;
    void mouseDown  (const juce::MouseEvent& e) override;
    void mouseDrag  (const juce::MouseEvent& e) override;
    void mouseUp    (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&,
                         const juce::MouseWheelDetails&) override;

    //==========================================================================
    /** Move the playhead (safe to call from any thread). */
    void setPlayheadPosition (double positionInSeconds);

    /** Attach an AudioThumbnail for waveform overlay (may be nullptr). */
    void setAudioThumbnail (juce::AudioThumbnail* thumbnail);

    //==========================================================================
    /** Add a detected note (call from message thread after pitch detection). */
    void addNote    (const MidiNote& note);
    void clearNotes ();

    /** Show "No notes detected" overlay when analysis returned nothing. */
    void setNoNotesPlaceholder (bool show) noexcept { noNotesPlaceholder_ = show; repaint(); }

    /** Show animated spinner while YIN analysis is running. */
    void setAnalyzing (bool analysing) noexcept { isAnalyzing_ = analysing; repaint(); }

    /** Show applying-correction overlay while phase-vocoder is running. */
    void setProcessing (bool p) noexcept { isProcessing_ = p; repaint(); }

    /** Notify the piano roll whether an RVC voice preset is loaded.
     *  When false, a warning banner is shown before the first note edit. */
    void setHasVoicePreset (bool hasPreset) noexcept
    {
        hasVoicePreset_ = hasPreset;
        repaint();
    }
    bool hasVoicePreset() const noexcept { return hasVoicePreset_; }

    //==========================================================================
    /** Fired on mouseUp after a note has been dragged to a new pitch/time.
     *  Passes a snapshot of all current notes so the processor can recompute. */
    std::function<void (const std::vector<MidiNote>&)> onNotesEdited;
    std::function<void (float sensitivity)> onDetectionSensitivityChanged;

    //==========================================================================
    /** Scroll/zoom the time view. */
    void setVisibleTimeRange (double startSec, double endSec);
    double getViewStartSec () const noexcept { return viewStartSec_; }
    double getViewEndSec   () const noexcept { return viewEndSec_;   }

    /** Update BPM for grid line drawing. */
    void setBpm (double bpm) noexcept { bpm_ = bpm; repaint(); }

private:
    //==========================================================================
    void timerCallback() override;  // 30 Hz repaint for playhead animation

    // Sub-renderers
    void paintTimeRuler       (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintPianoKeys       (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintNoteGrid        (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintNotes           (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintWaveform        (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintNoNotesOverlay    (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintAnalyzingOverlay  (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintProcessingOverlay (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintNoPresetBanner    (juce::Graphics&, juce::Rectangle<int> bounds);
    void paintPlayhead          (juce::Graphics&, juce::Rectangle<int> bounds);

    // Coordinate helpers
    float pitchToY    (int midiPitch, juce::Rectangle<int> bounds) const noexcept;
    float secondsToX  (double sec,    juce::Rectangle<int> bounds) const noexcept;

    /** The content area (grid + notes), excluding piano keys and time ruler. */
    juce::Rectangle<int> noteAreaBounds() const noexcept;

    /** Index into notes_ of the note under (gx, gy) in component coords.
     *  Returns -1 if nothing is hit. */
    int noteIndexAt (float gx, float gy) const noexcept;

    /** Human-readable note name, e.g. "A4", "C#3". */
    static juce::String midiNoteName (int midiPitch) noexcept;

    void scrollPitchView (int semitones);
    void zoomPitchView (double factor, int pivotPitch);
    int yToPitch (int y) const noexcept;

    //==========================================================================
    // Drag state — all on message thread, no lock needed
    struct NoteDrag
    {
        int    noteIdx   { -1 };
        double origStart { 0.0 };
        int    origPitch { 60 };
        float  origCents { 0.0f };
        int    mouseDownX { 0 };
        int    mouseDownY { 0 };
        bool   active    { false };
    };
    NoteDrag drag_;

    //==========================================================================
    juce::Array<MidiNote> notes_;
    juce::CriticalSection notesLock_;
    bool  noNotesPlaceholder_ { false };
    bool  isAnalyzing_        { false };
    bool  isProcessing_       { false };  ///< phase-vocoder / RVC running
    bool  hasVoicePreset_     { false };  ///< RVC preset loaded — hides fallback warning
    float spinnerAngle_       { 0.0f };

    std::atomic<double>   playheadPos_ { 0.0 };
    juce::AudioThumbnail* thumbnail_   { nullptr };

    juce::TextButton yZoomInBtn_  { "Y+" };
    juce::TextButton yZoomOutBtn_ { "Y-" };
    juce::TextButton yUpBtn_      { "^" };
    juce::TextButton yDownBtn_    { "v" };
    juce::Label      sensitivityLabel_ { {}, "DET" };
    juce::Slider     sensitivitySlider_;

    // View state (message-thread only)
    double viewStartSec_ { 0.0 };
    double viewEndSec_   { 8.0 };   // 8 seconds visible initially
    int    topNote_      { 84 };    // C6 at top row
    int    bottomNote_   { 36 };    // C2 at bottom row
    double bpm_          { 120.0 };

    static constexpr int kPianoKeyWidth = 52;
    static constexpr int kTimeRulerH    = 22;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollEditor)
};
