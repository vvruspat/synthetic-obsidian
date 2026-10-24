#include "NativeStudioView.h"
#include "StudioShortcuts.h"
#include "editor_ui/Chrome.h"
#include "editor_ui/PitchEditor.h"
#include "uikit/Controls.h"
#include <iostream>

namespace
{
juce::File snapshotDirectory;
void snapshot(const juce::String& name, juce::Component& component)
{
    if (snapshotDirectory == juce::File()) return;
    snapshotDirectory.createDirectory();
    juce::MemoryOutputStream output;
    juce::PNGImageFormat png;
    if (png.writeImageToStream(component.createComponentSnapshot(component.getLocalBounds()), output))
        snapshotDirectory.getChildFile(name + ".png").replaceWithData(output.getData(), output.getDataSize());
}
juce::Component* find(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren()) if (auto* result = find(*child, id)) return result;
    return nullptr;
}
bool toolbarFits(juce::Component& view)
{
    auto* toolbar = find(view, "studio.pitchToolbar");
    if (!toolbar) return false;
    for (auto* child : toolbar->getChildren())
    {
        if (!child->isVisible()) continue;
        const auto interactive = dynamic_cast<obsidian::ui::Button*>(child) || dynamic_cast<obsidian::ui::Select*>(child) || dynamic_cast<obsidian::ui::Slider*>(child);
        if (!interactive) continue;
        if (!toolbar->getLocalBounds().contains(child->getBounds()) || child->getWidth() < 28 || child->getHeight() < 20) return false;
        if (auto* select = dynamic_cast<obsidian::ui::Select*>(child); select && child->getWidth() < (select->getIconOnly() ? 44 : 60)) return false;
        for (auto* other : toolbar->getChildren())
            if (other != child && other->isVisible() && child->getBounds().intersects(other->getBounds())) return false;
    }
    if (toolbar->getHeight() > 46) return false;
    auto* vertical = find(view, "studio.pitchZoom");
    auto* primary = find(view, "studio.tool.primary");
    if (!vertical || !primary || vertical->getRight() > primary->getX()) return false;
    for (const auto& id : { "studio.addBacking", "studio.settings", "studio.voiceLab", "studio.delete", "studio.status.dismiss" })
        if (auto* control = find(view, id); control && control->isShowing()) return false;
    if (!dynamic_cast<juce::ScrollBar*>(find(view, "studio.horizontalScroll"))) return false;
    return true;
}
juce::var fixture()
{
    return juce::JSON::parse(R"({"chords":[{"label":"C","width":100,"startSeconds":0,"endSeconds":12,"pitchClasses":[0,4,7]}],"backingTrackOptions":["Unison Double","Third Above"],"voiceProfiles":[{"id":"source-timbre","name":"Source timbre"}],"initialBackingTracks":[{"id":"backing-id","name":"Same name"}],"tempo":120,"meter":"4/4","timelineDurationSeconds":12,"initialPlayhead":25,"initialVolume":76,"initialInstrumentalVolume":70,"initialCycleRange":{"start":0,"end":100},"tracks":[{"id":"Instrumental","name":"Same name","clips":[{"id":"a","name":"trimmed","start":0,"offset":1,"duration":4,"waveformDurationSeconds":8,"waveform":[[-0.1,0.1],[-0.2,0.2],[-0.3,0.3],[-0.4,0.4]]}]},{"id":"Voice Main","name":"Same name","clips":[{"id":"b","name":"one","start":1,"offset":0,"duration":3},{"id":"c","name":"two","start":6,"offset":0,"duration":2}]},{"id":"backing-id","name":"Same name","parentId":"Voice Main","clips":[]}],"clips":[{"id":"vocal-note","x":10,"width":10,"pitch":60.25,"pitchCurve":[{"x":10,"pitch":60.1},{"x":11,"pitch":60.3},{"x":20,"pitch":60.2}]}],"backingTrackContents":[{"id":"backing-id","voiceProfileId":"source-timbre","hasAudio":false,"clips":[{"id":"backing-id:precise-note","x":25,"width":10,"pitch":64.35,"pitchCurve":[{"x":25,"pitch":64.1},{"x":26,"pitch":64.6},{"x":27,"pitch":64.15},{"x":28,"pitch":64.7},{"x":29,"pitch":64.2},{"x":30,"pitch":64.65},{"x":31,"pitch":64.1},{"x":32,"pitch":64.6},{"x":33,"pitch":64.25},{"x":34,"pitch":64.55},{"x":35,"pitch":64.35}]}]}],"tempoSegments":[{"start":0,"end":4,"bpm":120},{"start":4,"end":12,"bpm":60}],"timeSignatures":[{"start":0,"end":4,"numerator":4,"denominator":4},{"start":4,"end":12,"numerator":6,"denominator":8}]})");
}
juce::var projectEvent(const juce::var& project)
{
    auto* event = new juce::DynamicObject(); event->setProperty("type", "project-state"); event->setProperty("project", project); return event;
}
juce::MouseEvent mouseEvent(juce::Component& target, juce::Point<float> point, int modifiers = juce::ModifierKeys::leftButtonModifier)
{
    const auto time = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), point, juce::ModifierKeys(modifiers),
             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, time, point, time, 1, false };
}
class StudioChecks final : public juce::UnitTest
{
public:
    StudioChecks() : UnitTest("Native studio binding", "NativeStudio") {}
    void runTest() override
    {
        using namespace vocal_annotation;
        std::vector<juce::var> commands;
        NativeStudioView view([&](const auto& value) { commands.push_back(value); });
        view.setBounds(-2000, -2000, 1280, 800); view.addToDesktop(0); view.setVisible(true);
        beginTest("Project snapshots use real IDs, multiple clips and silent model updates");
        const auto project = fixture(); view.dispatch(projectEvent(project));
        expectEquals(view.state().trackCount, 3); expectEquals(view.state().clipCount, 3);
        expectEquals(view.state().selectedTrack, juce::String("Voice Main")); expectEquals(view.state().noteCount, 1);
        expect(commands.empty());
        view.dispatch(projectEvent(project)); expect(commands.empty());
        auto* pitch = dynamic_cast<obsidian::editor::PitchEditor*>(find(view, "studio.pitch"));
        auto* ruler = dynamic_cast<obsidian::editor::TimelineRuler*>(find(view, "studio.ruler"));
        auto* lower = dynamic_cast<obsidian::editor::TimelineRuler*>(find(view, "studio.pitchRuler"));
        expect(pitch && ruler && lower);
        beginTest("Real transport state and geometry remain aligned across tempo changes");
        auto* transport = new juce::DynamicObject(); transport->setProperty("type", "transport-state"); transport->setProperty("playhead", 50); transport->setProperty("playing", true);
        view.dispatch(transport); expectWithinAbsoluteError(view.state().playheadSeconds, 6.0, 0.0001);
        if (ruler && lower)
        {
            ruler->onViewportGesture(0, 1.5, 300);
            const auto image = view.createComponentSnapshot(view.getLocalBounds()); expect(image.isValid());
        }
        snapshot("editor", view);
        if (auto* mute = dynamic_cast<obsidian::ui::Button*>(find(view, "studio.mute.Instrumental"))) mute->setSelected(true);
        if (auto* solo = dynamic_cast<obsidian::ui::Button*>(find(view, "studio.solo.Voice Main"))) solo->setSelected(true);
        snapshot("track-states", view);
        view.dispatch(projectEvent(project));
        beginTest("Compact layouts retain usable pitch editor");
        view.setSize(1000, 700); snapshot("compact", view); if (pitch) { expect(pitch->getWidth() >= 800); expect(pitch->getHeight() >= 150); }
        beginTest("Snapshots interrupt local note gestures and keep source IDs");
        view.dispatch(projectEvent(project)); expectEquals(view.state().noteCount, 1);
        beginTest("Independent primary and Command-click tool menus expose all legacy tools");
        auto* primary = dynamic_cast<obsidian::ui::Select*>(find(view, "studio.tool.primary"));
        auto* secondary = dynamic_cast<obsidian::ui::Select*>(find(view, "studio.tool.secondary"));
        expect(primary && secondary);
        if (primary && secondary && pitch)
        {
            expect(primary->getIconOnly() && secondary->getIconOnly());
            expectEquals(primary->getItems().size(), 9); expectEquals(secondary->getItems().size(), 9);
            expect(pitch->getTool() == obsidian::editor::PitchTool::pointer);
            expect(pitch->getSecondaryTool() == obsidian::editor::PitchTool::scissors);
            primary->setSelectedIndex(7, juce::sendNotificationSync);
            expect(pitch->getTool() == obsidian::editor::PitchTool::gain);
            expect(pitch->getSecondaryTool() == obsidian::editor::PitchTool::scissors);
            secondary->setSelectedIndex(4, juce::sendNotificationSync);
            expect(pitch->getSecondaryTool() == obsidian::editor::PitchTool::join);
            primary->showMenu(); snapshot("tool-menu", view);
            if (auto* menu = juce::Component::getCurrentlyModalComponent()) menu->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        }
        beginTest("Vertical zoom and collapsed panel preserve pitch data, selection and scroll");
        auto* vertical = dynamic_cast<obsidian::ui::Slider*>(find(view, "studio.pitchZoom"));
        auto* collapse = dynamic_cast<obsidian::ui::Button*>(find(view, "studio.pitchCollapse"));
        auto* arrangement = find(view, "studio.arrangementViewport");
        expect(vertical && collapse && arrangement);
        if (vertical && collapse && arrangement && pitch)
        {
            pitch->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
            vertical->setValue(2.5, juce::sendNotificationSync);
            expectWithinAbsoluteError(view.state().verticalZoom, 2.5, 0.001);
            const auto expanded = view.state(); const auto arrangementHeight = arrangement->getHeight();
            collapse->triggerClick();
            expect(view.state().pianoCollapsed); expect(!pitch->isShowing());
            expect(arrangement->getHeight() > arrangementHeight);
            snapshot("collapsed", view);
            collapse->triggerClick(); expect(!view.state().pianoCollapsed); expect(pitch->isShowing());
            expectWithinAbsoluteError(view.state().verticalZoom, expanded.verticalZoom, 0.001);
            expectEquals(view.state().pianoScrollY, expanded.pianoScrollY);
            expectEquals(view.state().selectedNoteId, expanded.selectedNoteId);
            expectEquals(view.state().noteCount, expanded.noteCount);
            snapshot("vertical-zoom", view);
            view.dispatch(projectEvent(project));
            expectWithinAbsoluteError(view.state().verticalZoom, expanded.verticalZoom, 0.001);
            expectEquals(view.state().pianoScrollY, expanded.pianoScrollY);
        }
        beginTest("All toolbar controls retain usable bounds at compact, laptop and large widths");
        for (const auto width : {1000, 1280, 1586})
        { view.setSize(width, 800); expect(toolbarFits(view), "Toolbar control clipped at width " + juce::String(width)); }
        beginTest("Time scrollbar thumb matches the viewport and pans notes within the project");
        auto* timeScroll = dynamic_cast<juce::ScrollBar*>(find(view, "studio.horizontalScroll"));
        auto* timeZoom = dynamic_cast<obsidian::ui::Slider*>(find(view, "studio.zoom"));
        expect(timeScroll && timeZoom);
        if (timeScroll && timeZoom && pitch && ruler)
        {
            timeZoom->setValue(160.0, juce::sendNotificationSync);
            timeScroll->setCurrentRangeStart(0.0, juce::sendNotificationSync);
            pitch->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
            const auto note = pitch->selectedNote();
            const auto before = pitch->noteBounds(note).getX();
            expectWithinAbsoluteError(timeScroll->getCurrentRangeSize(), static_cast<double>(ruler->getWidth()) / 160.0, 0.001);
            timeScroll->setCurrentRangeStart(10000.0, juce::sendNotificationSync);
            const auto start = timeScroll->getCurrentRangeStart();
            expect(start > 0.0);
            expectWithinAbsoluteError(start + timeScroll->getCurrentRangeSize(), timeScroll->getMaximumRangeLimit(), 0.001);
            expectWithinAbsoluteError(pitch->noteBounds(note).getX(), before - static_cast<float>(start * 160.0), 0.01f);
            snapshot("scrolled", view);
        }
        view.removeFromDesktop();
    }
};
StudioChecks studioChecks;

