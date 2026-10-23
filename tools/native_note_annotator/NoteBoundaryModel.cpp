#include "NoteBoundaryModel.h"
#include "PitchStructureFeatures.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace native_annotation
{
namespace
{
constexpr int kSampleRate = 16000;
constexpr int kHop = 160;
constexpr int kFft = 512;
constexpr int kBins = kFft / 2 + 1;
constexpr int kChannels = 64;
constexpr int kLongFft = 2048;
constexpr int kMultiscaleFeatures = 131;
constexpr int kVoicingFeatures = 138;
constexpr int kStructuredFeatures = kVoicingFeatures + kPitchStructureFeatureCount;

// Relative inputs only: no MIDI class or absolute pitch is supervised.
std::array<float, 7> voicingFeatures(const NoteAnalysis& analysis, int index)
{
    struct Stats { double count = 0.0, sum = 0.0, square = 0.0; };
    const auto stats = [&](int left, int right)
    {
        Stats value;
        for (int i = juce::jmax(0, index + left); i < juce::jmin(static_cast<int>(analysis.frames.size()), index + right); ++i)
        {
            const auto& frame = analysis.frames[static_cast<size_t>(i)];
            if (frame.voiced)
            {
                value.count += 1.0;
                value.sum += frame.midi;
                value.square += frame.midi * frame.midi;
            }
        }
        return value;
    };
    const auto local = stats(-10, 11);
    const auto wide = stats(-40, 41);
    const auto past = stats(-13, -3);
    const auto future = stats(3, 13);
    const bool supported = past.count >= 5.0 && future.count >= 5.0;
    const bool present = index < static_cast<int>(analysis.frames.size());
    const auto voiced = present && analysis.frames[static_cast<size_t>(index)].voiced;
    const auto confidence = present ? analysis.frames[static_cast<size_t>(index)].confidence : 0.0;
    const auto pitch = present ? analysis.frames[static_cast<size_t>(index)].midi : 0.0;
    const auto relative = voiced ? juce::jlimit(-1.0, 1.0, (pitch - wide.sum / juce::jmax(1.0, wide.count)) / 6.0) : 0.0;
    const auto delta = supported ? juce::jlimit(-1.0, 1.0, (future.sum / future.count - past.sum / past.count) / 6.0) : 0.0;
    const auto mean = local.sum / juce::jmax(1.0, local.count);
    const auto spread = local.count >= 2.0 ? juce::jlimit(0.0, 1.0, std::sqrt(juce::jmax(0.0, local.square / local.count - mean * mean)) / 3.0) : 0.0;
    return { static_cast<float>(confidence), voiced ? 1.0f : 0.0f, static_cast<float>(local.count / 21.0),
             static_cast<float>(relative), static_cast<float>(delta),
             supported ? static_cast<float>(std::sqrt(past.count * future.count) / 10.0) : 0.0f,
             static_cast<float>(spread) };
}

bool integerEquals(const juce::var& value, int expected)
{
    if (! value.isInt() && ! value.isInt64() && ! value.isDouble()) return false;
    const auto number = static_cast<double>(value);
    return std::isfinite(number) && number >= expected && number <= expected;
}

bool readFloats(const juce::var& value, size_t count, std::vector<float>& output)
{
    const auto* array = value.getArray();
    if (array == nullptr || static_cast<size_t>(array->size()) != count) return false;
    output.clear();
    output.reserve(count);
    for (const auto& item : *array)
    {
        if (! item.isDouble() && ! item.isInt() && ! item.isInt64()) return false;
        const auto number = static_cast<double>(item);
        if (! std::isfinite(number) || std::abs(number) > 1.0e6) return false;
        output.push_back(static_cast<float>(number));
    }
    return true;
}

float dot(const float* first, const float* second, int count) noexcept
{
    // Four accumulators permit vectorization without assuming allocator SIMD alignment.
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    int i = 0;
    for (; i + 3 < count; i += 4)
    {
        a += first[i] * second[i];
        b += first[i + 1] * second[i + 1];
        c += first[i + 2] * second[i + 2];
        d += first[i + 3] * second[i + 3];
    }
    auto result = (a + b) + (c + d);
    for (; i < count; ++i) result += first[i] * second[i];
    return result;
}

std::vector<int> peaks(const std::vector<std::array<float, 3>>& values, int head, float threshold)
{
    std::vector<int> candidates, selected;
    for (int i = 0; i < static_cast<int>(values.size()); ++i)
    {
        const auto value = values[static_cast<size_t>(i)][static_cast<size_t>(head)];
        const auto left = i > 0 ? values[static_cast<size_t>(i - 1)][static_cast<size_t>(head)] : 0.0f;
        const auto right = i + 1 < static_cast<int>(values.size()) ? values[static_cast<size_t>(i + 1)][static_cast<size_t>(head)] : 0.0f;
        if (value >= threshold && value >= left && value > right) candidates.push_back(i);
    }
    std::sort(candidates.begin(), candidates.end(), [&](int a, int b)
    {
        const auto first = values[static_cast<size_t>(a)][static_cast<size_t>(head)];
        const auto second = values[static_cast<size_t>(b)][static_cast<size_t>(head)];
        if (first > second) return true;
        if (first < second) return false;
        return a < b;
    });
    for (const auto index : candidates)
        if (std::none_of(selected.begin(), selected.end(), [index](int other) { return std::abs(index - other) < 8; }))
            selected.push_back(index);
    std::sort(selected.begin(), selected.end());
    return selected;
}
}

juce::Result NoteBoundaryModel::load(const juce::File& file)
{
    id_.clear();
    if (! file.existsAsFile() || file.getSize() > 4 * 1024 * 1024)
        return juce::Result::fail("Note model is missing or exceeds 4 MiB.");
    const auto root = juce::JSON::parse(file);
    const auto schema = root["schema"].toString();
    const bool structured = schema == "synthetic-obsidian-note-boundary-tcn-v4";
    const bool voicing = structured || schema == "synthetic-obsidian-note-boundary-tcn-v3";
    const bool multiscale = voicing || schema == "synthetic-obsidian-note-boundary-tcn-v2";
    const auto contract = structured ? "strongest-linear-multiscale-yin-structure-v4"
                        : voicing ? "strongest-linear-multiscale-yin-relative-v3"
                        : multiscale ? "strongest-linear-multiscale-noise-clipnorm-v2"
                                     : "strongest-linear-hann-slaney-clipnorm-v1";
    if ((! multiscale && schema != "synthetic-obsidian-note-boundary-tcn-v1")
        || root["feature_contract"].toString() != contract
        || ! integerEquals(root["sample_rate"], kSampleRate) || ! integerEquals(root["hop_length"], kHop)
        || ! integerEquals(root["n_fft"], kFft) || ! integerEquals(root["n_mels"], kChannels)
        || ! integerEquals(root["channels"], kChannels))
        return juce::Result::fail("Unsupported native note model or feature contract.");
    featureCount_ = structured ? kStructuredFeatures : voicing ? kVoicingFeatures : multiscale ? kMultiscaleFeatures : kChannels;
    dilations_ = multiscale ? std::vector<int> { 1, 2, 4, 8, 16, 16 } : std::vector<int> { 1, 2, 4, 8 };
    layers_.resize(2 + 2 * dilations_.size());
    if (multiscale)
    {
        const auto extra = root["extra_features"];
        constexpr std::array<const char*, 24> names { "spectral_flatness", "high_frequency_ratio", "log_energy",
            "yin_confidence", "yin_voiced", "voiced_context_210ms", "relative_pitch_810ms",
            "pitch_delta_100ms", "pitch_delta_support", "pitch_spread_210ms",
            "pitch_center_delta_200ms", "pitch_center_delta_400ms", "pitch_center_support_200ms", "pitch_center_support_400ms",
            "pitch_trend_past_200ms", "pitch_trend_future_200ms", "pitch_stability_past_200ms", "pitch_stability_future_200ms",
            "pitch_persistence_past_200ms", "pitch_persistence_future_200ms", "pitch_modulation_regularity_810ms",
            "pitch_modulation_frequency_810ms", "pitch_modulation_extent_810ms", "pitch_modulation_support_810ms" };
        const auto extras = structured ? static_cast<int>(names.size()) : voicing ? 10 : 3;
        if (! integerEquals(root["input_features"], featureCount_) || ! integerEquals(root["long_n_fft"], kLongFft)
            || extra.size() != extras
            || (voicing && root["yin_contract"].toString() != "native-yin-8k-40ms-hop10ms-v1")
            || (structured && root["pitch_structure_contract"].toString() != "native-yin-centres-difference-acf-810ms-v1")
            || ! readFloats(root["long_mel_filters"], static_cast<size_t>(kChannels * (kLongFft / 2 + 1)), longFilters_)
            || std::any_of(longFilters_.begin(), longFilters_.end(), [](float value) { return value < 0.0f; }))
            return juce::Result::fail("Invalid multiscale note features.");
        for (int i = 0; i < extras; ++i)
            if (extra[i].toString() != names[static_cast<size_t>(i)])
                return juce::Result::fail("Unsupported note feature order.");
    }
    const auto dilations = root["dilations"];
    const auto heads = root["heads"];
    if (dilations.size() != static_cast<int>(dilations_.size()) || heads.size() != 3 || heads[0].toString() != "onset"
        || heads[1].toString() != "offset" || heads[2].toString() != "activity")
        return juce::Result::fail("Invalid note model outputs.");
    for (size_t i = 0; i < dilations_.size(); ++i)
        if (! integerEquals(dilations[static_cast<int>(i)], dilations_[i]))
            return juce::Result::fail("Unsupported TCN receptive field.");
    std::vector<float> thresholds;
    if (! readFloats(root["thresholds"], 3, thresholds)
        || std::any_of(thresholds.begin(), thresholds.end(), [](float value) { return value <= 0.0f || value >= 1.0f; })
        || ! readFloats(root["mel_filters"], static_cast<size_t>(kChannels * kBins), filters_)
        || std::any_of(filters_.begin(), filters_.end(), [](float value) { return value < 0.0f; }))
        return juce::Result::fail("Invalid note model feature filters or thresholds.");
    const auto weights = root["layers"];
    const auto readLayer = [&](size_t index, const juce::String& prefix, int inputs, int outputs, int kernel)
    {
        auto& layer = layers_[static_cast<size_t>(index)];
        return readFloats(weights[juce::Identifier(prefix + ".weight")], static_cast<size_t>(outputs * kernel * inputs), layer.weights)
            && readFloats(weights[juce::Identifier(prefix + ".bias")], static_cast<size_t>(outputs), layer.bias);
    };
    if (! readLayer(0, "input_projection.0", featureCount_, kChannels, 1)
        || ! readLayer(layers_.size() - 1, "output_heads", kChannels, 3, 1))
        return juce::Result::fail("Invalid note model input/output weights.");
    for (size_t block = 0; block < dilations_.size(); ++block)
    {
        const auto prefix = "temporal_blocks." + juce::String(static_cast<int>(block));
        if (! readLayer(1 + block * 2, prefix + ".temporal", kChannels, kChannels, 3)
            || ! readLayer(2 + block * 2, prefix + ".mix", kChannels, kChannels, 1))
            return juce::Result::fail("Invalid note model residual weights.");
    }
    const auto identity = root["model_id"].toString();
    if (identity.isEmpty() || identity.length() > 160) return juce::Result::fail("Invalid model identity.");
    id_ = identity;
    name_ = root["name"].toString();
    std::copy(thresholds.begin(), thresholds.end(), thresholds_.begin());
    return juce::Result::ok();
}

std::vector<float> NoteBoundaryModel::makeFeatures(const juce::AudioBuffer<float>& audio, double sampleRate,
                                                  const std::atomic<bool>& cancelled, const NoteAnalysis* analysis) const
{
    if (audio.getNumSamples() == 0 || audio.getNumChannels() == 0 || ! std::isfinite(sampleRate) || sampleRate < 8000.0)
        return {};
    int channel = 0;
    double strongest = -1.0;
    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        double energy = 0.0;
        const auto* data = audio.getReadPointer(c);
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            if (! std::isfinite(data[i])) return {};
            energy += static_cast<double>(data[i]) * data[i];
        }
        if (energy > strongest) { strongest = energy; channel = c; }
    }
    const auto resampledSize = std::ceil(audio.getNumSamples() * static_cast<double>(kSampleRate) / sampleRate);
    if (resampledSize < 1.0 || resampledSize > std::numeric_limits<int>::max() / featureCount_) return {};
    const auto count = static_cast<int>(resampledSize);
    std::vector<float> mono(static_cast<size_t>(count));
    const auto* source = audio.getReadPointer(channel);
    for (int i = 0; i < count; ++i)
    {
        const auto position = i * sampleRate / kSampleRate;
        const auto index = juce::jmin(audio.getNumSamples() - 1, static_cast<int>(position));
        const auto next = juce::jmin(index + 1, audio.getNumSamples() - 1);
        mono[static_cast<size_t>(i)] = static_cast<float>(source[index] + (static_cast<double>(source[next]) - source[index]) * (position - index));
    }
    const auto frames = count / kHop + 1;
    std::vector<float> result(static_cast<size_t>(frames * featureCount_));
    const auto extract = [&](int size, int order, const std::vector<float>& filters, int destination)
    {
        const auto bins = size / 2 + 1;
        std::vector<float> window(static_cast<size_t>(size));
        for (int i = 0; i < size; ++i)
            window[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * i / size));
        juce::dsp::FFT fft(order);
        std::vector<float> work(static_cast<size_t>(size * 2));
        std::vector<double> power(static_cast<size_t>(bins)), levels(static_cast<size_t>(frames));
        auto maximum = -std::numeric_limits<float>::infinity();
        for (int frame = 0; frame < frames; ++frame)
        {
            if (cancelled.load(std::memory_order_relaxed)) return false;
            std::fill(work.begin(), work.end(), 0.0f);
            for (int i = 0; i < size; ++i)
            {
                const auto index = frame * kHop - size / 2 + i;
                if (index >= 0 && index < count) work[static_cast<size_t>(i)] = mono[static_cast<size_t>(index)] * window[static_cast<size_t>(i)];
            }
            fft.performRealOnlyForwardTransform(work.data(), true);
            double energy = 0.0, logSum = 0.0, high = 0.0;
            for (int bin = 0; bin < bins; ++bin)
            {
                const double real = work[static_cast<size_t>(bin * 2)];
                const double imaginary = work[static_cast<size_t>(bin * 2 + 1)];
                const auto value = real * real + imaginary * imaginary;
                power[static_cast<size_t>(bin)] = value;
                energy += value;
                logSum += std::log(juce::jmax(1.0e-10, value));
                if (bin >= size / 4) high += value;
            }
            for (int mel = 0; mel < kChannels; ++mel)
            {
                double melEnergy = 0.0;
                for (int bin = 0; bin < bins; ++bin)
                    melEnergy += power[static_cast<size_t>(bin)] * filters[static_cast<size_t>(mel * bins + bin)];
                const auto value = static_cast<float>(10.0 * std::log10(juce::jmax(1.0e-10, melEnergy)));
                result[static_cast<size_t>(frame * featureCount_ + destination + mel)] = value;
                maximum = juce::jmax(maximum, value);
            }
            if (destination > 0)
            {
                result[static_cast<size_t>(frame * featureCount_ + 128)] = static_cast<float>(std::exp(logSum / bins) / juce::jmax(1.0e-10, energy / bins));
                result[static_cast<size_t>(frame * featureCount_ + 129)] = static_cast<float>(high / juce::jmax(1.0e-10, energy));
                levels[static_cast<size_t>(frame)] = std::log10(juce::jmax(1.0e-10, energy));
            }
        }
        for (int mel = 0; mel < kChannels; ++mel)
        {
            double mean = 0.0;
            for (int f = 0; f < frames; ++f)
            {
                auto& value = result[static_cast<size_t>(f * featureCount_ + destination + mel)];
                value = juce::jmax(value, maximum - 80.0f);
                mean += value;
            }
            mean /= frames;
            double variance = 0.0;
            for (int f = 0; f < frames; ++f)
            {
                const auto delta = result[static_cast<size_t>(f * featureCount_ + destination + mel)] - mean;
                variance += delta * delta;
            }
            const auto scale = juce::jmax(1.0e-5, std::sqrt(variance / frames));
            for (int f = 0; f < frames; ++f)
            {
                auto& value = result[static_cast<size_t>(f * featureCount_ + destination + mel)];
                value = static_cast<float>((value - mean) / scale);
            }
        }
        if (destination > 0)
        {
            double mean = 0.0, variance = 0.0;
            for (auto value : levels) mean += value;
            mean /= frames;
            for (auto value : levels) variance += (value - mean) * (value - mean);
            const auto scale = juce::jmax(1.0e-5, std::sqrt(variance / frames));
            for (int f = 0; f < frames; ++f)
                result[static_cast<size_t>(f * featureCount_ + 130)] = static_cast<float>((levels[static_cast<size_t>(f)] - mean) / scale);
        }
        return true;
    };
    if (! extract(kFft, 9, filters_, 0)) return {};
    if (featureCount_ >= kMultiscaleFeatures && ! extract(kLongFft, 11, longFilters_, kChannels)) return {};
    if (featureCount_ >= kVoicingFeatures)
    {
        if (analysis == nullptr || static_cast<int>(analysis->frames.size()) > frames
            || frames - static_cast<int>(analysis->frames.size()) > 1) return {};
        for (int frame = 0; frame < frames; ++frame)
        {
            if (cancelled.load(std::memory_order_relaxed)) return {};
            const auto extra = voicingFeatures(*analysis, frame);
            std::copy(extra.begin(), extra.end(), result.begin() + frame * featureCount_ + kMultiscaleFeatures);
        }
        if (featureCount_ == kStructuredFeatures)
        {
            const auto structure = makePitchStructureFeatures(*analysis, frames, cancelled);
            if (structure.size() != static_cast<size_t>(frames)) return {};
            for (int frame = 0; frame < frames; ++frame)
                std::copy(structure[static_cast<size_t>(frame)].begin(), structure[static_cast<size_t>(frame)].end(),
                          result.begin() + frame * featureCount_ + kVoicingFeatures);
        }
    }
    return result;
}

