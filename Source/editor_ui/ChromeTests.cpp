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
