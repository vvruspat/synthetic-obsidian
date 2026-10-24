#include "Timeline.h"
#include "PitchEditor.h"

#include <cstring>

namespace obsidian::editor
{
namespace
{
juce::MouseEvent mouseDownAt(juce::Component& component, juce::Point<float> point, int modifiers = juce::ModifierKeys::leftButtonModifier)
{
    const auto time = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), point,
             juce::ModifierKeys(modifiers), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
             &component, &component, time, point, time, 1, false };
}
bool samePixels(const juce::Image& left, const juce::Image& right)
{
    if (left.getFormat() != right.getFormat() || left.getBounds() != right.getBounds()) return false;
    const juce::Image::BitmapData leftPixels(left, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData rightPixels(right, juce::Image::BitmapData::readOnly);
    const auto bytes = static_cast<std::size_t>(left.getWidth() * leftPixels.pixelStride);
    for (int y = 0; y < left.getHeight(); ++y)
        if (std::memcmp(leftPixels.getLinePointer(y), rightPixels.getLinePointer(y), bytes) != 0) return false;
    return true;
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
        expectWithinAbsoluteError(committedStart, 196.0, 0.001);
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
        draggable.setTool(PitchTool::flex);
        draggable.mouseDown(mouseDownAt(draggable, edge));
        draggable.mouseDrag(mouseDownAt(draggable, edge + juce::Point<float>(15, 0)));
        draggable.mouseUp(down);
        expectEquals(commits, 2);
        expectWithinAbsoluteError(committedLength, 3.0, 0.001);
        draggable.setTool(PitchTool::pointer);
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
            expectWithinAbsoluteError(movements[0].startBeat, 196.0, 0.001);
            expectWithinAbsoluteError(movements[1].startBeat, 199.0, 0.001);
            expectEquals(movements[0].midiNote, 61); expectEquals(movements[1].midiNote, 65);
        }

        beginTest("Pointer preserves time, moves fractional pitch and auditions one gesture");
        PitchEditor tools(viewport);
        tools.setSize(600, 300); testRoot.addAndMakeVisible(tools);
        NoteSegment exact = note; exact.pitch = 60.25; exact.gainDb = -6.0;
        tools.setNotes({ exact }); tools.setSnapBeats(0.0);
        auto moveCount = 0, previewStarts = 0, previewUpdates = 0, previewStops = 0;
        std::vector<PitchEditor::NoteMovement> exactMoves;
        tools.onNotesMoved = [&](const auto& values) { ++moveCount; exactMoves = values; };
        tools.onPitchPreview = [&](double previewPitch, bool restart)
        {
            if (restart) { ++previewStarts; expectWithinAbsoluteError(previewPitch, 60.25, 0.0001); }
            else { ++previewUpdates; expectWithinAbsoluteError(previewPitch, 60.5, 0.0001); }
        };
        tools.onPitchPreviewEnd = [&] { ++previewStops; };
        auto toolCentre = tools.noteBounds(7).getCentre();
        tools.mouseDown(mouseDownAt(tools, toolCentre));
        tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(30, -2.5f)));
        expectEquals(moveCount, 0); tools.mouseUp(mouseDownAt(tools, toolCentre));
        expectEquals(moveCount, 1); expectEquals(previewStarts, 1); expectEquals(previewUpdates, 1); expectEquals(previewStops, 1);
        if (!exactMoves.empty())
        {
            expectWithinAbsoluteError(exactMoves.front().pitch, 60.5, 0.0001);
            expectWithinAbsoluteError(exactMoves.front().startBeat, exact.startBeat, 0.0001);
            expectWithinAbsoluteError(exactMoves.front().lengthBeats, exact.lengthBeats, 0.0001);
        }
        tools.onPitchPreview = [&](double, bool restart) { if (restart) ++previewStarts; };
        tools.setNotes({ exact }); toolCentre = tools.noteBounds(7).getCentre();
        tools.mouseDown(mouseDownAt(tools, toolCentre));
        tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(60, 0)));
        tools.mouseUp(mouseDownAt(tools, toolCentre));
        expectEquals(moveCount, 1); expectEquals(previewStops, 2);
        tools.mouseDown(mouseDownAt(tools, toolCentre));
        tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -10)));
        tools.mouseDrag(mouseDownAt(tools, toolCentre)); tools.mouseUp(mouseDownAt(tools, toolCentre));
        expectEquals(moveCount, 1); expectEquals(previewStops, 3);

        beginTest("Owner pitch transforms drive local previews, group commits and audition");
        PitchEditor snapping(viewport); snapping.setSize(600, 300); testRoot.addAndMakeVisible(snapping); snapping.setSnapBeats(0.0);
        NoteSegment exactSecond = second; exactSecond.pitch = 64.25;
        snapping.setNotes({ exact, exactSecond }); snapping.setSelectedNotes({ 7, 11 });
        auto transformCalls = 0, snappedCommits = 0; double snappedAudition = 0.0;
        std::vector<PitchEditor::NoteMovement> snappedMoves;
        snapping.pitchTransform = [&](double requested, double beat)
        { ++transformCalls; return beat < 198.0 ? std::round(requested) : std::ceil(requested); };
        snapping.onPitchPreview = [&](double value, bool) { snappedAudition = value; };
        snapping.onNotesMoved = [&](const auto& values) { ++snappedCommits; snappedMoves = values; };
        const auto snappedCentre = snapping.noteBounds(7).getCentre();
        snapping.mouseDown(mouseDownAt(snapping, snappedCentre));
        snapping.mouseDrag(mouseDownAt(snapping, snappedCentre + juce::Point<float>(20, -2.5f)));
        expectEquals(transformCalls, 2); expectEquals(snappedCommits, 0);
        expectWithinAbsoluteError(snapping.noteBounds(7).getCentreY(), 145.0f, 0.0001f);
        expectWithinAbsoluteError(snappedAudition, 61.0, 0.0001);
        snapping.mouseUp(mouseDownAt(snapping, snappedCentre));
        expectEquals(snappedCommits, 1); expectEquals(static_cast<int>(snappedMoves.size()), 2);
        if (snappedMoves.size() == 2)
        {
            expectWithinAbsoluteError(snappedMoves[0].pitch, 61.0, 0.0001);
            expectWithinAbsoluteError(snappedMoves[1].pitch, 65.0, 0.0001);
            expectWithinAbsoluteError(snappedMoves[0].startBeat, exact.startBeat, 0.0001);
        }
        snapping.setNotes({ exact, exactSecond }); snapping.setSelectedNotes({ 7, 11 });
        snapping.pitchTransform = [](double, double beat) { return beat < 198.0 ? 60.25 : 64.25; };
        snapping.mouseDown(mouseDownAt(snapping, snappedCentre));
        snapping.mouseDrag(mouseDownAt(snapping, snappedCentre + juce::Point<float>(0, -2.5f)));
        snapping.mouseUp(mouseDownAt(snapping, snappedCentre)); expectEquals(snappedCommits, 1);
        snapping.setNotes({ exact }); snapping.setTool(PitchTool::flex);
        snapping.pitchTransform = [](double, double beat) { return beat < 197.0 ? 60.0 : 62.0; };
        const auto flexStart = snapping.noteBounds(7).getCentre().withX(snapping.noteBounds(7).getX() + 3.0f);
        snapping.mouseDown(mouseDownAt(snapping, flexStart));
        snapping.mouseDrag(mouseDownAt(snapping, flexStart + juce::Point<float>(15, 0)));
        snapping.mouseUp(mouseDownAt(snapping, flexStart)); expectEquals(snappedCommits, 2);
        if (!snappedMoves.empty())
        {
            expectWithinAbsoluteError(snappedMoves.front().startBeat, 197.0, 0.0001);
            expectWithinAbsoluteError(snappedMoves.front().pitch, 62.0, 0.0001);
        }
        snapping.setNotes({}); snapping.setTool(PitchTool::pencil);
        double transformedCreation = 0.0;
        snapping.onNoteCreated = [&](double, double, double value) { transformedCreation = value; };
        snapping.pitchTransform = [](double requested, double beat) { return requested + (beat < 195.0 ? 2.0 : 1.0); };
        snapping.mouseDown(mouseDownAt(snapping, { 60, 152.5f }));
        snapping.mouseDrag(mouseDownAt(snapping, { 30, 152.5f })); snapping.mouseUp(mouseDownAt(snapping, { 30, 152.5f }));
        expectWithinAbsoluteError(transformedCreation, 62.25, 0.0001);
        snapping.pitchTransform = [](double, double) { return 999.0; };
        snapping.mouseDown(mouseDownAt(snapping, { 60, 152.5f })); snapping.mouseUp(mouseDownAt(snapping, { 60, 152.5f }));
        expectWithinAbsoluteError(transformedCreation, 127.0, 0.0001);
        snapping.pitchTransform = [](double, double) { return std::numeric_limits<double>::quiet_NaN(); };
        snapping.mouseDown(mouseDownAt(snapping, { 60, 152.5f })); snapping.mouseUp(mouseDownAt(snapping, { 60, 152.5f }));
        expectWithinAbsoluteError(transformedCreation, 60.25, 0.0001);
        auto deletingTransform = std::make_unique<PitchEditor>(viewport);
        deletingTransform->setSize(600, 300); deletingTransform->setNotes({ exact }); testRoot.addAndMakeVisible(*deletingTransform);
        deletingTransform->pitchTransform = [&](double, double) { deletingTransform.reset(); return 60.0; };
        deletingTransform->mouseDown(mouseDownAt(*deletingTransform, snappedCentre));
        deletingTransform->mouseDrag(mouseDownAt(*deletingTransform, snappedCentre + juce::Point<float>(0, -10)));
        expect(deletingTransform == nullptr);

        beginTest("Modifier and right-click tools are latched for the complete gesture");
       #if JUCE_MAC
        constexpr auto secondaryModifier = juce::ModifierKeys::commandModifier;
       #else
        constexpr auto secondaryModifier = juce::ModifierKeys::ctrlModifier;
       #endif
        auto expressionCommits = 0; PitchTool expressionTool = PitchTool::pointer; double expressionValue = 0.0;
        tools.onNoteExpressionEdited = [&](PitchTool tool, int id, double value)
        { ++expressionCommits; expressionTool = tool; expressionValue = value; expectEquals(id, 7); };
        tools.setSecondaryTool(PitchTool::gain);
        tools.mouseDown(mouseDownAt(tools, toolCentre, juce::ModifierKeys::leftButtonModifier | secondaryModifier));
        tools.setSecondaryTool(PitchTool::vibrato); tools.setTool(PitchTool::eraser);
        tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -24)));
        expectEquals(expressionCommits, 0); tools.mouseUp(mouseDownAt(tools, toolCentre));
        expectEquals(expressionCommits, 1); expect(expressionTool == PitchTool::gain);
        expectWithinAbsoluteError(expressionValue, -3.0, 0.0001);
        tools.setNotes({ exact }); tools.setTool(PitchTool::pointer); tools.setSecondaryTool(PitchTool::scissors);
        auto immediateCount = 0; PitchTool immediateTool = PitchTool::pointer;
        tools.onEditRequested = [&](PitchTool tool, int id, double, int) { ++immediateCount; immediateTool = tool; expectEquals(id, 7); };
        tools.mouseDown(mouseDownAt(tools, toolCentre, juce::ModifierKeys::rightButtonModifier));
        expectEquals(immediateCount, 1); expect(immediateTool == PitchTool::scissors);
        for (const auto tool : { PitchTool::eraser, PitchTool::join })
        {
            tools.setTool(tool); tools.mouseDown(mouseDownAt(tools, toolCentre)); expect(immediateTool == tool);
        }
        expectEquals(immediateCount, 3);
       #if JUCE_MAC
        tools.setTool(PitchTool::pointer);
        tools.mouseDown(mouseDownAt(tools, toolCentre, juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier));
        tools.mouseUp(mouseDownAt(tools, toolCentre)); expectEquals(immediateCount, 3);
       #endif

        beginTest("Gain and vibrato preview locally, restore on cancellation and commit once");
        tools.setNotes({ exact }); tools.setSelectedNote(7); tools.setTool(PitchTool::gain); tools.grabKeyboardFocus();
        const auto expressionBaseline = tools.createComponentSnapshot(tools.getLocalBounds());
        tools.mouseDown(mouseDownAt(tools, toolCentre)); tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -32)));
        expectEquals(expressionCommits, 1);
        expect(!samePixels(expressionBaseline, tools.createComponentSnapshot(tools.getLocalBounds())));
        tools.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)); tools.setSelectedNote(7);
        expect(samePixels(expressionBaseline, tools.createComponentSnapshot(tools.getLocalBounds())));
        tools.mouseUp(mouseDownAt(tools, toolCentre)); expectEquals(expressionCommits, 1);
        tools.mouseDown(mouseDownAt(tools, toolCentre)); tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -24)));
        tools.mouseUp(mouseDownAt(tools, toolCentre)); expectEquals(expressionCommits, 2);
        expectWithinAbsoluteError(expressionValue, -3.0, 0.0001);
        tools.setNotes({ exact }); tools.setTool(PitchTool::vibrato); tools.setSelectedNote(7);
        const auto vibratoBaseline = tools.createComponentSnapshot(tools.getLocalBounds());
        tools.mouseDown(mouseDownAt(tools, toolCentre)); tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -64)));
        expect(!samePixels(vibratoBaseline, tools.createComponentSnapshot(tools.getLocalBounds())));
        tools.focusLost(juce::Component::focusChangedDirectly);
        expect(samePixels(vibratoBaseline, tools.createComponentSnapshot(tools.getLocalBounds())));
        tools.mouseUp(mouseDownAt(tools, toolCentre)); expectEquals(expressionCommits, 2);
        tools.mouseDown(mouseDownAt(tools, toolCentre)); tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -64)));
        tools.mouseUp(mouseDownAt(tools, toolCentre)); expectEquals(expressionCommits, 3);
        expect(expressionTool == PitchTool::vibrato); expectWithinAbsoluteError(expressionValue, 2.0, 0.0001);
        tools.setNotes({ exact }); tools.setTool(PitchTool::gain);
        tools.mouseDown(mouseDownAt(tools, toolCentre)); tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -500)));
        tools.mouseUp(mouseDownAt(tools, toolCentre)); expectWithinAbsoluteError(expressionValue, 12.0, 0.0001);
        tools.setNotes({ exact }); tools.mouseDown(mouseDownAt(tools, toolCentre));
        tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, 500)));
        tools.mouseUp(mouseDownAt(tools, toolCentre)); expectWithinAbsoluteError(expressionValue, -24.0, 0.0001);

        beginTest("Pencil creates fractional notes on release and drag length overrides click duration");
        tools.setNotes({}); tools.setTool(PitchTool::pencil); tools.setSnapBeats(0.0);
        auto creations = 0; double createdStart = 0, createdLength = 0, createdPitch = 0;
        tools.defaultNoteLengthBeats = [](double) { return 0.75; };
        tools.onNoteCreated = [&](double start, double length, double notePitch)
        { ++creations; createdStart = start; createdLength = length; createdPitch = notePitch; };
        const juce::Point<float> pencilOrigin(60.0f, 152.5f);
        tools.mouseDown(mouseDownAt(tools, pencilOrigin)); expectEquals(creations, 0);
        tools.mouseUp(mouseDownAt(tools, pencilOrigin)); expectEquals(creations, 1);
        expectWithinAbsoluteError(createdLength, 0.75, 0.0001); expectWithinAbsoluteError(createdPitch, 60.25, 0.0001);
        tools.mouseDown(mouseDownAt(tools, pencilOrigin));
        tools.mouseDrag(mouseDownAt(tools, pencilOrigin + juce::Point<float>(-30, 30)));
        tools.mouseUp(mouseDownAt(tools, pencilOrigin)); expectEquals(creations, 2);
        expectWithinAbsoluteError(createdStart, 194.0, 0.0001); expectWithinAbsoluteError(createdLength, 2.0, 0.0001);
        expectWithinAbsoluteError(createdPitch, 60.25, 0.0001);
        tools.mouseDown(mouseDownAt(tools, pencilOrigin)); tools.setEnabled(false); tools.mouseUp(mouseDownAt(tools, pencilOrigin));
        expectEquals(creations, 2); tools.setEnabled(true);

        beginTest("Pointer marquee selects multiple notes without moving them");
        tools.setNotes({ note, second }); tools.setTool(PitchTool::pointer); tools.setSelectedNotes({});
        std::vector<int> marqueeSelection;
        tools.onNotesSelected = [&](const auto& ids) { marqueeSelection = ids; };
        tools.mouseDown(mouseDownAt(tools, { 45, 100 })); tools.mouseDrag(mouseDownAt(tools, { 150, 170 }));
        tools.mouseUp(mouseDownAt(tools, { 150, 170 }));
        expect(marqueeSelection == std::vector<int>({ 7, 11 }));
        expectWithinAbsoluteError(tools.noteBounds(7).getX(), 60.0f, 0.0001f);

        beginTest("Zoom and wheel routing preserve the clicked anchor and modifier direction");
        tools.setTool(PitchTool::zoom); tools.setSecondaryTool(PitchTool::zoom);
        auto zoomCalls = 0; double zoomFactor = 0; float zoomAnchor = 0;
        tools.onViewportGesture = [&](double pan, double factor, float anchor)
        { ++zoomCalls; expectWithinAbsoluteError(pan, 0.0, 0.0001); zoomFactor = factor; zoomAnchor = anchor; };
        tools.mouseDown(mouseDownAt(tools, { 123, 30 }));
        expectWithinAbsoluteError(zoomFactor, 2.0, 0.0001); expectWithinAbsoluteError(zoomAnchor, 123.0f, 0.0001f);
        tools.mouseDown(mouseDownAt(tools, { 123, 30 }, juce::ModifierKeys::leftButtonModifier | secondaryModifier));
        expectWithinAbsoluteError(zoomFactor, 0.5, 0.0001);
        tools.mouseDown(mouseDownAt(tools, { 123, 30 }, juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier));
        expectWithinAbsoluteError(zoomFactor, 0.5, 0.0001); expectEquals(zoomCalls, 3);
        auto wheelCalls = 0;
        tools.onWheelGesture = [&](const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
        { ++wheelCalls; expect(event.mods.isAltDown()); expectWithinAbsoluteError(wheel.deltaY, 0.5f, 0.0001f); };
        juce::MouseWheelDetails wheel; wheel.deltaY = 0.5f;
        tools.mouseWheelMove(mouseDownAt(tools, { 123, 30 }, juce::ModifierKeys::altModifier), wheel);
        expectEquals(wheelCalls, 1); expectEquals(zoomCalls, 3);

        beginTest("Wide pitch slides remain visible when their note body is offscreen");
        tools.setNotes({});
        juce::Image emptyContour(juce::Image::ARGB, 600, 300, true);
        { juce::Graphics clipped(emptyContour); clipped.reduceClipRegion({ 61, 151, 28, 8 }); tools.paint(clipped); }
        NoteSegment sliding = note; sliding.midiNote = 40; sliding.pitch = 40.0; sliding.pitchOffsets = { 20.0f, 20.0f };
        tools.setNotes({ sliding });
        juce::Image visibleContour(juce::Image::ARGB, 600, 300, true);
        { juce::Graphics clipped(visibleContour); clipped.reduceClipRegion({ 61, 151, 28, 8 }); tools.paint(clipped); }
        expect(emptyContour.getPixelAt(75, 155) != visibleContour.getPixelAt(75, 155));

        beginTest("Read-only overlays allow selection and zoom while suppressing edits and audition");
        tools.setNotes({ exact }); tools.setSelectedNotes({}); tools.setReadOnly(true); tools.setTool(PitchTool::pointer);
        auto readOnlySelections = 0; tools.onNotesSelected = [&](const auto&) { ++readOnlySelections; };
        const auto movesBeforeReadOnly = moveCount, expressionsBeforeReadOnly = expressionCommits;
        const auto creationsBeforeReadOnly = creations, immediateBeforeReadOnly = immediateCount;
        const auto startsBeforeReadOnly = previewStarts;
        const auto readOnlyBounds = tools.noteBounds(7);
        tools.mouseDown(mouseDownAt(tools, toolCentre));
        tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(20, -10))); tools.mouseUp(mouseDownAt(tools, toolCentre));
        expectEquals(readOnlySelections, 1); expectEquals(tools.selectedNote(), 7);
        for (const auto tool : { PitchTool::pencil, PitchTool::eraser, PitchTool::scissors, PitchTool::join,
                                PitchTool::flex, PitchTool::vibrato, PitchTool::gain })
        {
            tools.setTool(tool); tools.mouseDown(mouseDownAt(tools, toolCentre));
            tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(20, -20))); tools.mouseUp(mouseDownAt(tools, toolCentre));
        }
        expect(tools.noteBounds(7) == readOnlyBounds);
        expectEquals(moveCount, movesBeforeReadOnly); expectEquals(expressionCommits, expressionsBeforeReadOnly);
        expectEquals(creations, creationsBeforeReadOnly); expectEquals(immediateCount, immediateBeforeReadOnly);
        expectEquals(previewStarts, startsBeforeReadOnly);
        const auto zoomsBeforeReadOnly = zoomCalls;
        tools.setTool(PitchTool::zoom); tools.mouseDown(mouseDownAt(tools, { 123, 30 }));
        expectEquals(zoomCalls, zoomsBeforeReadOnly + 1);
        tools.setReadOnly(false); tools.setTool(PitchTool::pointer);
        tools.mouseDown(mouseDownAt(tools, toolCentre));
        tools.mouseDrag(mouseDownAt(tools, toolCentre + juce::Point<float>(0, -10)));
        tools.setReadOnly(true); tools.mouseUp(mouseDownAt(tools, toolCentre));
        expect(tools.noteBounds(7) == readOnlyBounds); expectEquals(moveCount, movesBeforeReadOnly);
        tools.setReadOnly(false);

        beginTest("Audition ends on external data, hide and destructive callbacks");
        tools.setNotes({ exact }); tools.setTool(PitchTool::pointer); tools.setSecondaryTool(PitchTool::scissors);
        const auto stopsBeforeExternal = previewStops;
        tools.mouseDown(mouseDownAt(tools, toolCentre)); tools.setNotes({ exact });
        expectEquals(previewStops, stopsBeforeExternal + 1);
        tools.mouseDown(mouseDownAt(tools, toolCentre)); tools.setVisible(false); tools.mouseUp(mouseDownAt(tools, toolCentre));
        expectEquals(previewStops, stopsBeforeExternal + 2); tools.setVisible(true);
        auto deletingPreview = std::make_unique<PitchEditor>(viewport);
        deletingPreview->setSize(600, 300); deletingPreview->setNotes({ exact }); testRoot.addAndMakeVisible(*deletingPreview);
        auto deletedStops = 0;
        deletingPreview->onPitchPreviewEnd = [&] { ++deletedStops; };
        deletingPreview->onPitchPreview = [&](double, bool) { deletingPreview.reset(); };
        deletingPreview->mouseDown(mouseDownAt(*deletingPreview, toolCentre));
        expect(deletingPreview == nullptr); expectEquals(deletedStops, 1);

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