class StudioPresentationChecks final : public juce::UnitTest
{
public:
    StudioPresentationChecks() : UnitTest("Studio audio backdrop and gestures", "NativeStudio") {}
    void runTest() override
    {
        using namespace obsidian;
        vocal_annotation::NativeStudioView view([](const juce::var&) {});
        view.setBounds(-2000, -2000, 1280, 900); view.addToDesktop(0); view.setVisible(true);
        auto project = fixture();
        auto& vocalClip = (*project["tracks"].getArray())[1]["clips"].getArray()->getReference(0);
        vocalClip.getDynamicObject()->setProperty("offset", 1.0);
        vocalClip.getDynamicObject()->setProperty("duration", 5.0);
        vocalClip.getDynamicObject()->setProperty("waveformDurationSeconds", 8.0);
        juce::Array<juce::var> peaks;
        for (int i = 0; i < 512; ++i)
        {
            const auto amplitude = 0.05 + 0.8 * std::pow(std::sin(static_cast<double>(i) * 0.07), 2.0);
            peaks.add(juce::var(juce::Array<juce::var>{ -amplitude, amplitude }));
        }
        vocalClip.getDynamicObject()->setProperty("waveform", juce::var(peaks));
        auto& secondClip = (*project["tracks"].getArray())[1]["clips"].getArray()->getReference(1);
        secondClip.getDynamicObject()->setProperty("start", 8.0);
        secondClip.getDynamicObject()->setProperty("waveformDurationSeconds", 2.0);
        secondClip.getDynamicObject()->setProperty("waveform", juce::var(peaks));
        view.dispatch(projectEvent(project));
        auto* pitch = dynamic_cast<editor::PitchEditor*>(find(view, "studio.pitch"));
        auto* ruler = find(view, "studio.ruler");
        auto* pianoViewport = dynamic_cast<juce::Viewport*>(find(view, "studio.pitchViewport"));
        auto* zoom = dynamic_cast<ui::Slider*>(find(view, "studio.zoom"));
        auto* vertical = dynamic_cast<ui::Slider*>(find(view, "studio.pitchZoom"));
        auto* scroll = dynamic_cast<juce::ScrollBar*>(find(view, "studio.horizontalScroll"));
        auto* trackSelect = dynamic_cast<ui::Select*>(find(view, "studio.trackSelect"));
        beginTest("Native presentation controls are available");
        expect(pitch && ruler && pianoViewport && zoom && vertical && scroll && trackSelect);
        if (!pitch || !ruler || !pianoViewport || !zoom || !vertical || !scroll || !trackSelect) return;

        beginTest("Backing groups share exact header bounds");
        auto* row = find(view, "studio.track.Voice Main");
        auto* group = find(view, "studio.group.Voice Main");
        editor::TrackHeader* header = nullptr;
        if (row) for (auto* child : row->getChildren()) if (auto* candidate = dynamic_cast<editor::TrackHeader*>(child)) header = candidate;
        expect(header && group);
        if (header && group)
        {
            expectEquals(group->getX(), row->getX() + header->getX());
            expectEquals(group->getWidth(), header->getWidth());
        }

        beginTest("Selected audio backdrop follows trimmed placement across tempo changes");
        expectEquals(static_cast<int>(pitch->waveformRegionCount()), 2);
        if (!pitch->waveformRegions().empty())
        {
            const auto& region = pitch->waveformRegions().front();
            expectWithinAbsoluteError(region.startBeat, 2.0, 0.001);
            expectWithinAbsoluteError(region.lengthBeats, 8.0, 0.001);
            expect(!region.peaks.empty());
            if (!region.peaks.empty()) expectWithinAbsoluteError(static_cast<double>(region.peaks.front()),
                0.05 + 0.8 * std::pow(std::sin(64.0 * 0.07), 2.0), 0.0001);
        }
        if (pitch->waveformRegionCount() == 2)
        {
            const auto& second = pitch->waveformRegions()[1];
            expectWithinAbsoluteError(second.startBeat, 12.0, 0.001);
            expectWithinAbsoluteError(second.lengthBeats, 2.0, 0.001);
        }
        expect(!pitch->waveformBounds().isEmpty());
        const auto revision = pitch->waveformGeometryRevision();
        snapshot("waveform-background", view);
        view.createComponentSnapshot(view.getLocalBounds());
        expectEquals(pitch->waveformGeometryRevision(), revision);
        pianoViewport->setViewPosition(0, pianoViewport->getViewPositionY() + 90);
        expectWithinAbsoluteError(pitch->waveformBounds().getCentreY(),
            static_cast<float>(pianoViewport->getViewPositionY()) + static_cast<float>(pianoViewport->getHeight()) * 0.5f, 1.0f);
        trackSelect->setSelectedIndex(2, juce::sendNotificationSync);
        expectEquals(static_cast<int>(pitch->waveformRegionCount()), 0);
        expect(pitch->waveformBounds().isEmpty());
        trackSelect->setSelectedIndex(1, juce::sendNotificationSync);
        expectEquals(static_cast<int>(pitch->waveformRegionCount()), 2);
        if (auto* backing = find(view, "studio.track.backing-id"))
            backing->mouseDown(mouseEvent(*backing, {40, 20}, juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier));
        expectEquals(static_cast<int>(pitch->waveformRegionCount()), 0);
        if (trackSelect->onChange) trackSelect->onChange(1);

        beginTest("Clips beyond the source duration remain silent instead of repeating the final sample");
        vocalClip.getDynamicObject()->setProperty("offset", 10.0);
        secondClip.getDynamicObject()->setProperty("offset", 10.0);
        view.dispatch(projectEvent(project));
        for (const auto& region : pitch->waveformRegions())
            expect(std::all_of(region.peaks.begin(), region.peaks.end(), [](float peak) { return peak == 0.0f; }));
        expect(pitch->waveformBounds().isEmpty());
        vocalClip.getDynamicObject()->setProperty("offset", 1.0);
        secondClip.getDynamicObject()->setProperty("offset", 0.0);

        beginTest("Trackpad pinch keeps its time anchor and Option pinch keeps its pitch anchor");
        project.getDynamicObject()->setProperty("timelineDurationSeconds", 120.0);
        view.dispatch(projectEvent(project));
        zoom->setValue(80.0, juce::sendNotificationSync);
        vertical->setValue(2.0, juce::sendNotificationSync);
        scroll->setCurrentRangeStart(20.0, juce::sendNotificationSync);
        auto point = pitch->getLocalPoint(pianoViewport, juce::Point<float>(500, 100));
        const auto anchorX = ruler->getLocalPoint(pitch, point).x;
        const auto beatBefore = scroll->getCurrentRangeStart() + static_cast<double>(anchorX) / view.state().pixelsPerBeat;
        pitch->mouseMagnify(mouseEvent(*pitch, point, 0), 1.5f);
        expectWithinAbsoluteError(view.state().pixelsPerBeat, 120.0, 0.001);
        expectWithinAbsoluteError(scroll->getCurrentRangeStart() + static_cast<double>(anchorX) / view.state().pixelsPerBeat, beatBefore, 0.001);
        const auto pitchBefore = (pianoViewport->getViewPositionY() + 100.0) / view.state().verticalZoom;
        point = pitch->getLocalPoint(pianoViewport, juce::Point<float>(500, 100));
        pitch->mouseMagnify(mouseEvent(*pitch, point, juce::ModifierKeys::altModifier), 1.25f);
        expectWithinAbsoluteError(view.state().verticalZoom, 2.5, 0.001);
        expectWithinAbsoluteError((pianoViewport->getViewPositionY() + 100.0) / view.state().verticalZoom, pitchBefore, 1.0);
        const auto state = view.state();
        pitch->mouseMagnify(mouseEvent(*pitch, point), std::numeric_limits<float>::quiet_NaN());
        expectWithinAbsoluteError(view.state().pixelsPerBeat, state.pixelsPerBeat, 0.001);
        expectWithinAbsoluteError(view.state().verticalZoom, state.verticalZoom, 0.001);
        pianoViewport->mouseMagnify(mouseEvent(*pianoViewport, {30, 100}, juce::ModifierKeys::altModifier), 1.1f);
        expectWithinAbsoluteError(view.state().verticalZoom, 2.75, 0.001);

        beginTest("Diagonal trackpad noise does not suppress modifier zoom");
        juce::MouseWheelDetails wheel; wheel.deltaX = 0.001f; wheel.deltaY = 0.1f; wheel.isSmooth = true;
        vertical->setValue(2.0, juce::sendNotificationSync);
        pitch->mouseWheelMove(mouseEvent(*pitch, point, juce::ModifierKeys::altModifier), wheel);
        expect(view.state().verticalZoom > 2.1);
        view.removeFromDesktop();
    }
};
StudioPresentationChecks studioPresentationChecks;