std::vector<float> NoteBoundaryModel::convolve(const std::vector<float>& input, const Layer& layer,
                                               int inputs, int outputs, int kernel, int dilation, bool relu,
                                               const std::atomic<bool>& cancelled)
{
    const auto frames = static_cast<int>(input.size() / static_cast<size_t>(inputs));
    std::vector<float> result(static_cast<size_t>(frames * outputs));
    for (int frame = 0; frame < frames; ++frame)
    {
        if ((frame % 32) == 0 && cancelled.load(std::memory_order_relaxed)) return {};
        for (int output = 0; output < outputs; ++output)
        {
            auto value = layer.bias[static_cast<size_t>(output)];
            for (int k = 0; k < kernel; ++k)
            {
                const auto index = frame + (k - kernel / 2) * dilation;
                if (index >= 0 && index < frames)
                    value += dot(input.data() + index * inputs,
                                 layer.weights.data() + (output * kernel + k) * inputs, inputs);
            }
            if (! std::isfinite(value)) return {};
            result[static_cast<size_t>(frame * outputs + output)] = relu ? juce::jmax(0.0f, value) : value;
        }
    }
    return result;
}

NoteBoundaryModel::Prediction NoteBoundaryModel::predict(const juce::AudioBuffer<float>& audio, double sampleRate,
                                                         const std::atomic<bool>& cancelled, std::atomic<float>& progress,
                                                         const NoteAnalysis* analysis) const
{
    Prediction result;
    if (id_.isEmpty() || cancelled.load()) return result;
    NoteAnalysis ownedAnalysis;
    if (featureCount_ >= kVoicingFeatures && analysis == nullptr)
    {
        ownedAnalysis = NoteAnalysis::analyze(audio, sampleRate, cancelled, progress);
        analysis = &ownedAnalysis;
    }
    result.features = makeFeatures(audio, sampleRate, cancelled, analysis);
    if (result.features.empty()) return result;
    auto activation = convolve(result.features, layers_[0], featureCount_, kChannels, 1, 1, true, cancelled);
    if (activation.empty()) return {};
    for (size_t block = 0; block < dilations_.size(); ++block)
    {
        auto temporal = convolve(activation, layers_[1 + block * 2], kChannels, kChannels, 3, dilations_[block], true, cancelled);
        if (temporal.empty()) return {};
        auto mixed = convolve(temporal, layers_[2 + block * 2], kChannels, kChannels, 1, 1, false, cancelled);
        if (mixed.empty()) return {};
        for (size_t i = 0; i < activation.size(); ++i) activation[i] = juce::jmax(0.0f, activation[i] + mixed[i]);
        progress.store(0.3f + static_cast<float>(block + 1) * 0.6f / static_cast<float>(dilations_.size()), std::memory_order_relaxed);
    }
    const auto logits = convolve(activation, layers_.back(), kChannels, 3, 1, 1, false, cancelled);
    result.probabilities.resize(logits.size() / 3);
    for (size_t frame = 0; frame < result.probabilities.size(); ++frame)
        for (size_t head = 0; head < 3; ++head)
            result.probabilities[frame][head] = 1.0f / (1.0f + std::exp(-juce::jlimit(-60.0f, 60.0f, logits[frame * 3 + head])));
    progress.store(1.0f, std::memory_order_relaxed);
    return result;
}

