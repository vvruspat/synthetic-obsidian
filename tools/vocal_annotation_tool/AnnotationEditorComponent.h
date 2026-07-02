#pragma once

#include "AnnotationDocument.h"

namespace vocal_annotation
{

class AnnotationEditorComponent final : public juce::Component
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void beginUndoableAction() = 0;
        virtual void annotationChanged() = 0;
        virtual void selectionChanged(const juce::String& selectedNoteId, const juce::String& selectedBoundaryId) = 0;
        virtual void noteAuditionRequested(const juce::String& noteId) = 0;
        virtual void notesRecalculationRequested(const juce::StringArray& noteIds) = 0;
        virtual void waveformAuditionRequested(double startTime, double endTime) = 0;
    };

    AnnotationEditorComponent(juce::AudioFormatManager& formatManager, AnnotationDocument& document);

    void setListener(Listener* newListener);
    void setAudioFile(const juce::File& file);
    void setInstrumentalFile(const juce::File& file);
    void fitToClip();
    void setPlayheadTime(double seconds);
    void setHorizontalZoom(double zoom);
    void setPitchZoom(double zoom);
    void deleteSelection();
    void splitSelectedAtPlayhead();
    void mergeSelectedWithNext();
    void nudgeSelectedPitch(int semitones);
    void nudgeSelectedTime(double seconds);
    void jumpToNextSuspicious(bool reverse);
    void setSelectedBoundaryKind(BoundaryKind kind);
    void clearSelection();
    juce::String getSelectedNoteId() const;
    juce::String getSelectedBoundaryId() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    struct TimeRange
    {
        double start = 0.0;
        double end = 0.0;
    };

    enum class DragMode
    {
        none,
        moveNote,
        resizeNoteStart,
        resizeNoteEnd,
        moveBoundary
    };

    juce::Rectangle<int> getTimelineBounds() const;
    juce::Rectangle<int> getKeyboardBounds() const;
    juce::Rectangle<int> getEditorBounds() const;
    juce::Rectangle<int> getVocalEditorBounds() const;
    juce::Rectangle<int> getInstrumentalHeaderBounds() const;
    juce::Rectangle<int> getInstrumentalTrackBounds() const;
    juce::Rectangle<int> getLyricsBounds() const;

    double xToTime(float x) const;
    float timeToX(double time) const;
    int yToPitch(float y) const;
    float pitchToY(double pitch) const;
    juce::Rectangle<float> noteBoundsFor(const NoteBlock& note) const;
    void scrollTime(double deltaSeconds);
    void scrollPitch(double deltaSemitones);
    void zoomTimeAt(double pivotTime, double scaleFactor);
    void zoomPitchAt(double pivotPitch, double scaleFactor);
    std::optional<int> noteAt(juce::Point<float> position) const;
    std::optional<int> boundaryAt(juce::Point<float> position) const;
    std::optional<int> waveformPartBoundaryAt(juce::Point<float> position) const;
    std::optional<int> waveformPartNoteAt(juce::Point<float> position) const;
    bool waveformContains(juce::Point<float> position) const;
    void selectNote(int noteIndex);
    void selectBoundary(int boundaryIndex);
    void notifyChanged();
    void clampView();
    void sortNotes();
    void editSelectedNoteLyric();
    void addBoundaryAt(double time);
    void createNoteAt(juce::Point<float> position);
    void showNoteContextMenu(int noteIndex);
    void splitNoteAt(int noteIndex, double splitTime);
    static double noteMidiAtTime(const NoteBlock& note, double time);
    static void recalculateNotePitchFromCurve(NoteBlock& note);
    void drawPianoKeyboard(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawTimeline(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawInstrumentalHeader(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawMusicalGrid(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawMusicalContext(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawWaveform(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawInstrumentalTrack(juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawSelectedWaveformBorder(juce::Graphics& g, juce::Rectangle<int> waveformBounds);
    void drawNotes(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawBoundaries(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawSplitPreview(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawLyrics(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    std::optional<TimeRange> getSelectedTimeRange() const;

    AnnotationDocument& document_;
    juce::AudioFormatManager& formatManager_;
    juce::AudioThumbnailCache thumbnailCache_;
    juce::AudioThumbnail thumbnail_;
    juce::AudioThumbnail instrumentalThumbnail_;
    Listener* listener_ = nullptr;

    double visibleStart_ = 0.0;
    double visibleEnd_ = 10.0;
    double pitchCenter_ = 60.0;
    double visibleSemitones_ = 24.0;
    double playheadTime_ = 0.0;

    juce::String selectedNoteId_;
    juce::String selectedBoundaryId_;
    juce::String splitPreviewNoteId_;
    DragMode dragMode_ = DragMode::none;
    bool splitPreviewActive_ = false;
    double splitPreviewTime_ = 0.0;
    double dragStartTime_ = 0.0;
    int dragStartPitch_ = 60;
    double originalNoteStart_ = 0.0;
    double originalNoteEnd_ = 0.0;
    int originalNotePitch_ = 60;
    int lastDragAuditionPitch_ = -1;
    NoteBlock originalNoteSnapshot_;
    double originalBoundaryTime_ = 0.0;
};

} // namespace vocal_annotation
