#include "Gallery.h"

namespace obsidian::demo
{
namespace
{
juce::MouseEvent dragStart(ui::Slider& component)
{
    const juce::Point<float> position(40.0f, 12.0f);
    const auto time = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), position,
             juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, time, position, time, 1, false };
}

class GalleryTests final : public juce::UnitTest
{
public:
    GalleryTests() : UnitTest("Gallery lifecycle", "Obsidian UI") {}
    void runTest() override
    {
        beginTest("Gallery ends active gain and zoom gestures before destroying callback state");
        for (const bool gain : { true, false })
        {
            auto gallery = std::make_unique<Gallery>();
            gallery->setBounds(-2000, -2000, 1000, 700);
            gallery->addToDesktop(0); gallery->setVisible(true);
            auto& slider = gain ? gallery->getGainSlider() : gallery->getZoomSlider();
            const auto end = slider.onDragEnd;
            auto* view = gallery.get();
            int endings = 0;
            slider.onDragEnd = [&, end, view]
            {
                ++endings;
                // The original callback appends to the gallery's log. This must
                // happen while that log is alive, including under ASan.
                end();
                expect(view->getEventLog()[0].contains("gesture ended"));
            };
            slider.mouseDown(dragStart(slider));
            expect(slider.isDragging());
            gallery.reset();
            expectEquals(endings, 1);
        }
    }
};
GalleryTests galleryTests;
}
}
