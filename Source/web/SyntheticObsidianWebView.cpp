#include "SyntheticObsidianWebView.h"

#include <SyntheticObsidianFrontendData.h>

#include <cstddef>
#include <vector>

namespace synthetic_obsidian
{

namespace
{
using Resource = juce::WebBrowserComponent::Resource;

Resource makeResource(const char* data, int size, const char* mimeType)
{
    const auto* begin = reinterpret_cast<const std::byte*>(data);
    return { std::vector<std::byte>(begin, begin + size), mimeType };
}
} // namespace

SyntheticObsidianWebView::LocalBrowser::LocalBrowser(const Options& options)
    : juce::WebBrowserComponent(options)
{
}

bool SyntheticObsidianWebView::LocalBrowser::pageAboutToLoad(const juce::String& newUrl)
{
    return newUrl.startsWith(juce::WebBrowserComponent::getResourceProviderRoot())
        || newUrl == "about:blank";
}

SyntheticObsidianWebView::SyntheticObsidianWebView(CommandHandler commandHandler)
    : commandHandler_(std::move(commandHandler)),
      browser_(makeOptions())
{
    addAndMakeVisible(browser_);
    browser_.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
}

void SyntheticObsidianWebView::dispatch(const juce::var& event)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    const auto eventJson = juce::JSON::toString(event);
    browser_.evaluateJavascript(
        "window.syntheticObsidianDispatch?.(JSON.stringify(" + eventJson + "));");
}

void SyntheticObsidianWebView::resized()
{
    browser_.setBounds(getLocalBounds());
}

juce::WebBrowserComponent::Options SyntheticObsidianWebView::makeOptions()
{
    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withUserScript(R"javascript(
            window.syntheticObsidianHost = {
                postMessage(message) {
                    window.__JUCE__.backend.emitEvent("syntheticObsidianCommand", message);
                }
            };
        )javascript")
        .withEventListener(
            "syntheticObsidianCommand",
            [this](const juce::var& payload)
            {
                const auto command = juce::JSON::parse(payload.toString());
                if (command.isObject() && commandHandler_)
                    commandHandler_(command);
            })
        .withResourceProvider(
            [](const juce::String& path)
            {
                return getResource(path);
            });

#if JUCE_WINDOWS
    options = options
        .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options(
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder(
                    juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getChildFile("SyntheticObsidianWebView2"))
                .withStatusBarDisabled()
                .withBuiltInErrorPageDisabled()
                .withBackgroundColour(juce::Colour(0xff01040b)));
#endif

    return options;
}

std::optional<juce::WebBrowserComponent::Resource>
SyntheticObsidianWebView::getResource(const juce::String& path)
{
    if (path == "/" || path == "/index.html")
        return makeResource(
            SyntheticObsidianFrontendData::index_html,
            SyntheticObsidianFrontendData::index_htmlSize,
            "text/html");

    if (path == "/assets/app.js")
        return makeResource(
            SyntheticObsidianFrontendData::app_js,
            SyntheticObsidianFrontendData::app_jsSize,
            "text/javascript");

    if (path == "/assets/app.css")
        return makeResource(
            SyntheticObsidianFrontendData::app_css,
            SyntheticObsidianFrontendData::app_cssSize,
            "text/css");

    if (path == "/favicon.svg")
        return makeResource(
            SyntheticObsidianFrontendData::favicon_svg,
            SyntheticObsidianFrontendData::favicon_svgSize,
            "image/svg+xml");

    return std::nullopt;
}

} // namespace synthetic_obsidian
