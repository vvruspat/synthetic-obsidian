#pragma once
#include <JuceHeader.h>
#include "../state/TrackModel.h"

/** SeedVCBridge
 *
 *  Offline bridge to the research Seed-VC runner.
 *
 *  This bridge launches Python as a subprocess and must only be used from a
 *  background thread. It is intentionally not part of the realtime audio path.
 */
class SeedVCBridge
{
public:
    struct RenderSpec
    {
        juce::String name;
        int semitones { 0 };
        BackVocalStyle style { BackVocalStyle::None };
    };

    struct RenderedFile
    {
        juce::String name;
        int semitones { 0 };
        BackVocalStyle style { BackVocalStyle::None };
        juce::File file;
    };

    using RenderedFiles = std::vector<RenderedFile>;

    SeedVCBridge() = default;

    /** Render all specs for sourceFile into outputRoot.
     *  Returns an empty error string on success. */
    juce::String render (const juce::File& sourceFile,
                         const juce::File& outputRoot,
                         const std::vector<RenderSpec>& specs,
                         RenderedFiles& renderedFiles) const;

    bool isAvailable() const;

private:
    static juce::File findProjectRoot();
    static juce::File findFirstWav (const juce::File& directory);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeedVCBridge)
};