std::vector<std::pair<double, double>> NoteBoundaryModel::decode(const Prediction& prediction, double duration) const
{
    const auto& values = prediction.probabilities;
    const auto onsets = peaks(values, 0, thresholds_[0]);
    const auto offsets = peaks(values, 1, thresholds_[1]);
    const auto frames = static_cast<int>(values.size());
    std::vector<bool> active(values.size(), false);
    for (size_t i = 0; i < values.size(); ++i) active[i] = values[i][2] >= thresholds_[2];
    for (int i = 0; i < frames;)
    {
        if (active[static_cast<size_t>(i)]) { ++i; continue; }
        const auto start = i;
        while (i < frames && ! active[static_cast<size_t>(i)]) ++i;
        if (i - start <= 4 && start > 0 && i < frames)
            for (int gap = start; gap < i; ++gap) active[static_cast<size_t>(gap)] = true;
    }
    std::vector<std::pair<double, double>> output;
    for (int i = 0; i < frames;)
    {
        if (! active[static_cast<size_t>(i)]) { ++i; continue; }
        const auto left = i;
        while (i < frames && active[static_cast<size_t>(i)]) ++i;
        const auto right = i;
        if (right - left < 8) continue;
        std::vector<int> starts;
        for (const auto start : onsets) if (start >= left - 12 && start < right - 3) starts.push_back(start);
        if (starts.empty()) starts.push_back(left);
        else if (starts.front() > left + 15) starts.insert(starts.begin(), left);
        for (size_t index = 0; index < starts.size(); ++index)
        {
            const auto start = juce::jmax(0, starts[index]);
            const auto limit = index + 1 < starts.size() ? starts[index + 1] : juce::jmin(frames, right + 12);
            const auto expected = juce::jmin(limit, right);
            auto end = expected;
            int distance = std::numeric_limits<int>::max();
            float confidence = -1.0f;
            for (const auto candidate : offsets)
                if (candidate >= start + 5 && candidate <= limit && std::abs(candidate - expected) <= 20)
                {
                    const auto current = values[static_cast<size_t>(candidate)][1];
                    if (std::abs(candidate - expected) < distance || (std::abs(candidate - expected) == distance && current > confidence))
                    {
                        end = candidate;
                        distance = std::abs(candidate - expected);
                        confidence = current;
                    }
                }
            auto beginTime = start * 0.01;
            const auto endTime = juce::jmin(duration, end * 0.01);
            if (! output.empty()) beginTime = juce::jmax(beginTime, output.back().second);
            if (endTime - beginTime >= 0.05) output.emplace_back(beginTime, endTime);
        }
    }
    return output;
}

