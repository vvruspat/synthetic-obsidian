#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace obsidian::editor
{
// Shared by all time surfaces. Beats are quarter notes; x is surface-local.
struct TimeViewport
{
    struct TimeSignature { double beat = 0.0; int numerator = 4, denominator = 4; };
    struct Position { std::int64_t bar = 1; int beat = 1, ticks = 0, numerator = 4, denominator = 4; };
    // Sorted, validated changes supplied by the owner. Empty means 4/4.
    std::vector<TimeSignature> signatures;
    Position positionAt(double beat) const noexcept
    {
        double start = 0.0;
        int numerator = 4, denominator = 4;
        std::int64_t bar = 1;
        for (const auto& change : signatures)
        {
            if (change.beat > beat + 0.000001) break;
            if (change.beat > start)
                bar += static_cast<std::int64_t>(std::ceil((change.beat - start) / (numerator * 4.0 / denominator) - 0.000001));
            start = change.beat; numerator = change.numerator; denominator = change.denominator;
        }
        const auto quartersPerBeat = 4.0 / denominator;
        const auto quartersPerBar = numerator * quartersPerBeat;
        const auto elapsed = std::max(0.0, beat - start);
        const auto bars = static_cast<std::int64_t>(std::floor(elapsed / quartersPerBar + 0.000001));
        const auto inBar = std::max(0.0, elapsed - static_cast<double>(bars) * quartersPerBar);
        return { bar + bars, static_cast<int>(inBar / quartersPerBeat + 0.000001) + 1,
                 static_cast<int>(std::fmod(inBar, quartersPerBeat) * 960.0), numerator, denominator };
    }
    template <typename Callback>
    void forEachGridLine(double first, double last, double subdivision, Callback callback) const
    {
        auto start = 0.0;
        int numerator = 4, denominator = 4;
        const auto region = [&](double end)
        {
            if (end < first || start > last) return;
            const auto step = std::min(subdivision, 4.0 / denominator);
            const auto begin = std::max(0.0, std::floor((first - start) / step));
            for (auto index = begin; start + index * step <= last + 0.0000001 && start + index * step < end - 0.0000001; ++index)
            {
                const auto beat = start + index * step;
                const auto bar = std::abs(std::remainder(beat - start, numerator * 4.0 / denominator)) < 0.00001;
                callback(beat, bar);
            }
        };
        for (const auto& change : signatures)
        {
            region(change.beat);
            start = change.beat; numerator = change.numerator; denominator = change.denominator;
        }
        region(last + subdivision + 0.000001);
    }
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
