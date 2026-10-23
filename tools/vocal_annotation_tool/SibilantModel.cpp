#include "SibilantModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace vocal_annotation
{
namespace
{
constexpr int kRate = 24000, kHop = 240, kFft = 1024, kBins = 513;
constexpr int kMels = 80, kFeatures = 86, kChannels = 48, kHalo = 67;
constexpr std::array kDilations { 1, 2, 4, 8, 16, 32 };

bool numbers(const juce::var& value, size_t count, std::vector<float>& result)
{
    const auto* values = value.getArray();
    if (values == nullptr || static_cast<size_t>(values->size()) != count) return false;
    result.clear(); result.reserve(count);
    for (const auto& entry : *values)
    {
        if (! entry.isDouble() && ! entry.isInt() && ! entry.isInt64()) return false;
        const auto number = static_cast<double>(entry);
        if (! std::isfinite(number) || std::abs(number) > 1.0e6) return false;
        result.push_back(static_cast<float>(number));
    }
    return true;
}

double besselI0(double value)
{
    double sum = 1.0, term = 1.0;
    for (int i = 1; i < 40; ++i)
    {
        term *= value * value / (4.0 * i * i);
        sum += term;
        if (term < sum * 1.0e-16) break;
    }
    return sum;
}

std::vector<float> resample(const float* audio, int samples, int sampleRate, const std::atomic<bool>& cancelled)
{
    if (sampleRate == kRate) return { audio, audio + samples };
    const auto divisor = std::gcd(sampleRate, kRate);
    const auto up = kRate / divisor, down = sampleRate / divisor;
    const auto maximum = std::max(up, down), half = 10 * maximum;
    std::vector<double> window(static_cast<size_t>(2 * half + 1));
    double scale = 0.0;
    for (int i = 0; i <= 2 * half; ++i)
    {
        const auto position = static_cast<double>(i - half) / maximum;
        const auto sinc = std::abs(position) < 1.0e-12 ? 1.0 : std::sin(juce::MathConstants<double>::pi * position) / (juce::MathConstants<double>::pi * position);
        const auto relative = static_cast<double>(i - half) / half;
        window[static_cast<size_t>(i)] = sinc / maximum * besselI0(5.0 * std::sqrt(std::max(0.0, 1.0 - relative * relative))) / besselI0(5.0);
        scale += window[static_cast<size_t>(i)];
    }
    std::vector<float> coefficients(window.size());
    for (size_t i = 0; i < window.size(); ++i)
        coefficients[i] = static_cast<float>(window[i] / scale) * static_cast<float>(up);
    const auto prePad = down - half % down;
    const auto remove = (half + prePad) / down;
    const auto count = (static_cast<int64_t>(samples) * up + down - 1) / down;
    std::vector<float> output(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i)
    {
        if (i % 4096 == 0 && cancelled.load(std::memory_order_relaxed)) return {};
        const auto position = (i + remove) * down - prePad;
        const auto left = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(static_cast<double>(position - 2 * half) / up)));
        const auto right = std::min<int64_t>(samples - 1, position / up);
        float sum = 0.0f;
        for (auto sample = left; sample <= right; ++sample)
            sum += audio[sample] * coefficients[static_cast<size_t>(position - sample * up)];
        output[static_cast<size_t>(i)] = sum;
    }
    return output;
}

float dot(const float* a, const float* b, int count)
{
    float x = 0, y = 0, z = 0, w = 0;
    int i = 0;
    for (; i + 3 < count; i += 4)
    {
        x += a[i] * b[i]; y += a[i + 1] * b[i + 1];
        z += a[i + 2] * b[i + 2]; w += a[i + 3] * b[i + 3];
    }
    for (; i < count; ++i) x += a[i] * b[i];
    return (x + y) + (z + w);
}
} // namespace

