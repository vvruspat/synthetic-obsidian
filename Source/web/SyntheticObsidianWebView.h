#pragma once

#include <JuceHeader.h>

#include <functional>
#include <optional>

namespace synthetic_obsidian
{

class SyntheticObsidianWebView final : public juce::Component
{
public:
    using CommandHandler = std::function<void(const juce::var&)>;

    explicit SyntheticObsidianWebView(CommandHandler commandHandler);

    void dispatch(const juce::var& event);
    void resized() override;

private:
    class LocalBrowser final : public juce::WebBrowserComponent
    {
    public:
        explicit LocalBrowser(const Options& options);

        bool pageAboutToLoad(const juce::String& newUrl) override;
    };

    juce::WebBrowserComponent::Options makeOptions();
    static std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& path);

    CommandHandler commandHandler_;
    LocalBrowser browser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SyntheticObsidianWebView)
};

} // namespace synthetic_obsidian
