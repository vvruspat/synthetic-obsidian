#include "SyntheticObsidianLookAndFeel.h"

SyntheticObsidianLookAndFeel::SyntheticObsidianLookAndFeel()
    : bodyFont_(Theme::kFontSizeBody), headerFont_(Theme::kFontSizeHeader)
{
    // Set default colors for various components
    setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(Theme::kBackground));
    setColour(juce::TextButton::buttonColourId, juce::Colour(Theme::kGhostBg));
    setColour(juce::TextButton::textColourOffId, juce::Colour(Theme::kTextPrimary));
    setColour(juce::TextButton::textColourOnId, juce::Colour(Theme::kAccentCyan));
    setColour(juce::Slider::thumbColourId, juce::Colour(Theme::kAccentCyan));
    setColour(juce::Slider::trackColourId, juce::Colour(Theme::kSurface));
    setColour(juce::Label::textColourId, juce::Colour(Theme::kTextPrimary));
    setColour(juce::PopupMenu::backgroundColourId, juce::Colour(Theme::kSurface));
    setColour(juce::PopupMenu::textColourId, juce::Colour(Theme::kTextPrimary));
    setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(Theme::kAccentCyan));
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(Theme::kBackground));
}

void SyntheticObsidianLookAndFeel::drawButtonBackground(
    juce::Graphics& g, juce::Button& button,
    const juce::Colour& backgroundColour,
    bool shouldDrawButtonAsHighlighted,
    bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat();
    const float cornerRadius = Theme::kRadius;

    // Determine if this is a primary/active button
    const bool isPrimary = button.getName().containsIgnoreCase("primary");
    const bool isToggled = button.getToggleState();

    if (isPrimary || isToggled)
    {
        // Primary/active button style with cyan accent
        juce::Colour fillColour = juce::Colour(Theme::kAccentCyan);

        if (shouldDrawButtonAsDown)
            fillColour = fillColour.darker(0.15f);
        else if (shouldDrawButtonAsHighlighted)
            fillColour = fillColour.brighter(0.1f);

        g.setColour(fillColour);
        g.fillRoundedRectangle(bounds, cornerRadius);
    }
    else
    {
        // Ghost button style
        juce::Colour fillColour = juce::Colour(Theme::kGhostBg);

        if (shouldDrawButtonAsDown)
            fillColour = fillColour.withAlpha(0.08f);
        else if (shouldDrawButtonAsHighlighted)
            fillColour = fillColour.withAlpha(0.12f);

        g.setColour(fillColour);
        g.fillRoundedRectangle(bounds, cornerRadius);

        // Draw border
        g.setColour(juce::Colour(Theme::kBorder));
        g.drawRoundedRectangle(bounds, cornerRadius, 1.0f);
    }
}

void SyntheticObsidianLookAndFeel::drawButtonText(
    juce::Graphics& g, juce::TextButton& button,
    bool isHighlighted, bool isDown)
{
    g.setFont(bodyFont_);

    // Text color depends on button state
    const bool isToggled = button.getToggleState();
    const bool isPrimary = button.getName().containsIgnoreCase("primary");

    if (isPrimary || isToggled)
        g.setColour(juce::Colour(Theme::kBackground));
    else
        g.setColour(juce::Colour(Theme::kTextPrimary));

    const int yIndent = juce::roundToInt(button.proportionOfHeight(0.2f));
    const int cornerSize = juce::roundToInt(juce::jmin(button.getHeight(), button.getWidth()) / 2);

    g.drawFittedText(button.getButtonText(),
                     yIndent, yIndent,
                     button.getWidth() - yIndent * 2,
                     button.getHeight() - yIndent * 2,
                     juce::Justification::centred, 1);
}