juce::Result SibilantModel::load(const juce::File& file)
{
    id_.clear();
    if (! file.existsAsFile() || file.getSize() > 4 * 1024 * 1024)
        return juce::Result::fail("Sibilant model is missing or exceeds 4 MiB.");
    const auto root = juce::JSON::parse(file);
    const auto integer = [&root](const char* key, int expected)
    {
        const auto value = root[key];
        return (value.isDouble() || value.isInt() || value.isInt64()) && std::abs(static_cast<double>(value) - expected) < 1.0e-9;
    };
    if (root["schema"].toString() != "synthetic-obsidian-sibilant-native-v1"
        || ! integer("sample_rate", kRate) || ! integer("hop", kHop) || ! integer("n_fft", kFft)
        || ! integer("mels", kMels) || ! integer("feature_count", kFeatures) || ! integer("channels", kChannels)
        || root["output"].toString() != "sibilant_probability" || root["dilations"].size() != 6)
        return juce::Result::fail("Unsupported sibilant model configuration.");
    for (int i = 0; i < 6; ++i)
        if (static_cast<int>(root["dilations"][i]) != kDilations[static_cast<size_t>(i)])
            return juce::Result::fail("Unsupported sibilant context.");
    if (! numbers(root["mel_filters"], kMels * kBins, filters_) || ! numbers(root["input_mean"], kFeatures, mean_)
        || ! numbers(root["input_std"], kFeatures, std_)
        || std::any_of(filters_.begin(), filters_.end(), [](float v) { return v < 0; })
        || std::any_of(std_.begin(), std_.end(), [](float v) { return v < 1.0e-6f; }))
        return juce::Result::fail("Invalid sibilant spectral features.");
    const auto weights = root["layers"];
    const auto layer = [&](size_t index, const juce::String& name, int inputs, int outputs, int kernel)
    {
        return numbers(weights[juce::Identifier(name + ".weight")], static_cast<size_t>(inputs * outputs * kernel), layers_[index].weights)
            && numbers(weights[juce::Identifier(name + ".bias")], static_cast<size_t>(outputs), layers_[index].bias);
    };
    if (! layer(0, "input", kFeatures, kChannels, 1) || ! layer(13, "output", kChannels, 1, 1))
        return juce::Result::fail("Invalid sibilant input/output layers.");
    for (size_t i = 0; i < 6; ++i)
        if (! layer(1 + i * 2, "blocks." + juce::String(static_cast<int>(i)) + ".temporal", kChannels, kChannels, 3)
            || ! layer(2 + i * 2, "blocks." + juce::String(static_cast<int>(i)) + ".mix", kChannels, kChannels, 1))
            return juce::Result::fail("Invalid sibilant residual layers.");
    const auto value = root["conservative_threshold"];
    const auto threshold = static_cast<double>(value);
    if ((! value.isDouble() && ! value.isInt()) || ! std::isfinite(threshold) || threshold <= 0 || threshold >= 1
        || root["model_id"].toString().isEmpty() || root["model_id"].toString().length() > 160)
        return juce::Result::fail("Invalid sibilant threshold or identity.");
    threshold_ = static_cast<float>(threshold);
    const auto displayValue = root["balanced_threshold"];
    const auto displayThreshold = static_cast<double>(displayValue);
    if ((! displayValue.isDouble() && ! displayValue.isInt()) || ! std::isfinite(displayThreshold)
        || displayThreshold <= 0 || displayThreshold >= 1)
        return juce::Result::fail("Invalid sibilant display threshold.");
    displayThreshold_ = static_cast<float>(displayThreshold);
    id_ = root["model_id"].toString();
    return juce::Result::ok();
}

std::vector<SibilantModel::Range> SibilantModel::detectedRanges(const std::vector<float>& probabilities,
                                                               float threshold, double duration)
{
    std::vector<Range> ranges;
    if (! std::isfinite(duration) || duration <= 0.0 || ! std::isfinite(threshold)
        || threshold <= 0.0f || threshold >= 1.0f
        || std::any_of(probabilities.begin(), probabilities.end(), [](float p)
           { return ! std::isfinite(p) || p < 0.0f || p > 1.0f; }))
        return ranges;
    size_t first = 0;
    while (first < probabilities.size())
    {
        if (probabilities[first] < threshold) { ++first; continue; }
        auto end = first + 1;
        while (end < probabilities.size() && probabilities[end] >= threshold) ++end;
        const auto startTime = static_cast<double>(first) * kHop / kRate;
        const auto endTime = std::min(duration, static_cast<double>(end) * kHop / kRate);
        if (endTime > startTime) ranges.push_back({ startTime, endTime });
        first = end;
    }
    return ranges;
}

