#pragma once

#include <algorithm>
#include <cmath>

namespace obsidian::editor
{
// Shared by all time surfaces. Beats are quarter notes; x is surface-local.
struct TimeViewport
{
    double firstBeat = 192.0;
    double pixelsPerBeat = 15.0;

    float xForBeat(double beat) const noexcept
    {
        return static_cast<float>((beat - firstBeat) * pixelsPerBeat);
    }
    double beatAtX(float x) const noexcept { return firstBeat + x / pixelsPerBeat; }
    void scroll(double beats) noexcept { firstBeat = std::max(0.0, firstBeat + beats); }
    void zoomAt(double factor, float anchorX) noexcept
    {
        if (!std::isfinite(factor) || factor <= 0.0) return;
        const auto anchorBeat = beatAtX(anchorX);
        pixelsPerBeat = std::clamp(pixelsPerBeat * factor, 5.0, 160.0);
        firstBeat = std::max(0.0, anchorBeat - anchorX / pixelsPerBeat);
    }
};
}
