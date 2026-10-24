#pragma once

#include <JuceHeader.h>
#include <map>

namespace synthetic_obsidian
{
// Owned and called by the WebView's single background worker. Completed files remain
// available to unsaved projects; saving a project consolidates them into its assets.
class AudioFileTransfer final
{
public:
    explicit AudioFileTransfer(juce::File directory);
    ~AudioFileTransfer();
    juce::var process(const juce::var& request);

private:
    struct PendingFile
    {
        juce::File partial;
        juce::File destination;
        juce::int64 size = 0;
        juce::int64 written = 0;
    };
    juce::File directory_;
    std::map<juce::String, PendingFile> pending_;
};
}