SibilantModel::Inspection SibilantModel::inspectFile(const juce::File& audioFile, const juce::File& modelFile,
                                                    const std::atomic<bool>& cancelled)
{
    Inspection result;
    if (cancelled.load(std::memory_order_relaxed)) return result;
    SibilantModel model;
    if (const auto loaded = model.load(modelFile); loaded.failed())
    {
        result.error = loaded.getErrorMessage();
        return result;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audioFile));
    if (reader == nullptr || ! std::isfinite(reader->sampleRate)
        || reader->sampleRate < 8000.0 || reader->sampleRate > 192000.0
        || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max()
        || static_cast<juce::uint64>(reader->lengthInSamples) * reader->numChannels * sizeof(float) > 512ULL * 1024ULL * 1024ULL)
    {
        result.error = "Sibilant overlay needs mono/stereo audio at 8–192 kHz, below 512 MB decoded.";
        return result;
    }
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    for (int offset = 0; offset < audio.getNumSamples(); offset += 65536)
    {
        if (cancelled.load(std::memory_order_relaxed)) return {};
        const auto count = juce::jmin(65536, audio.getNumSamples() - offset);
        if (! reader->read(&audio, offset, count, offset, true, true))
        {
            result.error = "Cannot decode vocal for the sibilant overlay.";
            return result;
        }
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int i = offset; i < offset + count; ++i)
                if (! std::isfinite(audio.getSample(channel, i)))
                {
                    result.error = "Vocal contains non-finite samples.";
                    return result;
                }
    }
    const auto prediction = model.predict(audio, reader->sampleRate, cancelled);
    if (cancelled.load(std::memory_order_relaxed)) return {};
    if (prediction.probabilities.empty())
        result.error = "Sibilant overlay inference failed.";
    else
        result.ranges = detectedRanges(prediction.probabilities, model.displayThreshold_, audio.getNumSamples() / reader->sampleRate);
    return result;
}

std::vector<float> SibilantModel::makeFeatures(const juce::AudioBuffer<float>& audio, int start, int samples,
                                              int sampleRate, const std::atomic<bool>& cancelled) const
{
    int channel = 0;
    double strongest = -1;
    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        double energy = 0;
        for (int i = start; i < start + samples; ++i)
        {
            const auto value = audio.getSample(c, i);
            if (! std::isfinite(value)) return {};
            energy += static_cast<double>(value) * value;
        }
        if (energy > strongest) { channel = c; strongest = energy; }
    }
    const auto mono = resample(audio.getReadPointer(channel) + start, samples, sampleRate, cancelled);
    if (mono.empty()) return {};
    const auto frames = (static_cast<int>(mono.size()) + kHop - 1) / kHop;
    std::vector<float> result(static_cast<size_t>(frames * kFeatures));
    juce::dsp::FFT fft(10);
    std::array<float, 2 * kFft> data {};
    std::array<float, kFft> window {};
    for (int i = 0; i < kFft; ++i)
        window[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * juce::MathConstants<double>::pi * i / kFft));
    for (int frame = 0; frame < frames; ++frame)
    {
        if (cancelled.load(std::memory_order_relaxed)) return {};
        data.fill(0);
        bool previousSign = false;
        int zeroCrossings = 0;
        for (int i = 0; i < kFft; ++i)
        {
            const auto sample = frame * kHop + i - kFft / 2;
            const auto value = sample >= 0 && sample < static_cast<int>(mono.size()) ? mono[static_cast<size_t>(sample)] : 0.0f;
            const auto sign = std::signbit(value);
            if (i > 0 && sign != previousSign) ++zeroCrossings;
            previousSign = sign;
            data[static_cast<size_t>(i)] = value * window[static_cast<size_t>(i)];
        }
        fft.performRealOnlyForwardTransform(data.data());
        std::array<float, kBins> power {};
        double total = 0, logarithms = 0, high = 0, veryHigh = 0, centroid = 0;
        for (int bin = 0; bin < kBins; ++bin)
        {
            const auto real = data[static_cast<size_t>(2 * bin)], imaginary = data[static_cast<size_t>(2 * bin + 1)];
            const auto value = (real * real + imaginary * imaginary) / static_cast<float>(kFft * kFft);
            power[static_cast<size_t>(bin)] = value;
            const auto frequency = static_cast<double>(bin) * kRate / kFft;
            total += value; logarithms += std::log(std::max(value, 1.0e-12f)); centroid += frequency * value;
            if (frequency >= 3500) high += value;
            if (frequency >= 7000) veryHigh += value;
        }
        auto* row = result.data() + frame * kFeatures;
        for (int mel = 0; mel < kMels; ++mel)
            row[mel] = 10.0f * std::log10(std::max(1.0e-12f, dot(power.data(), filters_.data() + mel * kBins, kBins)));
        const auto energy = std::max(total, 1.0e-12);
        row[80] = static_cast<float>(10 * std::log10(energy));
        row[81] = static_cast<float>(std::exp(logarithms / kBins) / std::max(total / kBins, 1.0e-12));
        row[82] = static_cast<float>(high / energy); row[83] = static_cast<float>(veryHigh / energy);
        row[84] = static_cast<float>(centroid / energy / (kRate / 2)); row[85] = static_cast<float>(zeroCrossings) / (kFft - 1);
        for (int feature = 0; feature < kFeatures; ++feature)
            row[feature] = (row[feature] - mean_[static_cast<size_t>(feature)]) / std_[static_cast<size_t>(feature)];
    }
    return result;
}