void SyntheticObsidianLookAndFeel::drawRotarySlider(
    juce::Graphics& g, int x, int y, int width, int height,
    float sliderPosProportional, float rotaryStartAngle,
    float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>(x, y, width, height);
    auto centre = bounds.getCentre();
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f - 4.0f;

    // Draw background arc (track)
    g.setColour(juce::Colour(Theme::kSurface));
    juce::Path arcPath;
    arcPath.addCentredArc(centre.x, centre.y, radius, radius, 0.0f,
                          rotaryStartAngle, rotaryEndAngle, true);
    g.strokePath(arcPath, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Draw filled arc (progress)
    g.setColour(juce::Colour(Theme::kAccentCyan));
    juce::Path filledArcPath;
    const float currentAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    filledArcPath.addCentredArc(centre.x, centre.y, radius, radius, 0.0f,
                                rotaryStartAngle, currentAngle, true);
    g.strokePath(filledArcPath, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Draw thumb dot
    auto thumbAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    auto thumbX = centre.x + radius * std::cos(thumbAngle - juce::MathConstants<float>::halfPi);
    auto thumbY = centre.y + radius * std::sin(thumbAngle - juce::MathConstants<float>::halfPi);

    g.setColour(juce::Colour(Theme::kAccentCyan));
    g.fillEllipse(thumbX - 5.0f, thumbY - 5.0f, 10.0f, 10.0f);

    // Draw highlight ring around thumb
    g.setColour(juce::Colour(Theme::kAccentCyan).withAlpha(0.3f));
    g.drawEllipse(thumbX - 8.0f, thumbY - 8.0f, 16.0f, 16.0f, 1.0f);
}

void SyntheticObsidianLookAndFeel::drawLinearSlider(
    juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, float minSliderPos, float maxSliderPos,
    juce::Slider::SliderStyle style, juce::Slider& slider)
{
    // Only handle horizontal sliders for now
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
    {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height,
                                                sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const bool isVertical = (style == juce::Slider::LinearVertical);
    const float trackHeight = 4.0f;
    const float thumbSize = 16.0f;

    juce::Rectangle<float> bounds(x, y, width, height);
    juce::Rectangle<float> trackBounds;
    juce::Rectangle<float> filledBounds;

    if (isVertical)
    {
        trackBounds = bounds.withSizeKeepingCentre(trackHeight, bounds.getHeight());
        filledBounds = trackBounds.withHeight(sliderPos - y);
    }
    else
    {
        trackBounds = bounds.withSizeKeepingCentre(bounds.getWidth(), trackHeight);
        filledBounds = trackBounds.withWidth(sliderPos - x);
    }

    // Draw track background
    g.setColour(juce::Colour(Theme::kSurface));
    g.fillRoundedRectangle(trackBounds, Theme::kRadius);

    // Draw filled portion with gradient
    juce::ColourGradient gradient;
    if (isVertical)
    {
        gradient = juce::ColourGradient(juce::Colour(Theme::kAccentCyan),
                                        filledBounds.getTopLeft(),
                                        juce::Colour(Theme::kAccentPurple),
                                        filledBounds.getBottomLeft(), false);
    }
    else
    {
        gradient = juce::ColourGradient(juce::Colour(Theme::kAccentPurple),
                                        filledBounds.getTopLeft(),
                                        juce::Colour(Theme::kAccentCyan),
                                        filledBounds.getTopRight(), false);
    }
    g.setGradientFill(gradient);
    g.fillRoundedRectangle(filledBounds, Theme::kRadius);

    // Draw thumb
    juce::Rectangle<float> thumbBounds;
    if (isVertical)
        thumbBounds = juce::Rectangle<float>(trackBounds.getCentreX() - thumbSize / 2.0f,
                                              sliderPos - thumbSize / 2.0f,
                                              thumbSize, thumbSize);
    else
        thumbBounds = juce::Rectangle<float>(sliderPos - thumbSize / 2.0f,
                                              trackBounds.getCentreY() - thumbSize / 2.0f,
                                              thumbSize, thumbSize);

    g.setColour(juce::Colour(Theme::kTextPrimary));
    g.fillRoundedRectangle(thumbBounds, Theme::kRadius);

    // Draw thumb highlight
    g.setColour(juce::Colour(Theme::kAccentCyan).withAlpha(0.4f));
    g.drawRoundedRectangle(thumbBounds, Theme::kRadius, 1.0f);
}

void SyntheticObsidianLookAndFeel::drawToggleButton(
    juce::Graphics& g, juce::ToggleButton& button,
    bool shouldDrawButtonAsHighlighted,
    bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat();
    const float size = 24.0f;
    const float padding = (bounds.getHeight() - size) / 2.0f;
    auto boxBounds = juce::Rectangle<float>(bounds.getX() + padding, bounds.getY() + padding, size, size);

    const bool isToggled = button.getToggleState();
    const bool isMute = button.getName().containsIgnoreCase("mute");

    // Determine fill color
    juce::Colour fillColour;
    if (!isToggled)
    {
        fillColour = juce::Colour(Theme::kGhostBg);
    }
    else
    {
        fillColour = isMute ? juce::Colour(Theme::kWarning) : juce::Colour(Theme::kAccentCyan);
    }

    // Draw box background
    g.setColour(fillColour);
    g.fillRoundedRectangle(boxBounds, Theme::kRadius);

    // Draw border
    g.setColour(juce::Colour(Theme::kBorder));
    g.drawRoundedRectangle(boxBounds, Theme::kRadius, 1.0f);

    // Draw text label
    g.setFont(bodyFont_);
    g.setColour(isToggled ? juce::Colour(Theme::kBackground) : juce::Colour(Theme::kTextSecondary));

    const auto textBounds = bounds.withLeft(boxBounds.getRight() + Theme::kSpaceS);
    g.drawFittedText(button.getButtonText(),
                     juce::Rectangle<int>(textBounds.toNearestInt()),
                     juce::Justification::centredLeft, 1);
}

juce::Font SyntheticObsidianLookAndFeel::getLabelFont(juce::Label& label)
{
    return bodyFont_;
}
