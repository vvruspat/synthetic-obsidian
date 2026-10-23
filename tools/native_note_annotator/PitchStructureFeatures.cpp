#include "PitchStructureFeatures.h"
#include "NoteAnalysis.h"

#include <algorithm>
#include <cmath>

namespace native_annotation
{
namespace
{
using Prefixes = std::array<std::vector<double>, 6>;

double sum(const std::vector<double>& prefix, int index, int left, int right)
{
    const auto count = static_cast<int>(prefix.size()) - 1;
    return prefix[static_cast<size_t>(std::clamp(index + right, 0, count))]
         - prefix[static_cast<size_t>(std::clamp(index + left, 0, count))];
}

struct Stats
{
    double count, mean, slope, residual;
};

Stats stats(const Prefixes& prefix, int index, int left, int right)
{
    const auto n = sum(prefix[0], index, left, right);
    const auto total = sum(prefix[1], index, left, right);
    const auto square = sum(prefix[2], index, left, right);
    const auto time = sum(prefix[3], index, left, right);
    const auto time2 = sum(prefix[4], index, left, right);
    const auto cross = sum(prefix[5], index, left, right);
    const auto divisor = std::max(n, 1.0);
    const auto mean = total / divisor;
    const auto variance = std::max(0.0, square / divisor - mean * mean);
    const auto timeVariance = std::max(0.0, time2 / divisor - (time / divisor) * (time / divisor));
    const auto covariance = cross / divisor - time / divisor * mean;
    const auto slope = timeVariance > 1.0e-6 ? covariance / timeVariance : 0.0;
    return { n, mean, slope, std::sqrt(std::max(0.0, variance - slope * covariance)) };
}

float delta(double value) { return static_cast<float>(std::clamp(value / 3.0, -1.0, 1.0)); }
}

std::vector<std::array<float, kPitchStructureFeatureCount>> makePitchStructureFeatures(
    const NoteAnalysis& analysis, int frames, const std::atomic<bool>& cancelled)
{
    if (frames <= 0 || static_cast<int>(analysis.frames.size()) > frames
        || frames - static_cast<int>(analysis.frames.size()) > 1 || cancelled.load()) return {};
    std::vector<std::array<float, kPitchStructureFeatureCount>> output(static_cast<size_t>(frames));
    Prefixes pitchPrefix, pairPrefix;
    for (auto* prefix : { &pitchPrefix, &pairPrefix })
        for (auto& values : *prefix) values.resize(static_cast<size_t>(frames + 1), 0.0);
    std::vector<double> derivative(static_cast<size_t>(frames), 0.0), valid(derivative.size(), 0.0);
    std::vector<double> validPrefix(static_cast<size_t>(frames + 1), 0.0);
    for (int i = 0; i < frames; ++i)
    {
        const auto present = i < static_cast<int>(analysis.frames.size());
        const auto voiced = present && analysis.frames[static_cast<size_t>(i)].voiced;
        const auto pitch = present ? analysis.frames[static_cast<size_t>(i)].midi : 0.0;
        const auto v = voiced ? 1.0 : 0.0;
        const auto time = static_cast<double>(i);
        const std::array<double, 6> values { v, pitch*v, pitch*pitch*v, time*v, time*time*v, time*pitch*v };
        for (size_t k = 0; k < values.size(); ++k)
            pitchPrefix[k][static_cast<size_t>(i+1)] = pitchPrefix[k][static_cast<size_t>(i)] + values[k];
        if (i > 0 && voiced && analysis.frames[static_cast<size_t>(i-1)].voiced)
        {
            const auto difference = pitch - analysis.frames[static_cast<size_t>(i-1)].midi;
            if (std::abs(difference) <= 1.5)
            {
                valid[static_cast<size_t>(i)] = 1.0;
                derivative[static_cast<size_t>(i)] = std::clamp(difference, -.5, .5);
            }
        }
        validPrefix[static_cast<size_t>(i+1)] = validPrefix[static_cast<size_t>(i)] + valid[static_cast<size_t>(i)];
    }
    for (int i = 0; i < frames; ++i)
    {
        if ((i % 256) == 0 && cancelled.load()) return {};
        const auto past = stats(pitchPrefix, i, -23, -3), future = stats(pitchPrefix, i, 3, 23);
        const auto early = stats(pitchPrefix, i, -43, -23), late = stats(pitchPrefix, i, 23, 43);
        const auto widePast = stats(pitchPrefix, i, -43, -3), wideFuture = stats(pitchPrefix, i, 3, 43);
        auto& row = output[static_cast<size_t>(i)];
        if (past.count >= 15.0 && future.count >= 15.0)
        {
            row[0] = delta(future.mean - past.mean);
            row[2] = static_cast<float>(std::sqrt(past.count * future.count) / 20.0);
        }
        if (widePast.count >= 30.0 && wideFuture.count >= 30.0)
        {
            row[1] = delta(wideFuture.mean - widePast.mean);
            row[3] = static_cast<float>(std::sqrt(widePast.count * wideFuture.count) / 40.0);
        }
        const std::array<Stats, 2> sides { past, future };
        for (size_t side = 0; side < sides.size(); ++side)
            if (sides[side].count >= 15.0)
            {
                row[4+side] = delta(sides[side].slope * 20.0);
                const auto residual = sides[side].residual / .5;
                row[6+side] = static_cast<float>(1.0 / (1.0 + residual * residual));
            }
        if (past.count >= 15.0 && early.count >= 15.0) row[8] = delta(past.mean - early.mean);
        if (future.count >= 15.0 && late.count >= 15.0) row[9] = delta(late.mean - future.mean);
        const auto local = stats(pitchPrefix, i, -40, 41);
        if (local.count >= 53.0) row[12] = static_cast<float>(std::clamp(local.residual / 1.5, 0.0, 1.0));
        row[13] = static_cast<float>(sum(validPrefix, i, -40, 41) / 81.0);
    }

    // ACF of pitch differences removes constant drift. The first supported peak
    // describes modulation without imposing a hard vibrato/no-note frequency rule.
    std::vector<double> previous(static_cast<size_t>(frames), 0.0), previous2(previous.size(), -1.0);
    for (int lag = 8; lag <= 40; ++lag)
    {
        if (cancelled.load()) return {};
        for (int j = 0; j < frames; ++j)
        {
            const auto support = j >= lag ? valid[static_cast<size_t>(j)] * valid[static_cast<size_t>(j-lag)] : 0.0;
            const auto a = derivative[static_cast<size_t>(j)] * support;
            const auto b = j >= lag ? derivative[static_cast<size_t>(j-lag)] * support : 0.0;
            const std::array<double, 6> values { support, a, b, a*a, b*b, a*b };
            for (size_t k = 0; k < values.size(); ++k)
                pairPrefix[k][static_cast<size_t>(j+1)] = pairPrefix[k][static_cast<size_t>(j)] + values[k];
        }
        const auto left = -40 + lag / 2, right = 41 + lag / 2;
        for (int i = 0; i < frames; ++i)
        {
            const auto n = sum(pairPrefix[0], i, left, right), divisor = std::max(n, 1.0);
            const auto sa = sum(pairPrefix[1], i, left, right), sb = sum(pairPrefix[2], i, left, right);
            const auto va = std::max(0.0, sum(pairPrefix[3], i, left, right) - sa*sa/divisor);
            const auto vb = std::max(0.0, sum(pairPrefix[4], i, left, right) - sb*sb/divisor);
            const auto correlation = n >= 53.0 && va > 1.0e-6 && vb > 1.0e-6
                ? std::clamp((sum(pairPrefix[5], i, left, right) - sa*sb/divisor) / std::sqrt(std::max(va*vb, 1.0e-20)), -1.0, 1.0) : 0.0;
            const auto index = static_cast<size_t>(i);
            auto& row = output[index];
            if (lag > 8 && row[11] == 0.0f && previous[index] > .3
                && previous[index] >= previous2[index] && previous[index] > correlation)
            {
                row[10] = static_cast<float>(previous[index]);
                row[11] = 8.0f / static_cast<float>(lag - 1);
            }
            previous2[index] = previous[index];
            previous[index] = correlation;
        }
    }
    for (int i = 0; i < frames; ++i)
    {
        const auto index = static_cast<size_t>(i);
        auto& row = output[index];
        if (row[11] == 0.0f && previous[index] > .3 && previous[index] > previous2[index])
        {
            row[10] = static_cast<float>(previous[index]);
            row[11] = 8.0f / 40.0f;
        }
    }
    return output;
}
}
