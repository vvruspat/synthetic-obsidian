#include "Timeline.h"
#include "PitchEditor.h"

namespace obsidian::editor
{
namespace
{
juce::MouseEvent mouseDownAt(juce::Component& component, juce::Point<float> point)
{
    const auto time = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), point,
             juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
             &component, &component, time, point, time, 1, false };
}
class SurfaceTests final : public juce::UnitTest
{
public:
    SurfaceTests() : UnitTest("Editor surfaces", "Obsidian") {}
    void runTest() override
    {
        juce::Component testRoot;
        testRoot.setBounds(-2000, -2000, 800, 600);
        testRoot.addToDesktop(0);
        testRoot.setVisible(true);
        beginTest("Shared beat coordinates, pan and anchored zoom");
        TimeViewport viewport;
        expectWithinAbsoluteError(viewport.xForBeat(196.0), 60.0f, 0.001f);
        expectWithinAbsoluteError(viewport.beatAtX(60.0f), 196.0, 0.001);
        const auto anchorBeat = viewport.beatAtX(120.0f);
        viewport.zoomAt(2.0, 120.0f);
        expectWithinAbsoluteError(viewport.beatAtX(120.0f), anchorBeat, 0.001);
        viewport.scroll(4.0);
        expectWithinAbsoluteError(viewport.beatAtX(120.0f), anchorBeat + 4.0, 0.001);

        beginTest("Signature changes preserve cumulative bar numbering and eighth-note divisions");
        TimeViewport metric;
        metric.signatures = { { 0, 4, 4 }, { 8, 6, 8 }, { 14, 3, 4 } };
        expectEquals(static_cast<int>(metric.positionAt(8).bar), 3);
        expectEquals(metric.positionAt(8.5).beat, 2);
        expectEquals(static_cast<int>(metric.positionAt(11).bar), 4);
        expectEquals(static_cast<int>(metric.positionAt(14).bar), 5);
        expectEquals(metric.positionAt(15).beat, 2);
        std::vector<double> bars;
        metric.forEachGridLine(7, 17, 1, [&](double beat, bool bar) { if (bar) bars.push_back(beat); });
        expect(bars == std::vector<double>({ 8, 11, 14, 17 }));

        beginTest("Waveform cache rebuilds only when source or geometry changes");
        viewport = {};
        AudioClip clip(viewport);
        clip.setSize(500, 70);
        AudioClipData clipData;
        clipData.startBeat = 190.0;
        clipData.lengthBeats = 8.0;
        clipData.peaks = { 0.1f, 0.8f, 0.3f, 0.2f, 0.9f, 0.2f };
        clip.setData(std::move(clipData));
        expectWithinAbsoluteError(clip.clipBounds().getX(), -30.0f, 0.001f);
        expectWithinAbsoluteError(clip.clipBounds().getWidth(), 120.0f, 0.001f);
        const auto cacheRevision = clip.geometryRevision();
        clip.setSelected(true);
        juce::Image canvas(juce::Image::ARGB, 500, 300, true);
        juce::Graphics graphics(canvas);
        clip.paint(graphics);
        clip.paint(graphics);
        expectEquals(static_cast<int>(clip.geometryRevision()), static_cast<int>(cacheRevision));
        viewport.scroll(2.0);
        clip.viewportChanged();
        expect(clip.geometryRevision() > cacheRevision);
        expectWithinAbsoluteError(clip.clipBounds().getX(), -60.0f, 0.001f);

        beginTest("Note hit-testing, overlapping selection and silent programmatic updates");
        viewport = {};
        PitchEditor pitch(viewport);
        pitch.setSize(600, 300);
        pitch.setNoteRange(46, 75);
        int notifications = 0;
        int selected = -1;
        pitch.onNoteSelected = [&](int id) { ++notifications; selected = id; };
        NoteSegment note;
        note.id = 7; note.startBeat = 196.0; note.lengthBeats = 2.0; note.midiNote = 60;
        note.pitchOffsets = { 0.1f, -0.1f, 0.2f, 0.0f };
        NoteSegment overlay = note; overlay.id = 8;
        NoteSegment hidden = note; hidden.id = 9; hidden.startBeat = 1000.0;
        pitch.setNotes({ note, overlay, hidden });
        expectWithinAbsoluteError(pitch.noteBounds(7).getX(), 60.0f, 0.001f);
        expectWithinAbsoluteError(pitch.noteBounds(7).getCentreY(), 155.0f, 0.001f);
        expectWithinAbsoluteError(pitch.noteBounds(7).getHeight(), 17.5f, 0.001f);
        expectEquals(pitch.hitTestNote(pitch.noteBounds(7).getCentre()), 8);
        expectEquals(pitch.hitTestNote({ 0.0f, 0.0f }), -1);
        expectEquals(pitch.hitTestNote(pitch.noteBounds(9).getCentre()), -1);
        pitch.setSelectedNote(7);
        expectEquals(notifications, 0);
        const auto pitchCacheRevision = pitch.geometryRevision();
        pitch.paint(graphics);
        pitch.paint(graphics);
        expectEquals(static_cast<int>(pitch.geometryRevision()), static_cast<int>(pitchCacheRevision));
        expect(pitch.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
        expectEquals(selected, 8);
        expectEquals(notifications, 1);
        expect(pitch.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        expectEquals(selected, -1);
        expectEquals(notifications, 2);
        viewport.zoomAt(2.0, 0.0f);
        pitch.viewportChanged();
        expectWithinAbsoluteError(pitch.noteBounds(7).getX(), 120.0f, 0.001f);
        expectEquals(pitch.hitTestNote(pitch.noteBounds(7).getCentre()), 8);

        beginTest("Pointer gestures commit once and cancellation restores note geometry");
        viewport = {};
        PitchEditor draggable(viewport);
        draggable.setSize(600, 300);
        draggable.setNotes({ note });
        testRoot.addAndMakeVisible(draggable);
        int commits = 0;
        double committedStart = 0, committedLength = 0;
        int committedPitch = 0;
        draggable.onNoteMoved = [&](int id, double start, double length, int midi)
        { expectEquals(id, 7); ++commits; committedStart = start; committedLength = length; committedPitch = midi; };
        const auto original = draggable.noteBounds(7);
        const auto down = mouseDownAt(draggable, original.getCentre());
        draggable.mouseDown(down);
        draggable.mouseDrag(mouseDownAt(draggable, original.getCentre() + juce::Point<float>(15, -10)));
        expectEquals(commits, 0);
        draggable.mouseUp(down);
        expectEquals(commits, 1);
        expectWithinAbsoluteError(committedStart, 197.0, 0.001);
        expectWithinAbsoluteError(committedLength, 2.0, 0.001);
        expectEquals(committedPitch, 61);
        const auto beforeCancel = draggable.noteBounds(7);
        draggable.mouseDown(mouseDownAt(draggable, beforeCancel.getCentre()));
        draggable.mouseDrag(mouseDownAt(draggable, beforeCancel.getCentre() + juce::Point<float>(30, 20)));
        draggable.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        draggable.mouseUp(down);
        expectEquals(commits, 1);
        expect(draggable.noteBounds(7) == beforeCancel);
        const auto edge = beforeCancel.getCentre().withX(beforeCancel.getRight() - 1.0f);
        draggable.mouseDown(mouseDownAt(draggable, edge));
        draggable.mouseDrag(mouseDownAt(draggable, edge + juce::Point<float>(15, 0)));
        draggable.mouseUp(down);
        expectEquals(commits, 2);
        expectWithinAbsoluteError(committedLength, 3.0, 0.001);
        const auto beforeHide = draggable.noteBounds(7);
        draggable.mouseDown(mouseDownAt(draggable, beforeHide.getCentre()));
        draggable.mouseDrag(mouseDownAt(draggable, beforeHide.getCentre() + juce::Point<float>(15, 0)));
        draggable.setVisible(false);
        expect(draggable.noteBounds(7) == beforeHide);
        draggable.mouseUp(down);
        expectEquals(commits, 2);

        beginTest("Multiple selected notes move together in one committed callback");
        NoteSegment second = note; second.id = 11; second.startBeat += 3; second.midiNote = 64;
        draggable.setVisible(true);
        draggable.setNotes({ note, second });
        draggable.setSelectedNotes({ 7, 11 });
        int batches = 0;
        std::vector<PitchEditor::NoteMovement> movements;
        draggable.onNotesMoved = [&](const auto& values) { ++batches; movements = values; };
        const auto centre = draggable.noteBounds(7).getCentre();
        draggable.mouseDown(mouseDownAt(draggable, centre));
        draggable.mouseDrag(mouseDownAt(draggable, centre + juce::Point<float>(15, -10)));
        draggable.mouseUp(down);
        expectEquals(batches, 1); expectEquals(static_cast<int>(movements.size()), 2);
        if (movements.size() == 2)
        {
            expectWithinAbsoluteError(movements[0].startBeat, 197.0, 0.001);
            expectWithinAbsoluteError(movements[1].startBeat, 200.0, 0.001);
            expectEquals(movements[0].midiNote, 61); expectEquals(movements[1].midiNote, 65);
        }

        beginTest("Keyboard seeking follows the owner position across surfaces");
        TimelineRuler firstRuler(viewport), secondRuler(viewport);
        double ownerBeat = 210.25;
        int seeks = 0;
        const auto synchronise = [&](double beat)
        {
            ownerBeat = beat;
            firstRuler.setCurrentBeat(beat);
            secondRuler.setCurrentBeat(beat);
            ++seeks;
        };
        firstRuler.onSeek = synchronise;
        secondRuler.onSeek = synchronise;
        firstRuler.setCurrentBeat(ownerBeat);
        secondRuler.setCurrentBeat(ownerBeat);
        expectEquals(seeks, 0);
        expect(firstRuler.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        expectWithinAbsoluteError(ownerBeat, 211.25, 0.001);
        expect(secondRuler.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        expectWithinAbsoluteError(ownerBeat, 212.25, 0.001);
        expectEquals(seeks, 2);

        beginTest("Selection callbacks may destroy the editor before empty-space seeking");
        auto disposablePitch = std::make_unique<PitchEditor>(viewport);
        disposablePitch->setSize(600, 300);
        testRoot.addAndMakeVisible(*disposablePitch);
        int selectionCalls = 0, seekAfterDeletion = 0;
        disposablePitch->onSeek = [&](double) { ++seekAfterDeletion; };
        const auto callbackPayload = std::make_shared<int>(17);
        disposablePitch->onNoteSelected = [&, payload = callbackPayload](int id)
        {
            expectEquals(id, -1);
            disposablePitch.reset();
            // The callback's copied closure must remain alive after its component.
            expectEquals(*payload, 17);
            ++selectionCalls;
        };
        const auto emptyClick = mouseDownAt(*disposablePitch, { 23.0f, 30.0f });
        disposablePitch->mouseDown(emptyClick);
        expect(disposablePitch == nullptr);
        expectEquals(selectionCalls, 1);
        expectEquals(seekAfterDeletion, 0);

        beginTest("Seek and clip selection callbacks may destroy their source surface");
        auto disposableRuler = std::make_unique<TimelineRuler>(viewport);
        disposableRuler->onSeek = [&, payload = callbackPayload](double)
        {
            disposableRuler.reset();
            expectEquals(*payload, 17);
        };
        expect(disposableRuler->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        expect(disposableRuler == nullptr);
        auto disposableClip = std::make_unique<AudioClip>(viewport);
        disposableClip->onSelect = [&, payload = callbackPayload]
        {
            disposableClip.reset();
            expectEquals(*payload, 17);
        };
        expect(disposableClip->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        expect(disposableClip == nullptr);

        beginTest("Snap off preserves the exact clicked beat");
        viewport = {};
        PitchEditor pencil(viewport);
        pencil.setSize(600, 300);
        testRoot.addAndMakeVisible(pencil);
        pencil.setTool(PitchTool::pencil);
        double editedBeat = -1.0;
        pencil.onEditRequested = [&](PitchTool, int, double beat, int) { editedBeat = beat; };
        const auto pencilClick = mouseDownAt(pencil, { 23.0f, 30.0f });
        pencil.setSnapBeats(0.0);
        pencil.mouseDown(pencilClick);
        expectWithinAbsoluteError(editedBeat, viewport.beatAtX(23.0f), 0.0001);
        pencil.setSnapBeats(0.25);
        pencil.mouseDown(pencilClick);
        expectWithinAbsoluteError(editedBeat, 193.5, 0.0001);

        beginTest("Invalid clip duration produces an empty surface");
        AudioClipData invalid;
        invalid.lengthBeats = -2.0;
        clip.setData(std::move(invalid));
        expect(clip.clipBounds().isEmpty());

        beginTest("Playhead programmatic changes do not emit seek callbacks");
        Playhead playhead(viewport);
        playhead.setBeat(208.0);
        expectWithinAbsoluteError(playhead.beat(), 208.0, 0.001);
        playhead.setBeat(-1.0);
        expectWithinAbsoluteError(playhead.beat(), 0.0, 0.001);
    }
};
SurfaceTests surfaceTests;
}
}
