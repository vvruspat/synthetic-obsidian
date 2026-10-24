#include "Theme.h"

namespace obsidian::ui
{
void drawFocus(juce::Graphics& g, const juce::Component& component)
{
    if (component.isEnabled() && component.hasKeyboardFocus(false))
    {
        g.setColour(colour(tokens::focus));
        g.drawRoundedRectangle(component.getLocalBounds().toFloat().reduced(1.5f),
                               tokens::radius, 1.0f);
    }
}

void drawIcon(juce::Graphics& g, Icon icon, juce::Rectangle<float> bounds, juce::Colour ink)
{
    if (icon == Icon::none || bounds.isEmpty()) return;
    const auto side = juce::jmin(bounds.getWidth(), bounds.getHeight());
    juce::Graphics::ScopedSaveState saved(g);
    g.addTransform(juce::AffineTransform::scale(side / 24.0f)
                       .translated(bounds.getCentreX() - side * 0.5f,
                                   bounds.getCentreY() - side * 0.5f));
    g.setColour(ink);
    juce::Path path;
    const auto line = [&path](float x1, float y1, float x2, float y2)
    {
        path.startNewSubPath(x1, y1); path.lineTo(x2, y2);
    };
    bool fill = false;
    switch (icon)
    {
        case Icon::chevronDown: path.startNewSubPath(6, 9); path.lineTo(12, 15); path.lineTo(18, 9); break;
        case Icon::chevronRight: path.startNewSubPath(9, 6); path.lineTo(15, 12); path.lineTo(9, 18); break;
        case Icon::plus: line(5, 12, 19, 12); line(12, 5, 12, 19); break;
        case Icon::minus: line(5, 12, 19, 12); break;
        case Icon::play: path.addTriangle(7, 4, 20, 12, 7, 20); fill = true; break;
        case Icon::stop: path.addRectangle(6, 6, 12, 12); fill = true; break;
        case Icon::record: path.addEllipse(5, 5, 14, 14); fill = true; break;
        case Icon::rewind: path.addRectangle(5, 5, 2, 14); path.addTriangle(18, 5, 8, 12, 18, 19); fill = true; break;
        case Icon::loop:
            path.startNewSubPath(5, 10); path.cubicTo(5, 3, 18, 3, 19, 10);
            path.startNewSubPath(19, 14); path.cubicTo(19, 21, 6, 21, 5, 14);
            line(15, 8, 19, 10); line(19, 10, 20, 5);
            line(9, 16, 5, 14); line(5, 14, 4, 19); break;
        case Icon::pointer:
            path.startNewSubPath(5, 3); path.lineTo(19, 13); path.lineTo(12, 14);
            path.lineTo(9, 21); path.closeSubPath(); break;
        case Icon::pencil:
            path.startNewSubPath(5, 15); path.lineTo(15, 4); path.lineTo(20, 9);
            path.lineTo(9, 20); path.lineTo(4, 21); path.closeSubPath(); line(13, 6, 18, 11); break;
        case Icon::scissors:
            path.addEllipse(3, 14, 6, 6); path.addEllipse(15, 14, 6, 6);
            line(7, 15, 18, 3); line(17, 15, 6, 3); break;
        case Icon::search: path.addEllipse(3, 3, 12, 12); line(14, 14, 21, 21); break;
        case Icon::more:
            for (int i = 0; i < 3; ++i) path.addEllipse(10.5f, 4.0f + static_cast<float>(i) * 6.0f, 3, 3);
            fill = true; break;
        case Icon::waveform:
            line(3, 10, 3, 14); line(7, 6, 7, 18); line(11, 2, 11, 22);
            line(15, 5, 15, 19); line(19, 8, 19, 16); line(23, 11, 23, 13); break;
        case Icon::microphone:
            path.addRoundedRectangle(9, 2, 6, 13, 3);
            path.startNewSubPath(5, 10); path.cubicTo(5, 22, 19, 22, 19, 10);
            line(12, 19, 12, 23); line(8, 23, 16, 23); break;
        case Icon::backing:
            path.addEllipse(10, 10, 4, 4);
            path.startNewSubPath(8, 7); path.cubicTo(3, 9, 3, 15, 8, 17);
            path.startNewSubPath(16, 7); path.cubicTo(21, 9, 21, 15, 16, 17);
            path.startNewSubPath(5, 4); path.cubicTo(-1, 9, -1, 15, 5, 20);
            path.startNewSubPath(19, 4); path.cubicTo(25, 9, 25, 15, 19, 20); break;
        case Icon::clock: path.addEllipse(3, 3, 18, 18); line(12, 6, 12, 12); line(12, 12, 16, 12); break;
        case Icon::signature:
            path.addEllipse(3, 16, 5, 4); path.addEllipse(15, 14, 5, 4);
            line(8, 18, 8, 5); line(20, 16, 20, 3); line(8, 5, 20, 3); line(8, 9, 20, 7); break;
        case Icon::chords:
            for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x)
                path.addRoundedRectangle(4.0f + static_cast<float>(x) * 10.0f, 4.0f + static_cast<float>(y) * 10.0f, 6, 6, 1.5f);
            break;
        case Icon::logo:
            path.addTriangle(6, 2, 0, 12, 11, 12); path.addTriangle(8, 1, 14, 11, 23, 11);
            path.addTriangle(24, 13, 18, 23, 13, 13); path.addTriangle(16, 24, 10, 14, 1, 14);
            fill = true; break;
        case Icon::close: line(5, 5, 19, 19); line(19, 5, 5, 19); break;
        case Icon::check: path.startNewSubPath(4, 12); path.lineTo(9, 17); path.lineTo(20, 6); break;
        case Icon::undo:
        case Icon::redo:
            path.startNewSubPath(4, 9); path.lineTo(14, 9); path.cubicTo(23, 9, 23, 20, 14, 20);
            line(4, 9, 9, 4); line(4, 9, 9, 14);
            if (icon == Icon::redo) path.applyTransform(juce::AffineTransform::scale(-1.0f, 1.0f).translated(24.0f, 0.0f));
            break;
        case Icon::eraser:
            path.startNewSubPath(3, 14); path.lineTo(13, 4); path.lineTo(21, 12);
            path.lineTo(12, 21); path.lineTo(10, 21); path.closeSubPath();
            line(8, 9, 16, 17); line(11, 21, 22, 21); break;
        case Icon::join:
            path.addRoundedRectangle(2, 8, 8, 8, 2); path.addRoundedRectangle(14, 8, 8, 8, 2);
            line(7, 12, 17, 12); break;
        case Icon::flex:
            line(3, 5, 3, 19); line(21, 5, 21, 19); line(5, 12, 19, 12);
            line(5, 12, 8, 9); line(5, 12, 8, 15); line(19, 12, 16, 9); line(19, 12, 16, 15); break;
        case Icon::vibrato:
            path.startNewSubPath(2, 12); path.cubicTo(5, 0, 7, 0, 10, 12);
            path.cubicTo(13, 24, 15, 24, 18, 12); path.cubicTo(20, 4, 21, 4, 23, 12); break;
        case Icon::gain:
            line(5, 3, 5, 21); line(12, 3, 12, 21); line(19, 3, 19, 21);
            line(2, 8, 8, 8); line(9, 16, 15, 16); line(16, 10, 22, 10); break;
        case Icon::notes:
            path.addRoundedRectangle(2, 14, 7, 4, 1); path.addRoundedRectangle(9, 7, 6, 4, 1);
            path.addRoundedRectangle(15, 11, 7, 4, 1); break;
        case Icon::none: break;
    }
    if (fill) g.fillPath(path);
    else g.strokePath(path, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
}
}
