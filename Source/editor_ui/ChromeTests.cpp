#include "Chrome.h"

namespace obsidian::editor
{
namespace
{
class ChromeTests final : public juce::UnitTest
{
public:
    ChromeTests() : UnitTest("Editor chrome", "Obsidian UI") {}
    void runTest() override
    {
        beginTest("Signature determines bars, denominator beats and quarter-note ticks");
        TransportBar transport;
        transport.setPosition(5.5, 0.0);
        expectEquals(transport.getPositionText(), juce::String("002 | 2 | 480"));
        transport.setSignature(6, 8);
        expectEquals(transport.getSignatureText(), juce::String("6 / 8"));
        transport.setPosition(3.0, 0.0);
        expectEquals(transport.getPositionText(), juce::String("002 | 1 | 000"));
        transport.setPosition(5.75, 0.0);
        expectEquals(transport.getPositionText(), juce::String("002 | 6 | 240"));
        transport.setSignature(3, 4);
        transport.setPosition(6.0, 0.0);
        expectEquals(transport.getPositionText(), juce::String("003 | 1 | 000"));
        transport.setSignature(0, 8);
        transport.setSignature(7, 3);
        expectEquals(transport.getSignatureText(), juce::String("3 / 4"));

        beginTest("Externally supplied title and model values remain silent");
        auto callbacks = 0;
        transport.onPlay = [&](bool) { ++callbacks; };
        transport.onRecord = [&](bool) { ++callbacks; };
        transport.onLoop = [&](bool) { ++callbacks; };
        transport.onMasterVolume = [&](double) { ++callbacks; };
        transport.onProjectAction = [&](int) { ++callbacks; };
        transport.setProjectTitle("External project");
        transport.setProjectActions({ "Custom action", "Another action" });
        transport.setPlaying(true);
        transport.setRecording(true);
        transport.setLooping(true);
        transport.setMasterVolume(0.5);
        transport.setSignature(7, 8);
        transport.setTempo(120.0);
        transport.setPosition(10.0, 5.0);
        expectEquals(transport.getProjectTitle(), juce::String("External project"));
        expectEquals(callbacks, 0);

        beginTest("Transport callback may destroy its owning view");
        auto disposable = std::make_unique<TransportBar>();
        disposable->onPlay = [&](bool playing)
        {
            expect(playing);
            disposable.reset();
        };
        disposable->getPlayButton().triggerClick();
        expect(disposable == nullptr);

        beginTest("Compact track controls stay beside the name when rows grow");
        TrackHeader compact;
        compact.setTrack(2, "Voice Main", ui::colour(ui::tokens::olive), ui::Icon::none);
        compact.setSize(262, 70); compact.setGainVisible(false);
        expectEquals(compact.getMuteButton().getX(), 58); expectEquals(compact.getMuteButton().getY(), 33);
        expectEquals(compact.getSoloButton().getX(), 84); expectEquals(compact.getSoloButton().getY(), 33);
        expect(compact.getMuteButton().getBounds().getIntersection(compact.getSoloButton().getBounds()).isEmpty());
        compact.setSize(262, 140);
        expectEquals(compact.getMuteButton().getY(), 33); expectEquals(compact.getSoloButton().getY(), 33);
        expect(!compact.getGainSlider().isVisible());
        compact.setGainVisible(true); expect(compact.getGainSlider().isVisible());
        expect(compact.getGainSlider().getX() > compact.getSoloButton().getRight());

        beginTest("Active mute is blue and active solo is yellow with silent model updates");
        auto muteCalls = 0, soloCalls = 0;
        compact.onMute = [&](bool) { ++muteCalls; }; compact.onSolo = [&](bool) { ++soloCalls; };
        compact.setMuted(true); compact.setSolo(true);
        expectEquals(muteCalls, 0); expectEquals(soloCalls, 0);
        const auto muteImage = compact.getMuteButton().createComponentSnapshot(compact.getMuteButton().getLocalBounds());
        const auto soloImage = compact.getSoloButton().createComponentSnapshot(compact.getSoloButton().getLocalBounds());
        const auto muteColour = muteImage.getPixelAt(5, 12), soloColour = soloImage.getPixelAt(5, 12);
        expect(muteColour.getBlue() > muteColour.getRed());
        expect(soloColour.getRed() > soloColour.getBlue() && soloColour.getGreen() > soloColour.getBlue());
        expect(muteColour != soloColour);
        compact.getMuteButton().triggerClick(); compact.getSoloButton().triggerClick();
        expectEquals(muteCalls, 1); expectEquals(soloCalls, 1);

        beginTest("Compact backing action preserves its icon, accessible name and callback");
        TrackGroupHeader group;
        group.setSize(262, 36); group.setAddCompact(true);
        ui::Button* add = nullptr;
        for (auto* child : group.getChildren()) if (child->getComponentID() == "group.add") add = dynamic_cast<ui::Button*>(child);
        expect(add != nullptr);
        if (add != nullptr)
        {
            expectEquals(add->getWidth(), 28); expect(add->getText().isEmpty());
            expectEquals(add->getTitle(), juce::String("Add backing vocal track"));
            auto adds = 0; group.onAdd = [&] { ++adds; }; add->triggerClick(); expectEquals(adds, 1);
            group.setAddCompact(false); expectEquals(add->getWidth(), 116); expectEquals(add->getText(), juce::String("Add backing"));
        }

        beginTest("Track callback may destroy its owning view");
        auto track = std::make_unique<TrackHeader>();
        track->onMute = [&](bool muted)
        {
            expect(muted);
            track.reset();
        };
        track->getMuteButton().triggerClick();
        expect(track == nullptr);
    }
};
ChromeTests chromeTests;
}
}
