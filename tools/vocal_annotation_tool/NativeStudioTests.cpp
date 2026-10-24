#include "NativeStudioView.h"
#include "editor_ui/Chrome.h"
#include "editor_ui/PitchEditor.h"
#include "uikit/Controls.h"
#include <iostream>

namespace
{
juce::Component* find(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren()) if (auto* result = find(*child, id)) return result;
    return nullptr;
}
juce::var fixture()
{
    return juce::JSON::parse(R"({"tempo":120,"meter":"4/4","timelineDurationSeconds":12,"initialPlayhead":25,"initialVolume":76,"initialInstrumentalVolume":70,"initialCycleRange":{"start":0,"end":100},"tracks":[{"id":"Instrumental","name":"Same name","clips":[{"id":"a","name":"trimmed","start":0,"offset":1,"duration":4,"waveformDurationSeconds":8,"waveform":[[-0.1,0.1],[-0.2,0.2],[-0.3,0.3],[-0.4,0.4]]}]},{"id":"Voice Main","name":"Same name","clips":[{"id":"b","name":"one","start":1,"offset":0,"duration":3},{"id":"c","name":"two","start":6,"offset":0,"duration":2}]},{"id":"backing-id","name":"Same name","parentId":"Voice Main","clips":[]}],"clips":[{"id":"vocal-note","x":10,"width":10,"pitch":60.25,"pitchCurve":[{"x":10,"pitch":60.1},{"x":11,"pitch":60.3},{"x":20,"pitch":60.2}]}],"backingTrackContents":[{"id":"backing-id","clips":[{"id":"backing-id:precise-note","x":25,"width":10,"pitch":64.35}]}],"tempoSegments":[{"start":0,"end":4,"bpm":120},{"start":4,"end":12,"bpm":60}],"timeSignatures":[{"start":0,"end":4,"numerator":4,"denominator":4},{"start":4,"end":12,"numerator":6,"denominator":8}]})");
}
juce::var projectEvent(const juce::var& project)
{
    auto* event = new juce::DynamicObject(); event->setProperty("type", "project-state"); event->setProperty("project", project); return event;
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
        beginTest("Compact layouts retain usable pitch editor");
        view.setSize(1000, 700); if (pitch) { expect(pitch->getWidth() >= 800); expect(pitch->getHeight() >= 150); }
        beginTest("Snapshots interrupt local note gestures and keep source IDs");
        view.dispatch(projectEvent(project)); expectEquals(view.state().noteCount, 1);
        view.removeFromDesktop();
    }
};
StudioChecks studioChecks;

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
        });
        view_->setBounds(-2000, -2000, 1280, 800); view_->addToDesktop(0); view_->setVisible(true); view_->dispatch(projectEvent(fixture()));
        startTimer(10);
    }
private:
    void check(bool pass, const char* message) { if (!pass) { ++failures_; std::cerr << message << '\n'; } }
    const juce::var* last(const char* type)
    { for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) if ((*it)["type"].toString() == type) return &*it; return nullptr; }
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
                    auto* remove = find(*view_, "studio.delete");
                    check(remove && remove->isEnabled(), "clip selection must enable toolbar Delete");
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
            default:
                stopTimer(); view_.reset(); juce::MessageManager::getInstance()->stopDispatchLoop(); break;
        }
    }
    int& failures_; int phase_ = 0;
    std::vector<juce::var> commands_;
    std::unique_ptr<vocal_annotation::NativeStudioView> view_;
};
}
int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    juce::UnitTestRunner runner; runner.setAssertOnFailure(false); runner.runTestsInCategory("NativeStudio", 123456);
    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) failures += runner.getResult(i)->failures;
    QueuedChecks queued(failures);
    juce::MessageManager::getInstance()->runDispatchLoop();
    std::cout << "Native studio failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