void NoteBoundaryModel::apply(NoteAnalysis& analysis, const Prediction& prediction) const
{
    analysis.notes.clear();
    if (std::none_of(analysis.frames.begin(), analysis.frames.end(), [](const auto& frame) { return frame.rms > 0.0001f; }))
        return;
    for (const auto& [start, end] : decode(prediction, analysis.duration))
    {
        auto note = analysis.noteForInterval(start, end);
        note.id = "n" + juce::String(static_cast<int>(analysis.notes.size()) + 1).paddedLeft('0', 4);
        note.flags.add("model_draft");
        note.flags.add("model:" + id_);
        analysis.notes.push_back(std::move(note));
    }
}

NoteDraftResult analyzeDraft(const juce::AudioBuffer<float>& audio, double sampleRate,
                             const juce::File& modelFile, const std::atomic<bool>& cancelled,
                             std::atomic<float>& progress)
{
    NoteDraftResult result;
    result.analysis = NoteAnalysis::analyze(audio, sampleRate, cancelled, progress);
    if (modelFile == juce::File() || cancelled.load()) return result;
    NoteBoundaryModel model;
    if (const auto loaded = model.load(modelFile); loaded.failed())
    {
        result.warning = loaded.getErrorMessage() + " Using the math draft.";
        return result;
    }
    progress.store(0.0f, std::memory_order_relaxed);
    const auto prediction = model.predict(audio, sampleRate, cancelled, progress, &result.analysis);
    if (cancelled.load()) return result;
    if (prediction.probabilities.empty())
    {
        result.warning = "AI prediction failed. Using the math draft.";
        return result;
    }
    model.apply(result.analysis, prediction);
    result.modelId = model.id();
    result.modelHash = juce::SHA256(modelFile).toHexString();
    result.name = model.name();
    return result;
}
}