// Exercise queued UI -> backend -> UI feedback through the real message loop.
class QueuedChecks final : private juce::Timer
{
public:
    explicit QueuedChecks(int& failures) : failures_(failures)
    {
        view_ = std::make_unique<vocal_annotation::NativeStudioView>([this](const juce::var& value)
        {
            commands_.push_back(value);
            if (value["type"].toString() == "set-track-state") view_->dispatch(projectEvent(fixture()));
            if (value["type"].toString() == "set-loop")
            {
                auto* event = new juce::DynamicObject(); event->setProperty("type", "loop-state");
                event->setProperty("active", value["active"]); event->setProperty("range", value["range"]); view_->dispatch(event);
            }
        }, shortcuts_);
        view_->setBounds(-2000, -2000, 1280, 800); view_->addToDesktop(0); view_->setVisible(true); view_->dispatch(projectEvent(fixture()));
        startTimer(10);
    }
private:
    void check(bool pass, const char* message) { if (!pass) { ++failures_; std::cerr << message << '\n'; } }
    const juce::var* last(const char* type)
    { for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) if ((*it)["type"].toString() == type) return &*it; return nullptr; }
    float cycleX(double percent) const
    {
        const auto seconds = percent * 12.0 / 100.0;
        const auto beat = seconds <= 4.0 ? seconds * 2.0 : 8.0 + seconds - 4.0;
        return static_cast<float>(beat * view_->state().pixelsPerBeat);
    }
    void timerCallback() override
    {
        using namespace obsidian;
        switch (phase_++)
        {
            case 0:
            {
                auto* transport = dynamic_cast<editor::TransportBar*>(find(*view_, "studio.transport"));
                check(transport != nullptr, "missing native transport");
                if (transport) { transport->onMasterVolume(0.65); transport->getPlayButton().triggerClick(); }
                auto* mute = dynamic_cast<ui::Button*>(find(*view_, "studio.mute.Voice Main"));
                check(mute != nullptr, "missing real track mute"); if (mute) mute->triggerClick();
                break;
            }
            case 1:
            {
                check(last("set-volume") && std::abs(static_cast<double>((*last("set-volume"))["value"]) - 65.0) < 0.001, "master gain conversion must use 0..100");
                check(last("transport") && (*last("transport"))["action"].toString() == "play", "native Play must request backend transport");
                check(last("set-track-state") && (*last("set-track-state"))["track"].toString() == "Voice Main", "track identity lost during mute");
                check(view_->state().trackCount == 3, "synchronous backend feedback destroyed studio state");
                auto* select = dynamic_cast<ui::Select*>(find(*view_, "studio.trackSelect"));
                if (select && select->onChange) select->onChange(2);
                break;
            }
            case 2:
            {
                check(view_->state().selectedTrack == "backing-id", "duplicate display names must select backing by ID");
                check(toolbarFits(*view_), "selected-backing profile/render controls must fit the toolbar");
                snapshot("backing-controls", *view_);
                auto* pitch = dynamic_cast<editor::PitchEditor*>(find(*view_, "studio.pitch"));
                if (pitch)
                {
                    pitch->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
                    const auto id = pitch->selectedNote();
                    if (pitch->onNotesMoved) pitch->onNotesMoved({ { id, 9.0, 1.0, 65 } });
                    else check(false, "missing batched note move");
                }
                break;
            }
            case 3:
            {
                const auto* moved = last("move-note-group");
                check(moved != nullptr, "group move did not reach backend");
                if (moved)
                {
                    const auto note = (*moved)["notes"][0];
                    check((*moved)["track"].toString() == "backing-id" && note["clipId"].toString() == "backing-id:precise-note", "native edit changed backing note identity");
                    check(std::abs(static_cast<double>(note["pitch"]) - 65.35) < 0.0001, "native move must preserve fractional MIDI");
                    check(std::abs(static_cast<double>(note["startDeltaSeconds"]) - 2.0) < 0.001, "native edit failed piecewise tempo conversion");
                }
                if (auto* clip = dynamic_cast<editor::AudioClip*>(find(*view_, "studio.clip.b")))
                {
                    clip->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
                    view_->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
                }
                auto disposable = std::make_unique<vocal_annotation::NativeStudioView>([](const juce::var&) {});
                disposable->setSize(1000, 700); disposable->dispatch(projectEvent(fixture()));
                if (auto* add = dynamic_cast<ui::Button*>(find(*disposable, "studio.addTrack"))) add->triggerClick();
                disposable.reset(); // Queued callbacks must not retain this view.
                break;
            }
            case 4:
            {
                const auto* remove = last("remove-arrangement-clip");
                check(remove && (*remove)["clipId"].toString() == "b", "Delete after clip selection must target the audio clip, not old pitch notes");
                auto* createdTrack = new juce::DynamicObject(); createdTrack->setProperty("id", "new-backing");
                auto* createdEvent = new juce::DynamicObject(); createdEvent->setProperty("type", "backing-track-created"); createdEvent->setProperty("track", createdTrack);
                view_->dispatch(createdEvent);
                auto updated = fixture();
                auto* track = new juce::DynamicObject(); track->setProperty("id", "new-backing"); track->setProperty("name", "Same name"); track->setProperty("parentId", "Voice Main");
                updated["tracks"].getArray()->add(juce::var(track));
                view_->dispatch(projectEvent(updated));
                break;
            }
            case 5:
            {
                const auto* select = last("select-tracks");
                check(view_->state().selectedTrack == "new-backing" && select && (*select)["tracks"][0].toString() == "new-backing",
                      "automatic backing selection must synchronize backend target once");
                break;
            }
            case 6:
            {
                auto* tracks = dynamic_cast<ui::Select*>(find(*view_, "studio.trackSelect"));
                if (tracks) tracks->setSelectedIndex(1, juce::sendNotificationSync);
                auto* primary = dynamic_cast<ui::Select*>(find(*view_, "studio.tool.primary"));
                if (primary) primary->setSelectedIndex(2, juce::sendNotificationSync); // Eraser
                auto* pitch = dynamic_cast<editor::PitchEditor*>(find(*view_, "studio.pitch"));
                if (pitch)
                {
                    pitch->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
                    const auto point = pitch->noteBounds(pitch->selectedNote()).getCentre();
                   #if JUCE_MAC
                    const auto modifier = juce::ModifierKeys::commandModifier;
                   #else
                    const auto modifier = juce::ModifierKeys::ctrlModifier;
                   #endif
                    const auto event = mouseEvent(*pitch, point, juce::ModifierKeys::leftButtonModifier | modifier);
                    pitch->mouseDown(event); pitch->mouseUp(event);
                }
                break;
            }
            case 7:
            {
                check(last("split-clip") && (*last("split-clip"))["clipId"].toString() == "vocal-note", "Command-click must use secondary Scissors, not primary Eraser");
                auto* pitch = dynamic_cast<editor::PitchEditor*>(find(*view_, "studio.pitch"));
                if (pitch) { const auto event = mouseEvent(*pitch, pitch->noteBounds(pitch->selectedNote()).getCentre()); pitch->mouseDown(event); pitch->mouseUp(event); }
                break;
            }
            case 8:
            {
                check(last("delete-clips") && (*last("delete-clips"))["clipIds"][0].toString() == "vocal-note", "ordinary click must use independently selected Eraser");
                auto* primary = dynamic_cast<ui::Select*>(find(*view_, "studio.tool.primary"));
                if (primary) primary->setSelectedIndex(7, juce::sendNotificationSync); // Gain
                auto* pitch = dynamic_cast<editor::PitchEditor*>(find(*view_, "studio.pitch"));
                if (pitch)
                {
                    pitch->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
                    const auto point = pitch->noteBounds(pitch->selectedNote()).getCentre();
                    pitch->mouseDown(mouseEvent(*pitch, point)); pitch->mouseDrag(mouseEvent(*pitch, point + juce::Point<float>(0, -16))); pitch->mouseUp(mouseEvent(*pitch, point));
                }
                break;
            }
            case 9:
                check(last("set-clip-gain") && std::abs(static_cast<double>((*last("set-clip-gain"))["gainDb"]) - 2.0) < 0.001,
                      "Gain tool drag must commit through the existing backend");
                break;
            case 10:
            {
                auto* backingRow = find(*view_, "studio.track.backing-id");
                if (backingRow) backingRow->mouseDown(mouseEvent(*backingRow, {40, 20}, juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier));
                check(view_->state().selectedTrackCount == 2 && view_->state().pitchReadOnly && view_->state().noteCount == 2,
                      "Shift-click tracks must combine notes in a read-only pitch overlay");
                break;
            }
            case 11:
            {
                auto* tracks = dynamic_cast<ui::Select*>(find(*view_, "studio.trackSelect"));
                if (tracks && tracks->onChange) tracks->onChange(1);
                if (auto* add = dynamic_cast<ui::Button*>(find(*view_, "studio.addBacking.Voice Main"))) add->triggerClick();
                auto* generator = dynamic_cast<ui::Button*>(find(*view_, "studio.backing.generator.terra"));
                auto* style = dynamic_cast<ui::Button*>(find(*view_, "studio.backing.type.1"));
                check(generator && style, "Add backing must open direct generator and harmony style choices");
                snapshot("backing-picker", *view_);
                if (generator) generator->triggerClick(); if (style) style->triggerClick();
                break;
            }
            case 12:
            {
                const auto* add = last("add-backing-track");
                check(add && (*add)["track"].toString() == "Third Above" && (*add)["generator"].toString() == "terra" && (*add)["parentId"].toString() == "Voice Main",
                      "backing type and generator must create on the chosen source track");
                auto* loop = new juce::DynamicObject(); loop->setProperty("type", "loop-state"); loop->setProperty("active", false);
                auto* range = new juce::DynamicObject(); range->setProperty("start", 0); range->setProperty("end", 10); loop->setProperty("range", range); view_->dispatch(loop);
                if (auto* cycle = find(*view_, "studio.cycle"))
                {
                    auto event = mouseEvent(*cycle, {150, 12});
                    cycle->mouseDown(event); cycle->mouseDrag(event.withNewPosition(juce::Point<float>(350, 12))); cycle->mouseUp(event.withNewPosition(juce::Point<float>(350, 12)));
                }
                else check(false, "direct cycle lane is missing");
                break;
            }
            case 13:
            {
                const auto* loop = last("set-loop");
                check(loop && static_cast<bool>((*loop)["active"]) && static_cast<double>((*loop)["range"]["end"]) > static_cast<double>((*loop)["range"]["start"]),
                      "dragging the cycle lane must create a real active loop");
                check(view_->state().loopActive, "created cycle must appear active immediately");
                snapshot("cycle", *view_);
                if (auto* cycle = find(*view_, "studio.cycle"))
                { auto event = mouseEvent(*cycle, {250, 12}); cycle->mouseDown(event); cycle->mouseUp(event); }
                break;
            }
            case 14:
            {
                const auto* loop = last("set-loop");
                check(loop && !static_cast<bool>((*loop)["active"]), "clicking the cycle body must toggle the loop off");
                cycleStart_ = view_->state().loopStart; cycleEnd_ = view_->state().loopEnd;
                if (auto* cycle = find(*view_, "studio.cycle"))
                {
                    const auto centre = (cycleX(cycleStart_) + cycleX(cycleEnd_)) * 0.5f;
                    auto event = mouseEvent(*cycle, {centre, 12});
                    cycle->mouseDown(event); cycle->mouseDrag(event.withNewPosition(juce::Point<float>(centre + 80, 12))); cycle->mouseUp(event);
                }
                break;
            }
            case 15:
            {
                check(view_->state().loopStart > cycleStart_ && std::abs((view_->state().loopEnd - view_->state().loopStart) - (cycleEnd_ - cycleStart_)) < 0.001,
                      "dragging cycle body must move range while preserving length");
                cycleStart_ = view_->state().loopStart; cycleEnd_ = view_->state().loopEnd;
                if (auto* cycle = find(*view_, "studio.cycle"))
                {
                    const auto x = cycleX(cycleStart_); auto event = mouseEvent(*cycle, {x, 12});
                    cycle->mouseDown(event); cycle->mouseDrag(event.withNewPosition(juce::Point<float>(x - 80, 12))); cycle->mouseUp(event);
                }
                break;
            }
            case 16:
            {
                check(view_->state().loopStart < cycleStart_ && std::abs(view_->state().loopEnd - cycleEnd_) < 0.001,
                      "cycle left edge must resize with the right edge fixed");
                cycleStart_ = view_->state().loopStart; cycleEnd_ = view_->state().loopEnd;
                if (auto* cycle = find(*view_, "studio.cycle"))
                {
                    const auto x = cycleX(cycleEnd_); auto event = mouseEvent(*cycle, {x, 12});
                    cycle->mouseDown(event); cycle->mouseDrag(event.withNewPosition(juce::Point<float>(x + 80, 12))); cycle->mouseUp(event);
                }
                break;
            }
            case 17:
            {
                check(view_->state().loopEnd > cycleEnd_ && std::abs(view_->state().loopStart - cycleStart_) < 0.001,
                      "cycle right edge must resize with the left edge fixed");
                const auto before = commands_.size();
                if (auto* cycle = find(*view_, "studio.cycle"))
                {
                    const auto x = cycleX(view_->state().loopEnd) + 70; auto event = mouseEvent(*cycle, {x, 12});
                    cycle->mouseDown(event); cycle->mouseDrag(event.withNewPosition(juce::Point<float>(x + 60, 12))); cycle->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)); cycle->mouseUp(event);
                }
                check(commands_.size() == before, "Escape must cancel a cycle draft without sending a backend edit");
                break;
            }
            case 18:
            {
                shortcuts_->assign("project.save", juce::KeyPress(juce::KeyPress::upKey));
                auto* pitch = dynamic_cast<editor::PitchEditor*>(find(*view_, "studio.pitch"));
                if (pitch)
                {
                    beforeShortcutNote_ = view_->state().selectedNoteId;
                    pitch->grabKeyboardFocus();
                    if (auto* peer = view_->getPeer()) peer->handleKeyPress(juce::KeyPress(juce::KeyPress::upKey));
                }
                break;
            }
            case 19:
            {
                const auto* action = last("project-action");
                check(action && (*action)["action"].toString() == "save-project", "a configured Save->Up shortcut must take precedence over focused note selection");
                check(view_->state().selectedNoteId == beforeShortcutNote_, "captured custom shortcut must not also change note selection");
                break;
            }
            case 20:
            {
                auto* primary = dynamic_cast<ui::Select*>(find(*view_, "studio.tool.primary"));
                if (primary) primary->setSelectedIndex(0, juce::sendNotificationSync);
                if (auto* snap = dynamic_cast<ui::Button*>(find(*view_, "studio.chordSnap"))) snap->triggerClick();
                auto* pitch = dynamic_cast<editor::PitchEditor*>(find(*view_, "studio.pitch"));
                if (pitch)
                {
                    pitch->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
                    const auto point = pitch->noteBounds(pitch->selectedNote()).getCentre();
                    auto event = mouseEvent(*pitch, point); pitch->mouseDown(event);
                    pitch->mouseDrag(event.withNewPosition(point + juce::Point<float>(20, -18)));
                    pitch->mouseUp(event);
                }
                break;
            }
            case 21:
            {
                const auto* moved = last("move-note-group"); const auto* preview = last("preview-pitch-tone");
                check(moved && preview && std::abs(static_cast<double>((*moved)["notes"][0]["pitch"]) - 60.0) < 0.001
                      && std::abs(static_cast<double>((*preview)["pitch"]) - 60.0) < 0.001,
                      "Chord Snap must make audition and committed pitch agree");
                if (moved) check(std::abs(static_cast<double>((*moved)["notes"][0]["startDeltaSeconds"])) < 0.001
                                 && std::abs(static_cast<double>((*moved)["notes"][0]["endDeltaSeconds"])) < 0.001,
                                 "Pointer pitch editing must preserve source note timing");
                break;
            }
            default:
                stopTimer(); view_.reset(); juce::MessageManager::getInstance()->stopDispatchLoop(); break;
        }
    }
    int& failures_; int phase_ = 0;
    std::vector<juce::var> commands_;
    std::shared_ptr<vocal_annotation::StudioShortcuts> shortcuts_ = std::make_shared<vocal_annotation::StudioShortcuts>();
    juce::String beforeShortcutNote_;
    double cycleStart_ = 0, cycleEnd_ = 0;
    std::unique_ptr<vocal_annotation::NativeStudioView> view_;
};
}
int main(int argc, char** argv)
{
    if (argc == 3 && juce::String(argv[1]) == "--snapshot-dir")
        snapshotDirectory = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(argv[2]));
    juce::ScopedJuceInitialiser_GUI initialise;
    juce::UnitTestRunner runner; runner.setAssertOnFailure(false); runner.runTestsInCategory("NativeStudio", 123456);
    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) failures += runner.getResult(i)->failures;
    QueuedChecks queued(failures);
    juce::MessageManager::getInstance()->runDispatchLoop();
    std::cout << "Native studio failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
