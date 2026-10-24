#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "Timeline.h"

namespace obsidian::editor
{
class PianoKeyboard final : public juce::Component
{
public:
    PianoKeyboard();
    void setNoteRange(int lowestMidiNote, int highestMidiNote);
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
};

enum class PitchTool { pointer, pencil, scissors };

class PitchEditor final : public EditorSurface
{
public:
    explicit PitchEditor(const TimeViewport&);
    void setNotes(std::vector<NoteSegment>);
    void setNoteRange(int lowestMidiNote, int highestMidiNote);
    void setSelectedNote(int id);
    int selectedNote() const noexcept { return selectedNote_; }
    void setTool(PitchTool);
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
    bool keyPressed(const juce::KeyPress&) override;
private:
    struct CachedNote
    {
        int id = 0;
        juce::Rectangle<float> bounds;
        juce::Path contour;
        juce::Colour colour;
        bool ghost = false;
    };
    void rebuildGeometry();
    int midiNoteAt(float y) const noexcept;
    void selectFromUser(int id);
    std::vector<NoteSegment> notes_;
    std::vector<CachedNote> visibleNotes_;
    juce::Path minorGrid_, majorGrid_, rowGrid_;
    int lowest_ = 46, highest_ = 75, selectedNote_ = -1;
    double snapBeats_ = 0.25;
    PitchTool tool_ = PitchTool::pointer;
    std::size_t geometryRevision_ = 0;
};
}
