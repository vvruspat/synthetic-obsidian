#include "StudioShortcuts.h"
#include "uikit/Controls.h"

namespace vocal_annotation
{
namespace
{
namespace ui = obsidian::ui;
juce::Component* findShortcutComponent(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren()) if (auto* found = findShortcutComponent(*child, id)) return found;
    return nullptr;
}
class StudioShortcutsTests final : public juce::UnitTest
{
public:
    StudioShortcutsTests() : UnitTest("Studio keyboard shortcuts", "NativeStudio") {}
    void runTest() override
    {
        beginTest("All 34 default actions have distinct working bindings");
        StudioShortcuts defaults;
        expectEquals(static_cast<int>(StudioShortcuts::definitions().size()), 34);
        for (const auto& definition : StudioShortcuts::definitions())
        {
            expect(defaults.getKey(definition.id) == definition.defaultKey, definition.id);
            expectEquals(defaults.actionFor(definition.defaultKey), definition.id);
        }
        expect(!defaults.getKey("unknown.action").isValid());
        expect(defaults.actionFor(juce::KeyPress()).isEmpty());
        const juce::KeyPress unbound('Q', juce::ModifierKeys::altModifier, 0);
        expect(defaults.actionFor(unbound).isEmpty());
        defaults.assign("unknown.action", unbound);
        expect(defaults.actionFor(unbound).isEmpty());

        beginTest("Binding assignment, clearing and duplicate transfer persist exactly");
        juce::TemporaryFile preferences(".json");
        StudioShortcuts persistent(preferences.getFile());
        const juce::KeyPress custom('K', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0);
        persistent.assign("project.save", custom);
        expect(persistent.error().isEmpty());
        expect(preferences.getFile().existsAsFile());
        StudioShortcuts reloaded(preferences.getFile());
        expect(reloaded.getKey("project.save") == custom);
        expectEquals(reloaded.actionFor(custom), juce::String("project.save"));
        expect(reloaded.actionFor(defaults.getKey("project.save")).isEmpty());
        const auto pointerKey = defaults.getKey("tool.left.pointer");
        reloaded.assign("project.save", pointerKey);
        expect(!reloaded.getKey("tool.left.pointer").isValid());
        expectEquals(reloaded.actionFor(pointerKey), juce::String("project.save"));
        StudioShortcuts transferred(preferences.getFile());
        expect(!transferred.getKey("tool.left.pointer").isValid());
        expect(transferred.getKey("project.save") == pointerKey);
        transferred.assign("project.save", {});
        StudioShortcuts cleared(preferences.getFile());
        expect(!cleared.getKey("project.save").isValid());
        expect(cleared.actionFor(pointerKey).isEmpty());
        cleared.restoreDefaults();
        StudioShortcuts reset(preferences.getFile());
        for (const auto& definition : StudioShortcuts::definitions()) expect(reset.getKey(definition.id) == definition.defaultKey);

        beginTest("Malformed JSON retains defaults; malformed fields do not replace valid defaults");
        juce::TemporaryFile malformed(".json");
        expect(malformed.getFile().replaceWithText("{ not valid JSON"));
        StudioShortcuts broken(malformed.getFile());
        for (const auto& definition : StudioShortcuts::definitions()) expect(broken.getKey(definition.id) == definition.defaultKey);
        expect(malformed.getFile().replaceWithText(R"({"project.save":123,"project.open":true,"project.new":[],"transport.play-pause":null,"tool.left.pointer":"","unknown.action":"Q"})"));
        StudioShortcuts malformedFields(malformed.getFile());
        expect(malformedFields.getKey("project.save") == defaults.getKey("project.save"));
        expect(malformedFields.getKey("project.open") == defaults.getKey("project.open"));
        expect(malformedFields.getKey("project.new") == defaults.getKey("project.new"));
        expect(!malformedFields.getKey("transport.play-pause").isValid());
        expect(!malformedFields.getKey("tool.left.pointer").isValid());
        expect(!malformedFields.getKey("unknown.action").isValid());

        beginTest("Shortcut editor captures keys, transfers duplicates and clears bindings");
        juce::Component root;
        root.setBounds(-4000, -4000, 760, 600); root.addToDesktop(0); root.setVisible(true);
        auto model = std::make_shared<StudioShortcuts>();
        StudioShortcutEditor editor(model);
        root.addAndMakeVisible(editor); editor.setBounds(root.getLocalBounds());
        auto* save = control(editor, "settings.shortcut.project.save");
        auto* pointer = control(editor, "settings.shortcut.tool.left.pointer");
        auto* restore = control(editor, "settings.shortcuts.reset");
        if (save && pointer && restore)
        {
            save->grabKeyboardFocus();
            expect(save->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
            expectEquals(save->getText(), juce::String("Press keys..."));
            expect(save->keyPressed(custom));
            expect(model->getKey("project.save") == custom);
            expectEquals(save->getText(), custom.getTextDescriptionWithIcons());
            save->triggerClick(); expect(save->keyPressed(pointerKey));
            expect(model->getKey("project.save") == pointerKey);
            expect(!model->getKey("tool.left.pointer").isValid());
            expectEquals(pointer->getText(), juce::String("Unassigned"));
            save->triggerClick(); expect(save->keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey)));
            expect(!model->getKey("project.save").isValid()); expectEquals(save->getText(), juce::String("Unassigned"));
            save->triggerClick(); expect(save->keyPressed(custom));
            save->triggerClick(); expect(save->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
            expect(!model->getKey("project.save").isValid());

            beginTest("Escape and focus loss cancel recording without changing shortcuts");
            save->triggerClick(); expect(save->keyPressed(custom));
            save->triggerClick(); expect(save->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
            expect(model->getKey("project.save") == custom);
            expectEquals(save->getText(), custom.getTextDescriptionWithIcons());
            save->triggerClick();
            save->focusLost(juce::Component::focusChangedDirectly);
            expect(!save->keyPressed(pointerKey));
            expect(model->getKey("project.save") == custom);
            expectEquals(save->getText(), custom.getTextDescriptionWithIcons());
            restore->triggerClick();
            for (const auto& definition : StudioShortcuts::definitions()) expect(model->getKey(definition.id) == definition.defaultKey);
            expectEquals(save->getText(), defaults.getKey("project.save").getTextDescriptionWithIcons());
            expectEquals(pointer->getText(), pointerKey.getTextDescriptionWithIcons());
        }
    }
private:
    ui::Button* control(juce::Component& root, const juce::String& id)
    {
        auto* result = dynamic_cast<ui::Button*>(findShortcutComponent(root, id));
        expect(result != nullptr, "Missing control " + id); return result;
    }
};
StudioShortcutsTests studioShortcutsTests;
}
}
