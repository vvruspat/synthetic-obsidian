#include "NativeStudioSettings.h"
#include "uikit/Controls.h"

#include <utility>
#include <vector>

namespace vocal_annotation
{
namespace
{
namespace ui = obsidian::ui;
using Properties = std::initializer_list<std::pair<juce::Identifier, juce::var>>;
juce::var object(Properties properties)
{
    auto result = std::make_unique<juce::DynamicObject>();
    for (const auto& property : properties) result->setProperty(property.first, property.second);
    return result.release();
}
juce::var array(std::initializer_list<juce::var> values)
{
    juce::Array<juce::var> result;
    for (const auto& value : values) result.add(value);
    return result;
}
juce::Component* findComponent(juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id) return &component;
    for (auto* child : component.getChildren()) if (auto* found = findComponent(*child, id)) return found;
    return nullptr;
}
juce::var audioEvent()
{
    return object({ { "type", "audio-settings-state" }, { "state", object({
        { "deviceType", "Test driver" }, { "inputDevices", array({ "Microphone A", "Microphone B" }) },
        { "outputDevices", array({ "Output A", "Output B" }) }, { "inputDevice", "Microphone A" },
        { "outputDevice", "Output B" }, { "sampleRate", 48000.0 }, { "bufferSize", 256 }, { "error", "" }
    }) } });
}
juce::var voiceEvent(bool canCreate = true, bool canTrain = true, juce::String phase = "ready", bool canCancelJob = true)
{
    return object({ { "type", "voice-training-state" }, { "state", object({
        { "phase", phase }, { "message", "Ready for local preparation" }, { "progress", 0.4 },
        { "device", "Test CPU" }, { "runtimeStatus", "ready" }, { "runtimeMessage", "Runtime available" },
        { "canCreateProfile", canCreate }, { "canStartTraining", canTrain }, { "fineTuningAvailable", true },
        { "activeProfileName", "" }, { "outputDirectory", "/test/voice-profile" },
        { "totalDurationSeconds", 40.0 }, { "minimumDurationSeconds", 30.0 }, { "recommendedDurationSeconds", 180.0 },
        { "sources", array({ object({ { "id", "source-a" }, { "name", "Clean vocal.wav" },
            { "durationSeconds", 40.0 }, { "sampleRate", 48000.0 }, { "channels", 1 }, { "status", "ready" }, { "message", "Usable solo vocal" } }) }) },
        { "jobs", array({ object({ { "jobId", "job-a" }, { "presetName", "Singer A" }, { "status", "training" },
            { "stage", "Training adapter" }, { "message", "Learning" }, { "progress", 0.25 },
            { "currentStep", 25 }, { "totalSteps", 100 }, { "loss", 0.125 }, { "canCancel", canCancelJob } }) }) }
    }) } });
}
juce::var projectEvent()
{
    return object({ { "type", "project-state" }, { "project", object({
        { "voiceProfiles", array({ object({ { "id", "source-timbre" }, { "name", "Source timbre" }, { "quality", "balanced" } }),
                                    object({ { "id", "profile-a" }, { "name", "Singer A" }, { "quality", "high" } }) }) },
        { "tracks", array({ object({ { "id", "instrumental-a" }, { "name", "Instrumental" } }),
                              object({ { "id", "vocal-a" }, { "name", "Voice Main" } }),
                              object({ { "id", "vocal-b" }, { "name", "Audio 3" } }) }) },
        { "backingTrackOptions", array({ "Unison Double", "Third Above" }) },
        { "initialBackingTracks", array({ object({ { "id", "backing-a" }, { "name", "Backing A" } }),
                                           object({ { "id", "backing-b" }, { "name", "Backing B" } }) }) },
        { "backingTrackContents", array({ object({ { "id", "backing-a" }, { "voiceProfileId", "profile-a" } }),
                                          object({ { "id", "backing-b" }, { "voiceProfileId", "source-timbre" } }) }) }
    }) } });
}
class NativeStudioSettingsTests final : public juce::UnitTest
{
public:
    NativeStudioSettingsTests() : UnitTest("Native studio settings", "NativeStudio") {}
    void runTest() override
    {
        juce::Component testRoot;
        testRoot.setBounds(-4000, -4000, 800, 700);
        testRoot.addToDesktop(0); testRoot.setVisible(true);
        std::vector<juce::var> commands;
        NativeStudioSettings settings([&](const juce::var& command) { commands.push_back(command); });
        testRoot.addChildComponent(settings); settings.setBounds(testRoot.getLocalBounds());

        beginTest("Opening requests real device and voice state");
        expect(commands.empty());
        settings.setVisible(true);
        expectEquals(static_cast<int>(commands.size()), 2);
        if (commands.size() == 2)
        {
            expectEquals(commands[0]["type"].toString(), juce::String("audio-settings"));
            expectEquals(commands[0]["action"].toString(), juce::String("request"));
            expectEquals(commands[1]["type"].toString(), juce::String("voice-training"));
            expectEquals(commands[1]["action"].toString(), juce::String("request-state"));
        }
        commands.clear();
        settings.dispatch(audioEvent()); settings.dispatch(voiceEvent()); settings.dispatch(projectEvent());
        expect(commands.empty(), "Backend snapshots must not emit user commands");

        auto* input = control<ui::Select>(settings, "settings.audio.input");
        auto* output = control<ui::Select>(settings, "settings.audio.output");
        auto* apply = control<ui::Button>(settings, "settings.audio.apply");
        beginTest("Explicit no-input maps to an empty wire value and output remains selected");
        if (input && output && apply)
        {
            expectEquals(input->getSelectedText(), juce::String("Microphone A"));
            expectEquals(output->getSelectedText(), juce::String("Output B"));
            input->setSelectedIndex(0, juce::sendNotificationSync);
            apply->triggerClick();
            expectCommand(commands, "audio-settings", "set-devices");
            if (!commands.empty())
            {
                expectEquals(commands.back()["inputDevice"].toString(), juce::String());
                expectEquals(commands.back()["outputDevice"].toString(), juce::String("Output B"));
            }
            commands.clear();
            input->setSelectedIndex(2, juce::sendNotificationSync);
            output->setSelectedIndex(0, juce::sendNotificationSync);
            apply->triggerClick();
            expectCommand(commands, "audio-settings", "set-devices");
            if (!commands.empty()) expectEquals(commands.back()["inputDevice"].toString(), juce::String("Microphone B"));
        }
        for (const auto& action : { std::pair { "settings.audio.test", "test-output" },
                                    std::pair { "settings.audio.refresh", "request" } })
        {
            commands.clear(); click(settings, action.first); expectCommand(commands, "audio-settings", action.second);
        }

        beginTest("Voice state flags govern create and train without snapshot notifications");
        auto* create = control<ui::Button>(settings, "settings.voice.create");
        auto* train = control<ui::Button>(settings, "settings.voice.train");
        auto* name = control<juce::TextEditor>(settings, "settings.voice.name");
        auto* quality = control<ui::Select>(settings, "settings.voice.quality");
        commands.clear(); settings.dispatch(voiceEvent(false, false));
        if (create && train)
        {
            expect(!create->isEnabled()); expect(!train->isEnabled());
            create->triggerClick(); train->triggerClick(); expect(commands.empty());
            settings.dispatch(voiceEvent(true, false)); expect(create->isEnabled()); expect(!train->isEnabled());
        }
        if (name && quality && create && train)
        {
            name->setText("Singer request", false); quality->setSelectedIndex(1);
            name->setText("", false); settings.dispatch(voiceEvent(true, true));
            expect(!create->isEnabled()); expect(!train->isEnabled());
            name->setText("Singer request", false); settings.dispatch(voiceEvent(true, true));
            for (int index = 0; index < 3; ++index) settings.dispatch(voiceEvent(true, true));
            expect(commands.empty()); expectEquals(name->getText(), juce::String("Singer request"));
            create->triggerClick(); expectCommand(commands, "voice-training", "create-profile");
            if (!commands.empty())
            {
                expectEquals(commands.back()["presetName"].toString(), juce::String("Singer request"));
                expectEquals(commands.back()["quality"].toString(), juce::String("high"));
            }
            commands.clear(); train->triggerClick(); expectCommand(commands, "voice-training", "start-training");
            commands.clear(); settings.dispatch(voiceEvent(true, true, "preparing"));
            expect(!create->isEnabled()); expect(!train->isEnabled());
            click(settings, "settings.voice.cancel"); expectCommand(commands, "voice-training", "cancel");
            settings.dispatch(voiceEvent());
        }
        for (const auto& action : { std::pair { "settings.voice.add-files", "add-files" },
                                    std::pair { "settings.voice.current", "add-current-track" },
                                    std::pair { "settings.voice.clear", "clear-files" },
                                    std::pair { "settings.voice.reveal", "reveal-profile" } })
        {
            commands.clear(); click(settings, action.first); expectCommand(commands, "voice-training", action.second);
        }
        commands.clear(); click(settings, "settings.source.remove.source-a");
        expectCommand(commands, "voice-training", "remove-file");
        if (!commands.empty()) expectEquals(commands.back()["sourceId"].toString(), juce::String("source-a"));
        commands.clear(); click(settings, "settings.job.action.job-a");
        expectCommand(commands, "voice-training", "cancel-training");
        if (!commands.empty()) expectEquals(commands.back()["jobId"].toString(), juce::String("job-a"));
        commands.clear(); settings.dispatch(voiceEvent(true, true, "ready", false));
        click(settings, "settings.job.action.job-a"); expectCommand(commands, "voice-training", "reveal-training");

        beginTest("Repeated voice snapshots preserve focused row controls and lifecycle");
        auto* sourceAction = control<ui::Button>(settings, "settings.source.remove.source-a");
        auto* jobAction = control<ui::Button>(settings, "settings.job.action.job-a");
        commands.clear();
        if (sourceAction && jobAction)
        {
            sourceAction->grabKeyboardFocus(); expect(sourceAction->hasKeyboardFocus(false));
            settings.dispatch(voiceEvent());
            expect(findComponent(settings, "settings.source.remove.source-a") == sourceAction);
            expect(sourceAction->hasKeyboardFocus(false));
            jobAction->grabKeyboardFocus(); expect(jobAction->hasKeyboardFocus(false));
            settings.dispatch(voiceEvent(true, true, "ready", false));
            expect(findComponent(settings, "settings.job.action.job-a") == jobAction);
            expect(jobAction->hasKeyboardFocus(false));
        }
        expect(commands.empty());
        if (sourceAction)
        {
            sourceAction->grabKeyboardFocus();
            juce::Component::SafePointer<ui::Button> deletedSource(sourceAction);
            auto removedSource = voiceEvent();
            removedSource["state"].getDynamicObject()->setProperty("sources", array({}));
            settings.dispatch(removedSource);
            expect(deletedSource == nullptr);
            expect(commands.empty());
        }

        beginTest("Backing source seeding is silent and survives a delayed project snapshot");
        std::vector<juce::var> pendingCommands;
        NativeStudioSettings pending([&](const juce::var& command) { pendingCommands.push_back(command); });
        testRoot.addChildComponent(pending);
        pending.setBounds(testRoot.getLocalBounds());
        pending.setBackingParent("future-vocal");
        pending.dispatch(projectEvent());
        auto laterProject = projectEvent();
        auto* laterTracks = laterProject["project"]["tracks"].getArray();
        expect(laterTracks != nullptr);
        if (laterTracks != nullptr) laterTracks->add(object({ { "id", "future-vocal" }, { "name", "Future vocal" } }));
        pending.dispatch(laterProject);
        auto* pendingParent = control<ui::Select>(pending, "settings.backing.parent");
        if (pendingParent) expectEquals(pendingParent->getSelectedText(), juce::String("Future vocal"));
        pending.setBackingParent("vocal-b");
        if (pendingParent) expectEquals(pendingParent->getSelectedText(), juce::String("Audio 3"));
        expect(pendingCommands.empty());

        beginTest("Backing generation preserves generator, source parent and target identities");
        auto* parent = control<ui::Select>(settings, "settings.backing.parent");
        auto* style = control<ui::Select>(settings, "settings.backing.style");
        auto* generator = control<ui::Select>(settings, "settings.backing.generator");
        auto* target = control<ui::Select>(settings, "settings.backing.target");
        auto* voice = control<ui::Select>(settings, "settings.backing.voice");
        commands.clear(); settings.dispatch(projectEvent());
        if (parent && style && generator && target && voice)
        {
            expectEquals(parent->getSelectedText(), juce::String("Voice Main"));
            expectEquals(voice->getSelectedText(), juce::String("Singer A"));
            parent->setSelectedIndex(2); style->setSelectedIndex(1);
            parent->grabKeyboardFocus(); settings.dispatch(projectEvent());
            expect(parent->hasKeyboardFocus(false));
            expectEquals(parent->getSelectedText(), juce::String("Audio 3"));
            expectEquals(style->getSelectedText(), juce::String("Third Above"));
            expect(commands.empty());
            for (int index = 0; index < 3; ++index)
            {
                commands.clear(); generator->setSelectedIndex(index); click(settings, "settings.backing.generate");
                expectCommand(commands, "add-backing-track");
                if (!commands.empty())
                {
                    expectEquals(commands.back()["parentId"].toString(), juce::String("vocal-b"));
                    expectEquals(commands.back()["track"].toString(), juce::String("Third Above"));
                    expectEquals(commands.back()["generator"].toString(), juce::String(index == 0 ? "local" : index == 1 ? "luna" : "terra"));
                }
            }
            target->setSelectedIndex(1, juce::sendNotificationSync);
            expectEquals(voice->getSelectedText(), juce::String("Source timbre"));
            commands.clear(); voice->setSelectedIndex(1, juce::sendNotificationSync);
            expectCommand(commands, "set-backing-voice-profile");
            if (!commands.empty())
            {
                expectEquals(commands.back()["track"].toString(), juce::String("backing-b"));
                expectEquals(commands.back()["profileId"].toString(), juce::String("profile-a"));
            }
            for (const auto& action : { std::pair { "settings.backing.regenerate", "regenerate-backing-track" },
                                        std::pair { "settings.backing.render", "render-backing-track" },
                                        std::pair { "settings.backing.remove", "remove-backing-track" } })
            {
                commands.clear(); click(settings, action.first); expectCommand(commands, action.second);
                if (!commands.empty()) expectEquals(commands.back()["track"].toString(), juce::String("backing-b"));
            }
        }

        beginTest("Profile deletion requires explicit confirmation and preserves source timbre");
        auto* profiles = control<ui::Select>(settings, "settings.profile.select");
        auto* deleteProfile = control<ui::Button>(settings, "settings.profile.delete");
        if (profiles && deleteProfile)
        {
            commands.clear(); profiles->setSelectedIndex(0, juce::sendNotificationSync);
            expect(!deleteProfile->isEnabled()); deleteProfile->triggerClick(); expect(commands.empty());
            profiles->setSelectedIndex(1, juce::sendNotificationSync);
            deleteProfile->triggerClick(); expect(commands.empty());
            deleteProfile->triggerClick(); expectCommand(commands, "delete-voice-profile");
            if (!commands.empty()) expectEquals(commands.back()["profileId"].toString(), juce::String("profile-a"));
        }
        beginTest("Busy backend state prevents backing mutations");
        commands.clear();
        settings.dispatch(object({ { "type", "status-state" }, { "backingGenerationRunning", true }, { "message", "Generating" } }));
        for (auto id : { "settings.backing.generate", "settings.backing.regenerate", "settings.backing.render", "settings.backing.remove" })
        { auto* button = control<ui::Button>(settings, id); if (button) { expect(!button->isEnabled()); button->triggerClick(); } }
        expect(commands.empty());

        beginTest("Close and Escape notify the host without sending backend work");
        auto closes = 0; settings.onClose = [&] { ++closes; };
        click(settings, "settings.close"); expectEquals(closes, 1);
        expect(settings.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))); expectEquals(closes, 2);
        expect(commands.empty());

        beginTest("Host may destroy settings while a refresh command is handled");
        std::unique_ptr<NativeStudioSettings> disposable;
        auto refreshCalls = 0;
        disposable = std::make_unique<NativeStudioSettings>([&](const juce::var&)
        { ++refreshCalls; disposable.reset(); });
        disposable->refresh(); expect(disposable == nullptr); expectEquals(refreshCalls, 1);
    }
private:
    template <typename Control> Control* control(juce::Component& root, const juce::String& id)
    {
        auto* result = dynamic_cast<Control*>(findComponent(root, id));
        expect(result != nullptr, "Missing control " + id); return result;
    }
    void click(juce::Component& root, const juce::String& id)
    { if (auto* button = control<ui::Button>(root, id)) button->triggerClick(); }
    void expectCommand(const std::vector<juce::var>& commands, const juce::String& type, const juce::String& action = {})
    {
        expectEquals(static_cast<int>(commands.size()), 1, "Expected one " + type + " command");
        if (commands.empty()) return;
        expectEquals(commands.back()["type"].toString(), type);
        if (action.isNotEmpty()) expectEquals(commands.back()["action"].toString(), action);
    }
};
NativeStudioSettingsTests nativeStudioSettingsTests;
}
}
