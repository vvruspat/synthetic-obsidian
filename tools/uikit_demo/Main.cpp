#include <juce_gui_basics/juce_gui_basics.h>
#include "Demo.h"
#include "Gallery.h"

namespace obsidian::demo
{
class Host final : public juce::Component
{
public:
    Host()
    {
        addAndMakeVisible(demo_); addChildComponent(gallery_);
        demo_.onShowGallery = [this] { showGallery(true); };
        gallery_.onShowDemo = [this] { showGallery(false); };
        setSize(1586, 992);
    }
    void showGallery(bool show) { gallery_.setVisible(show); demo_.setVisible(!show); resized(); }
    void resized() override { demo_.setBounds(getLocalBounds()); gallery_.setBounds(getLocalBounds()); }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getTextCharacter() == 'g' || key.getTextCharacter() == 'G') { showGallery(!gallery_.isVisible()); return true; }
        return false;
    }
private:
    Demo demo_;
    Gallery gallery_;
};

class Application final : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "Obsidian UI Kit"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String& commandLine) override
    {
        auto args = juce::StringArray::fromTokens(commandLine, true);
        args.trim(); args.removeEmptyStrings();
        int width = 1586, height = 992;
        double scale = 1.0;
        bool gallery = false;
       #if OBSIDIAN_GALLERY_APP
        gallery = true;
       #endif
        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--gallery") gallery = true;
            if (args[i] == "--snapshot" && i + 1 < args.size()) snapshot_ = args[++i].unquoted();
            if (args[i] == "--width" && i + 1 < args.size()) width = juce::jlimit(1000, 2560, args[++i].getIntValue());
            if (args[i] == "--height" && i + 1 < args.size()) height = juce::jlimit(700, 1800, args[++i].getIntValue());
            if (args[i] == "--scale" && i + 1 < args.size()) scale = juce::jlimit(1.0, 3.0, args[++i].getDoubleValue());
        }
        snapshotScale_ = scale;
        window_ = std::make_unique<Window>(getApplicationName(), *this);
        host_ = new Host;
        host_->setSize(width, height); host_->showGallery(gallery);
        window_->setContentOwned(host_, true);
        window_->setResizable(true, false); window_->setResizeLimits(1000, 700, 2560, 1800);
        window_->centreWithSize(window_->getWidth(), window_->getHeight()); window_->setVisible(true);
        if (snapshot_.isNotEmpty()) startTimer(300);
    }
    void shutdown() override { stopTimer(); host_ = nullptr; window_.reset(); }
    void systemRequestedQuit() override { quit(); }
private:
    class Window final : public juce::DocumentWindow
    {
    public:
        Window(const juce::String& title, Application& app)
            : DocumentWindow(title, juce::Colour(0xff1b2023), allButtons), app_(app)
        { setUsingNativeTitleBar(true); }
        void closeButtonPressed() override { app_.systemRequestedQuit(); }
    private:
        Application& app_;
    };
    void timerCallback() override
    {
        stopTimer();
        const juce::File target(snapshot_);
        target.getParentDirectory().createDirectory();
        const auto image = host_->createComponentSnapshot(host_->getLocalBounds(), true, static_cast<float>(snapshotScale_));
        juce::FileOutputStream stream(target);
        // FileOutputStream opens existing files at the end. A snapshot replaces
        // its previous PNG rather than appending another image to the file.
        if (!stream.openedOk() || !stream.setPosition(0) || stream.truncate().failed()
            || !juce::PNGImageFormat().writeImageToStream(image, stream))
            setApplicationReturnValue(1);
        stream.flush();
        if (stream.getStatus().failed()) setApplicationReturnValue(1);
        quit();
    }
    std::unique_ptr<Window> window_;
    Host* host_ = nullptr; // Owned by window_'s content component.
    juce::String snapshot_;
    double snapshotScale_ = 1.0;
};
}
START_JUCE_APPLICATION(obsidian::demo::Application)
