#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <limits>
#include <vector>
#include "Timeline.h"

namespace obsidian::editor
{
class PianoKeyboard final : public juce::Component
{
public:
    PianoKeyboard();
    void setNoteRange(int lowestMidiNote, int highestMidiNote);
    juce::Rectangle<float> keyBoundsForMidiNote(int midiNote) const noexcept;
    void paint(juce::Graphics&) override;
private:
    int lowest_ = 46, highest_ = 75;
};

struct NoteSegment
{
    int id = 0;
    double startBeat = 0.0, lengthBeats = 1.0;
    int midiNote = 60;
    // Uniformly spaced semitone offsets, supplied by the caller, never analysed here.
    std::vector<float> pitchOffsets;
    juce::Colour colour { 0xffb5c89b };
    bool ghost = false;
    double gainDb = 0.0;
    // Exact semitone pitch; NaN preserves the integer-only API for existing users.
    double pitch = std::numeric_limits<double>::quiet_NaN();
};

enum class PitchTool { pointer, pencil, eraser, scissors, join, flex, vibrato, gain, zoom };

class PitchEditor final : public EditorSurface, private juce::Timer
{
public:
    explicit PitchEditor(const TimeViewport&);
    ~PitchEditor() override;
    void setNotes(std::vector<NoteSegment>);
    void setNoteRange(int lowestMidiNote, int highestMidiNote);
    void setSelectedNote(int id);
    int selectedNote() const noexcept { return selectedNote_; }
    void setSelectedNotes(std::vector<int> ids);
    const std::vector<int>& selectedNotes() const noexcept { return selectedNotes_; }
    std::function<void(const std::vector<int>&)> onNotesSelected;
    struct NoteMovement
    {
        int id;
        double startBeat, lengthBeats;
        int midiNote;
        double pitch = std::numeric_limits<double>::quiet_NaN();
    };
    std::function<void(const std::vector<NoteMovement>&)> onNotesMoved;
    void setTool(PitchTool);
    void setSecondaryTool(PitchTool);
    void setReadOnly(bool);
    bool isReadOnly() const noexcept { return readOnly_; }
    PitchTool getTool() const noexcept { return tool_; }
    PitchTool getSecondaryTool() const noexcept { return secondaryTool_; }
    void setMinimumNoteLengthBeats(double);
    std::function<double(double startBeat)> defaultNoteLengthBeats;
    // Optional owner-provided pitch policy, e.g. snapping to the chord at this beat.
    std::function<double(double requestedPitch, double startBeat)> pitchTransform;
    std::function<void(double startBeat, double lengthBeats, double pitch)> onNoteCreated;
    // Vibrato commits a relative scale [0,4], gain an absolute dB value [-24,12].
    std::function<void(PitchTool, int, double)> onNoteExpressionEdited;
    std::function<void(double pitch, bool restart)> onPitchPreview;
    std::function<void()> onPitchPreviewEnd;
    std::function<void(const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheelGesture;
    // Zero disables snapping; positive values are quarter-note beat divisions.
    void setSnapBeats(double);
    int hitTestNote(juce::Point<float>) const noexcept;
    juce::Rectangle<float> noteBounds(int id) const noexcept;
    std::size_t geometryRevision() const noexcept { return geometryRevision_; }
    std::function<void(int)> onNoteSelected;
    // Edit intent only; the owner updates the demo/project data through setNotes().
    std::function<void(PitchTool, int, double, int)> onEditRequested;
    void resized() override;
    void viewportChanged() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void focusLost(FocusChangeType) override;
    void visibilityChanged() override;
    void enablementChanged() override;
    // One committed move/edge resize per pointer gesture; model setters are silent.
    std::function<void(int, double, double, int)> onNoteMoved;
    bool keyPressed(const juce::KeyPress&) override;
private:
    struct CachedNote
    {
        int id = 0;
        juce::Rectangle<float> bounds, visualBounds;
        juce::Path contour;
        juce::Colour colour;
        bool ghost = false;
        double gainDb = 0.0;
    };
    void rebuildGeometry();
    void cancelNoteDrag();
    int midiNoteAt(float y) const noexcept;
    double pitchAt(float y) const noexcept;
    void timerCallback() override;
    void endPitchPreview();
    double snappedBeat(float x) const noexcept;
    double transformPitch(double requestedPitch, double startBeat) const;
    void selectFromUser(int id, bool extend = false);
    std::vector<NoteSegment> notes_;
    std::vector<CachedNote> visibleNotes_;
    juce::Path minorGrid_, majorGrid_, rowGrid_;
    int lowest_ = 46, highest_ = 75, selectedNote_ = -1;
    double snapBeats_ = 0.25, minimumLengthBeats_ = 0.01;
    PitchTool tool_ = PitchTool::pointer, secondaryTool_ = PitchTool::scissors;
    std::size_t geometryRevision_ = 0;
    struct NoteDrag
    {
        int id = -1, edge = 0;
        double start = 0.0, length = 1.0;
        double pitch = 60.0, gainDb = 0.0, expression = 1.0;
        PitchTool tool = PitchTool::pointer;
        juce::Point<float> origin;
        bool moved = false;
    };
    NoteDrag drag_;
    std::vector<int> selectedNotes_;
    std::vector<NoteSegment> dragNotes_;
    struct Marquee
    {
        bool active = false, moved = false;
        juce::Point<float> origin;
        juce::Rectangle<float> bounds;
        std::vector<int> baseSelection;
    };
    Marquee marquee_;
    struct Pencil
    {
        bool active = false;
        double anchor = 0.0, start = 0.0, length = 0.5, pitch = 60.0;
        juce::Point<float> origin;
        double requestedPitch = 60.0;
    };
    Pencil pencil_;
    bool previewActive_ = false, readOnly_ = false;
    double lastPreviewPitch_ = std::numeric_limits<double>::quiet_NaN();
};
}