std::vector<float> SibilantModel::convolve(const std::vector<float>& input, const Layer& layer,
                                         int inputs, int outputs, int kernel, int dilation, bool relu,
                                         const std::atomic<bool>& cancelled)
{
    const auto frames = static_cast<int>(input.size() / static_cast<size_t>(inputs));
    std::vector<float> result(static_cast<size_t>(frames * outputs));
    for (int frame = 0; frame < frames; ++frame)
    {
        if (frame % 32 == 0 && cancelled.load(std::memory_order_relaxed)) return {};
        for (int output = 0; output < outputs; ++output)
        {
            auto value = layer.bias[static_cast<size_t>(output)];
            for (int k = 0; k < kernel; ++k)
            {
                const auto index = frame + (k - kernel / 2) * dilation;
                if (index >= 0 && index < frames)
                    value += dot(input.data() + index * inputs, layer.weights.data() + (output * kernel + k) * inputs, inputs);
            }
            if (! std::isfinite(value)) return {};
            result[static_cast<size_t>(frame * outputs + output)] = relu ? std::max(value, 0.0f) : value;
        }
    }
    return result;
}

SibilantModel::Prediction SibilantModel::predict(const juce::AudioBuffer<float>& audio, double sampleRate,
                                                const std::atomic<bool>& cancelled, bool diagnostics) const
{
    Prediction result;
    if (id_.isEmpty() || cancelled.load() || ! std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 192000
        || std::abs(sampleRate - std::round(sampleRate)) > 1.0e-6 || audio.getNumSamples() == 0 || audio.getNumChannels() == 0)
        return result;
    const auto frames = static_cast<int>(std::ceil(audio.getNumSamples() * 100.0 / sampleRate));
    result.probabilities.resize(static_cast<size_t>(frames));
    if (diagnostics) result.features.resize(static_cast<size_t>(frames * kFeatures));
    for (int start = 0; start < frames; start += 6000)
    {
        const auto end = std::min(frames, start + 6000), left = std::max(0, start - kHalo), right = std::min(frames, end + kHalo);
        const auto offset = static_cast<int>(std::round(left * sampleRate / 100));
        const auto samples = std::min(audio.getNumSamples() - offset, static_cast<int>(std::round((right - left) * sampleRate / 100)));
        const auto values = makeFeatures(audio, offset, samples, static_cast<int>(std::round(sampleRate)), cancelled);
        if (values.size() < static_cast<size_t>((end - left) * kFeatures)) return {};
        if (diagnostics)
            std::copy(values.begin() + (start - left) * kFeatures, values.begin() + (end - left) * kFeatures, result.features.begin() + start * kFeatures);
        auto activation = convolve(values, layers_[0], kFeatures, kChannels, 1, 1, true, cancelled);
        if (activation.empty()) return {};
        for (size_t block = 0; block < kDilations.size(); ++block)
        {
            const auto temporal = convolve(activation, layers_[1 + block * 2], kChannels, kChannels, 3, kDilations[block], true, cancelled);
            if (temporal.empty()) return {};
            const auto mixed = convolve(temporal, layers_[2 + block * 2], kChannels, kChannels, 1, 1, false, cancelled);
            if (mixed.empty()) return {};
            for (size_t i = 0; i < activation.size(); ++i) activation[i] = std::max(0.0f, activation[i] + mixed[i]);
        }
        const auto logits = convolve(activation, layers_[13], kChannels, 1, 1, 1, false, cancelled);
        if (logits.empty()) return {};
        for (int i = start; i < end; ++i)
            result.probabilities[static_cast<size_t>(i)] = 1.0f / (1.0f + std::exp(-std::clamp(logits[static_cast<size_t>(i - left)], -60.0f, 60.0f)));
    }
    return result;
}

