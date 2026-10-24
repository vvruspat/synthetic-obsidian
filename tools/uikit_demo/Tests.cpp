#include "Demo.h"
#include "editor_ui/Chrome.h"
#include "editor_ui/PitchEditor.h"
#include "uikit/Controls.h"
#include <iostream>

namespace obsidian::ui { void runAsyncDeletionChecks(std::function<void(bool)>); }
namespace
{
juce::Component* find(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren()) if (auto* result = find(*child, id)) return result;
    return nullptr;
}
class IntegrationTests final : public juce::UnitTest
{
public:
    IntegrationTests() : UnitTest("Demo integration", "Obsidian") {}
    void runTest() override
    {
        using namespace obsidian;
        demo::Demo demo;
        demo.setSize(1586, 992);
        demo.addToDesktop(0); demo.setVisible(true);
        beginTest("Demo starts with five tracks and cached pitch data");
        expectEquals(demo.state().tracks, 5); expectEquals(demo.state().notes, 40);
        expect(!demo.state().playing);
        auto* play = dynamic_cast<ui::Button*>(find(demo, "transport.play"));
        auto* stop = dynamic_cast<ui::Button*>(find(demo, "transport.stop"));
        expect(play != nullptr && stop != nullptr);
        if (play && stop)
        {
            beginTest("Transport uses controlled model callbacks");
            play->triggerClick(); expect(demo.state().playing); stop->triggerClick(); expect(!demo.state().playing);
        }
        auto* group = dynamic_cast<ui::Button*>(find(demo, "group.toggle"));
        if (group)
        {
            beginTest("Backing group toggles and restores rows");
            group->triggerClick(); expect(!demo.state().backingExpanded);
            group->triggerClick(); expect(demo.state().backingExpanded);
        }
        else expect(false, "Group toggle missing");
        auto* add = dynamic_cast<ui::Button*>(find(demo, "track.add"));
        if (add) { add->triggerClick(); expectEquals(demo.state().tracks, 6); }
        auto* ruler = dynamic_cast<editor::TimelineRuler*>(find(demo, "ruler"));
        auto* lower = dynamic_cast<editor::TimelineRuler*>(find(demo, "pitch.ruler"));
        auto* pitch = dynamic_cast<editor::PitchEditor*>(find(demo, "pitch"));
        expect(ruler && lower && pitch);
        if (ruler && lower && pitch)
        {
            beginTest("Time origins agree between timeline and piano roll through zoom and seek");
            ruler->onSeek(240.0); expectWithinAbsoluteError(demo.state().playhead, 240.0, 0.0001);
            lower->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
            expectWithinAbsoluteError(demo.state().playhead, 241.0, 0.0001);
            const auto before = demo.state();
            ruler->onViewportGesture(0, 1.5, 300);
            expectWithinAbsoluteError(demo.state().pixelsPerBeat, before.pixelsPerBeat * 1.5, 0.001);
            const auto offset = static_cast<float>(lower->getBoundsInParent().getX() - ruler->getBoundsInParent().getX());
            const auto viewBefore = demo.state();
            lower->onViewportGesture(0, 1.2, 300 - offset);
            expectWithinAbsoluteError(demo.state().firstBeat + 300.0 / demo.state().pixelsPerBeat,
                                      viewBefore.firstBeat + 300.0 / viewBefore.pixelsPerBeat, 0.001);
            beginTest("Pencil and split change only demonstration notes");
            const auto count = demo.state().notes;
            pitch->onEditRequested(editor::PitchTool::pencil, -1, 240, 64);
            expectEquals(demo.state().notes, count + 1);
            pitch->onEditRequested(editor::PitchTool::scissors, demo.state().selectedNote, 240.125, 64);
            expectEquals(demo.state().notes, count + 2);
            demo.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
            expectEquals(demo.state().notes, count + 1);
            beginTest("Painting does not regenerate cached geometry");
            const auto revision = pitch->geometryRevision();
            const auto image = demo.createComponentSnapshot(demo.getLocalBounds());
            expect(image.isValid()); expectEquals(pitch->geometryRevision(), revision);
        }
        beginTest("Small and large layouts preserve useful pitch area");
        for (const auto size : { juce::Point<int>(1000, 700), juce::Point<int>(1920, 1080) })
        {
            demo.setSize(size.x, size.y);
            if (pitch) { expect(pitch->getWidth() > 800); expect(pitch->getHeight() >= 160); }
        }
        demo.removeFromDesktop();
    }
};
IntegrationTests integrationTests;
}
int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.runAllTests(123456);
    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) failures += runner.getResult(i)->failures;
    obsidian::ui::runAsyncDeletionChecks([&failures](bool success)
    {
        if (!success) { ++failures; std::cerr << "Async notification lifecycle test failed\n"; }
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    std::cout << "UI test failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
