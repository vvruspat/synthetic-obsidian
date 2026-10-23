#include "NoteAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace native_annotation
{
namespace
{
constexpr double kAnalysisRate = 8000.0;
constexpr double kHopSeconds = 0.01;
constexpr int kWindow = 320;
constexpr int kMinLag = 8;   // 1000 Hz.
constexpr int kMaxLag = 114; // 70 Hz.
constexpr double kMinDraftLength = 0.08;

std::pair<double, double> yin(const std::vector<float>& mono, int centre)
{
    std::array<double, kMaxLag + 1> difference {};
    const auto sample = [&mono](int index)
    {
        return index >= 0 && index < static_cast<int>(mono.size()) ? mono[static_cast<size_t>(index)] : 0.0f;
    };
    double sum = 0.0;
    difference[0] = 1.0;
    for (int lag = 1; lag <= kMaxLag; ++lag)
    {
        double value = 0.0;
        for (int j = 0; j < kWindow; ++j)
        {
            const double delta = sample(centre - kWindow / 2 + j)
                               - sample(centre - kWindow / 2 + j + lag);
            value += delta * delta;
        }
        sum += value;
        difference[static_cast<size_t>(lag)] = sum > 1.0e-12 ? value * lag / sum : 1.0;
    }

    int best = kMinLag;
    for (int lag = kMinLag; lag < kMaxLag; ++lag)
    {
        if (difference[static_cast<size_t>(lag)] < difference[static_cast<size_t>(best)])
            best = lag;
        if (difference[static_cast<size_t>(lag)] < 0.15)
        {
            while (lag < kMaxLag && difference[static_cast<size_t>(lag + 1)] < difference[static_cast<size_t>(lag)])
                ++lag;
            best = lag;
            break;
        }
    }
    const auto confidence = juce::jlimit(0.0, 1.0, 1.0 - difference[static_cast<size_t>(best)]);
    double lag = best;
    if (best > kMinLag && best < kMaxLag)
    {
        const auto left = difference[static_cast<size_t>(best - 1)];
        const auto middle = difference[static_cast<size_t>(best)];
        const auto right = difference[static_cast<size_t>(best + 1)];
        const auto denominator = left - 2.0 * middle + right;
        if (std::abs(denominator) > 1.0e-12)
            lag += juce::jlimit(-0.5, 0.5, 0.5 * (left - right) / denominator);
    }
    return { 69.0 + 12.0 * std::log2(kAnalysisRate / lag / 440.0), confidence };
}

double median(std::vector<double> values)
{
    if (values.empty())
        return 60.0;
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

vocal_annotation::NoteBlock makeNote(const std::vector<AnalysisFrame>& frames, double start, double end)
{
    vocal_annotation::NoteBlock note;
    note.start = start;
    note.end = end;
    note.voicedStart = start;
    note.voicedEnd = end;
    note.flags.add("auto_draft");
    std::vector<double> pitches;
    for (const auto& frame : frames)
        if (frame.time >= start && frame.time < end && frame.voiced)
        {
            pitches.push_back(frame.midi);
            note.curve.push_back({ frame.time, frame.midi, frame.confidence });
        }
    note.pitchExact = juce::jlimit(0.0, 127.0, median(std::move(pitches)));
    note.pitch = juce::roundToInt(note.pitchExact);
    if (note.curve.empty())
        note.flags.add("pitch_uncertain");
    else
    {
        note.voicedStart = note.curve.front().time;
        note.voicedEnd = juce::jmin(end, note.curve.back().time + kHopSeconds);
    }
    return note;
}
}

NoteAnalysis NoteAnalysis::analyze(const juce::AudioBuffer<float>& audio, double sampleRate,
                                 const std::atomic<bool>& cancelled, std::atomic<float>& progress)
{
    NoteAnalysis result;
    if (sampleRate < kAnalysisRate || audio.getNumChannels() == 0 || audio.getNumSamples() == 0)
        return result;
    result.duration = audio.getNumSamples() / sampleRate;

    int analysisChannel = 0;
    double strongestEnergy = 0.0;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    {
        double energy = 0.0;
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const auto value = audio.getSample(channel, i);
            if (std::isfinite(value)) energy += static_cast<double>(value) * value;
        }
        if (energy > strongestEnergy) { strongestEnergy = energy; analysisChannel = channel; }
    }
    // Analyze the strongest channel so inverted stereo channels cannot cancel the vocal.
    // Box-filtered downsampling keeps this draft inexpensive at 44.1/48/96 kHz.
    std::vector<float> mono(static_cast<size_t>(std::ceil(result.duration * kAnalysisRate)), 0.0f);
    for (size_t i = 0; i < mono.size(); ++i)
    {
        if (cancelled.load(std::memory_order_relaxed))
            return {};
        const auto begin = static_cast<int>(static_cast<double>(i) * sampleRate / kAnalysisRate);
        const auto end = juce::jmin(audio.getNumSamples(), static_cast<int>(static_cast<double>(i + 1) * sampleRate / kAnalysisRate));
        double value = 0.0;
        for (int s = begin; s < end; ++s)
        {
            const auto input = audio.getSample(analysisChannel, s);
            value += std::isfinite(input) ? input : 0.0f;
        }
        mono[i] = static_cast<float>(value / juce::jmax(1, end - begin));
    }
    const auto mean = std::accumulate(mono.begin(), mono.end(), 0.0) / static_cast<double>(mono.size());
    for (auto& sample : mono)
        sample -= static_cast<float>(mean);

    float peakRms = 0.0f;
    constexpr int hop = static_cast<int>(kAnalysisRate * kHopSeconds);
    for (int offset = 0; offset < static_cast<int>(mono.size()); offset += hop)
    {
        double energy = 0.0;
        const auto count = juce::jmin(hop, static_cast<int>(mono.size()) - offset);
        for (int i = 0; i < count; ++i)
            energy += mono[static_cast<size_t>(offset + i)] * mono[static_cast<size_t>(offset + i)];
        const auto rms = static_cast<float>(std::sqrt(energy / count));
        peakRms = juce::jmax(peakRms, rms);
        result.frames.push_back({ offset / kAnalysisRate, 60.0, 0.0, rms, false });
    }
    const auto threshold = juce::jmax(0.0001f, peakRms * 0.025f);
    for (size_t i = 0; i < result.frames.size(); ++i)
    {
        if (cancelled.load(std::memory_order_relaxed))
            return {};
        auto& frame = result.frames[i];
        if (frame.rms > threshold)
        {
            const auto [pitch, confidence] = yin(mono, static_cast<int>(i) * hop);
            frame.midi = pitch;
            frame.confidence = confidence;
            frame.voiced = confidence > 0.65;
        }
        progress.store(static_cast<float>(i + 1) / static_cast<float>(result.frames.size()), std::memory_order_relaxed);
    }

    // Median smoothing plus a sustained transition avoids splitting every vibrato cycle.
    std::vector<double> smoothed(result.frames.size(), 60.0);
    for (size_t i = 0; i < result.frames.size(); ++i)
    {
        std::vector<double> values;
        for (size_t j = i > 3 ? i - 3 : 0; j < juce::jmin(result.frames.size(), i + 4); ++j)
            if (result.frames[j].voiced)
                values.push_back(result.frames[j].midi);
        smoothed[i] = median(std::move(values));
    }
    int start = -1;
    int quiet = 0;
    int changedAt = -1;
    double anchor = 60.0;
    auto close = [&](int end)
    {
        auto attack = start;
        const auto previousEnd = result.notes.empty() ? 0.0 : result.notes.back().end;
        // Include the acoustic rise before stable F0, without crossing the previous note.
        while (attack > juce::jmax(0, start - 12)
               && (attack - 1) * kHopSeconds >= previousEnd
               && result.frames[static_cast<size_t>(attack - 1)].rms > threshold)
            --attack;
        const auto beginTime = attack * kHopSeconds;
        const auto endTime = juce::jmin(result.duration, end * kHopSeconds);
        if (endTime - beginTime >= kMinDraftLength)
        {
            auto note = makeNote(result.frames, beginTime, endTime);
            if (note.flags.contains("pitch_uncertain"))
                return;
            note.id = "n" + juce::String(static_cast<int>(result.notes.size()) + 1).paddedLeft('0', 4);
            result.notes.push_back(std::move(note));
        }
    };
    for (int i = 0; i < static_cast<int>(result.frames.size()); ++i)
    {
        const auto& frame = result.frames[static_cast<size_t>(i)];
        if (! frame.voiced)
        {
            changedAt = -1;
            if (start >= 0 && ++quiet >= 3)
            {
                close(i - quiet + 1);
                start = -1;
            }
            continue;
        }
        quiet = 0;
        if (start < 0)
        {
            start = i;
            anchor = smoothed[static_cast<size_t>(i)];
            continue;
        }
        const auto pitchChanged = std::abs(smoothed[static_cast<size_t>(i)] - anchor) >= 0.9;
        if (pitchChanged)
        {
            if (changedAt < 0)
                changedAt = i;
            if (i - changedAt >= 7 && (changedAt - start) * kHopSeconds >= kMinDraftLength)
            {
                close(changedAt);
                start = changedAt;
                anchor = smoothed[static_cast<size_t>(i)];
                changedAt = -1;
            }
        }
        else
        {
            changedAt = -1;
            anchor = anchor * 0.97 + smoothed[static_cast<size_t>(i)] * 0.03;
        }
        // A deep valley followed by a new attack can separate repeated pitches.
        if (i >= 8 && (i - start) * kHopSeconds > 0.14)
        {
            const auto previous = result.frames[static_cast<size_t>(i - 6)].rms;
            const auto valley = result.frames[static_cast<size_t>(i - 2)].rms;
            if (valley < previous * 0.3f && frame.rms > previous * 0.65f && previous > threshold * 3.0f)
            {
                close(i - 2);
                start = i - 2;
                anchor = smoothed[static_cast<size_t>(i)];
                changedAt = -1;
            }
        }
    }
    if (start >= 0)
        close(static_cast<int>(result.frames.size()) - quiet);
    return result;
}

vocal_annotation::NoteBlock NoteAnalysis::suggest(double start, double limit) const
{
    auto end = juce::jmin(limit, start + 0.25);
    for (const auto& note : notes)
        if (note.start <= start + 0.08 && note.end > start + 0.025)
        {
            end = juce::jmin(limit, note.end);
            break;
        }
    return makeNote(frames, start, end);
}

vocal_annotation::NoteBlock NoteAnalysis::noteForInterval(double start, double end) const
{
    return makeNote(frames, start, end);
}
}