bool SibilantModel::hasVoicedCore(const NoteBlock& note, const std::vector<float>& probabilities, float threshold)
{
    if (! std::isfinite(note.start) || ! std::isfinite(note.end) || note.end <= note.start
        || ! std::isfinite(threshold) || threshold <= 0.0f || threshold >= 1.0f)
        return false;
    // Short notes need a shorter core. Confidence comes from the existing offline YIN analysis;
    // pitch may move freely, so vibrato and legato are not required to stay at one MIDI value.
    const auto requiredDuration = std::clamp((note.end - note.start) * 0.25, 0.03, 0.08);
    double runStart = -1.0, previous = -1.0;
    for (const auto& point : note.curve)
    {
        if (! std::isfinite(point.time) || point.time < note.start || point.time >= note.end
            || ! std::isfinite(point.midi) || ! std::isfinite(point.confidence) || point.confidence < 0.8
            || point.confidence > 1.0 || point.time < 0.0
            || point.time * 100.0 >= static_cast<double>(probabilities.size()))
        {
            runStart = previous = -1.0;
            continue;
        }
        const auto frame = static_cast<size_t>(std::floor(point.time * 100.0 + 1.0e-9));
        if (frame >= probabilities.size() || ! std::isfinite(probabilities[frame]) || probabilities[frame] >= threshold)
        {
            runStart = previous = -1.0;
            continue;
        }
        if (previous < 0.0 || point.time <= previous || point.time - previous > 0.015)
            runStart = point.time;
        previous = point.time;
        if (std::min(note.end, point.time + 0.01) - runStart >= requiredDuration - 1.0e-9)
            return true;
    }
    return false;
}

int SibilantModel::mergeToNearest(std::vector<NoteBlock>& notes, const std::vector<float>& probabilities,
                                  float threshold, double minimumCoverage, double maximumGap)
{
    if (probabilities.empty() || ! std::isfinite(threshold) || threshold <= 0 || threshold >= 1
        || ! std::isfinite(minimumCoverage) || minimumCoverage <= 0 || minimumCoverage > 1
        || std::any_of(probabilities.begin(), probabilities.end(), [](float p) { return ! std::isfinite(p) || p < 0 || p > 1; })) return 0;
    std::vector<bool> sibilant(notes.size(), false), removed(notes.size(), false);
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& note = notes[i];
        if (! std::isfinite(note.start) || ! std::isfinite(note.end) || note.start < 0 || note.end <= note.start) continue;
        if (note.end > static_cast<double>(probabilities.size()) / 100.0 + 1.0e-9) continue;
        const auto start = static_cast<size_t>(std::floor(note.start * 100));
        const auto end = static_cast<size_t>(std::ceil(note.end * 100 - 1.0e-9));
        if (end > probabilities.size() || end <= start) continue;
        double sibilantDuration = 0.0;
        for (auto frame = start; frame < end; ++frame)
            if (probabilities[frame] >= threshold)
                sibilantDuration += std::max(0.0, std::min(note.end, static_cast<double>(frame + 1) / 100.0)
                    - std::max(note.start, static_cast<double>(frame) / 100.0));
        // Weight partial edge frames by time, so manual boundaries use the same white spans.
        sibilant[i] = sibilantDuration / (note.end - note.start) > minimumCoverage + 1.0e-9
            && ! hasVoicedCore(note, probabilities, threshold);
    }
    int merged = 0;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (! sibilant[i]) continue;
        const auto source = notes[i];
        auto target = notes.size();
        double nearest = std::numeric_limits<double>::max();
        // Scan in time order: exact ties deterministically favour the previous note.
        for (size_t j = 0; j < notes.size(); ++j)
        {
            if (sibilant[j]) continue;
            const auto gap = std::max({ 0.0, source.start - notes[j].end, notes[j].start - source.end });
            if (gap <= maximumGap && gap < nearest - 1.0e-9) { nearest = gap; target = j; }
        }
        if (target == notes.size()) continue;
        auto& note = notes[target];
        note.start = std::min(note.start, source.start); note.end = std::max(note.end, source.end);
        note.flags.addIfNotAlreadyThere("sibilant_attached");
        removed[i] = true; ++merged;
    }
    std::vector<NoteBlock> kept;
    kept.reserve(notes.size() - static_cast<size_t>(merged));
    for (size_t i = 0; i < notes.size(); ++i) if (! removed[i]) kept.push_back(std::move(notes[i]));
    notes = std::move(kept);
    return merged;
}

} // namespace vocal_annotation
