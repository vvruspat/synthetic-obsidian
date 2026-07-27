#include "AlgorithmicPitchPreview.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <vector>

namespace vocal_annotation
{

namespace
{

float wrapPhase(float phase) noexcept
{
    const auto pi = juce::MathConstants<float>::pi;
    const auto twoPi = juce::MathConstants<float>::twoPi;
    while (phase > pi)
        phase -= twoPi;
    while (phase < -pi)
        phase += twoPi;
    return phase;
}

} // namespace

juce::Result AlgorithmicPitchPreview::render(
    const juce::File& sourceFile,
    const juce::File& outputFile,
    const std::vector<AlgorithmicPitchCorrection>& corrections)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(sourceFile));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        return juce::Result::fail("Could not read the source audio.");
    if (reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        return juce::Result::fail("Source audio is too long for the preview.");
    }

    const auto channelCount = static_cast<int>(juce::jmin(2u, reader->numChannels));
    const auto sampleCount = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buffer(channelCount, sampleCount);
    if (! reader->read(&buffer, 0, sampleCount, 0, true, true))
        return juce::Result::fail("Could not decode the source audio.");

    applyCorrections(buffer, reader->sampleRate, corrections);

    if (outputFile.existsAsFile() && ! outputFile.deleteFile())
        return juce::Result::fail("Could not replace the previous pitch preview.");
    auto outputStream = outputFile.createOutputStream();
    if (outputStream == nullptr)
        return juce::Result::fail("Could not create the pitch preview WAV.");

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(
            outputStream.get(),
            reader->sampleRate,
            static_cast<unsigned int>(channelCount),
            24,
            {},
            0));
    if (writer == nullptr)
        return juce::Result::fail("Could not initialise the pitch preview WAV writer.");

    outputStream.release();
    if (! writer->writeFromAudioSampleBuffer(buffer, 0, sampleCount))
        return juce::Result::fail("Could not write the pitch preview WAV.");

    return juce::Result::ok();
}

void AlgorithmicPitchPreview::applyCorrections(
    juce::AudioBuffer<float>& buffer,
    double sampleRate,
    const std::vector<AlgorithmicPitchCorrection>& corrections)
{
    for (const auto& correction : corrections)
    {
        if (std::abs(correction.pitchRatio - 1.0f) < 0.0001f)
            continue;

        const auto startSample = juce::jmax(
            0,
            static_cast<int>(std::lround(correction.startSeconds * sampleRate)));
        const auto endSample = juce::jmin(
            buffer.getNumSamples(),
            static_cast<int>(std::lround(correction.endSeconds * sampleRate)));
        const auto segmentLength = endSample - startSample;
        if (segmentLength < 512)
            continue;

        const auto fadeSamples = juce::jmin(
            segmentLength / 4,
            static_cast<int>(std::lround(0.008 * sampleRate)));
        const auto paddedStart = juce::jmax(0, startSample - fadeSamples);
        const auto paddedEnd = juce::jmin(buffer.getNumSamples(), endSample + fadeSamples);
        const auto paddedLength = paddedEnd - paddedStart;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            std::vector<float> original(static_cast<size_t>(paddedLength));
            std::copy_n(
                buffer.getReadPointer(channel, paddedStart),
                paddedLength,
                original.begin());
            auto corrected = original;
            phaseVocoderMono(corrected.data(), paddedLength, correction.pitchRatio);

            auto* destination = buffer.getWritePointer(channel, paddedStart);
            for (int sample = 0; sample < paddedLength; ++sample)
            {
                auto blend = 1.0f;
                if (fadeSamples > 0 && sample < fadeSamples)
                    blend = static_cast<float>(sample) / static_cast<float>(fadeSamples);
                else if (fadeSamples > 0 && sample >= paddedLength - fadeSamples)
                    blend = static_cast<float>(paddedLength - 1 - sample)
                        / static_cast<float>(fadeSamples);
                destination[sample] = original[static_cast<size_t>(sample)] * (1.0f - blend)
                    + corrected[static_cast<size_t>(sample)] * blend;
            }
        }
    }
}

void AlgorithmicPitchPreview::phaseVocoderMono(float* data,
                                                int numSamples,
                                                float ratio)
{
    constexpr int fftOrder = 11;
    constexpr int fftSize = 1 << fftOrder;
    constexpr int hopSize = fftSize / 4;
    if (std::abs(ratio - 1.0f) < 0.0001f || numSamples < fftSize)
        return;

    using Complex = juce::dsp::Complex<float>;
    juce::dsp::FFT fft(fftOrder);
    const auto twoPi = juce::MathConstants<float>::twoPi;
    constexpr auto overlapGain = 1.0f / 1.5f;
    const auto size = static_cast<size_t>(fftSize);
    const auto hop = static_cast<size_t>(hopSize);
    const auto binCount = fftSize / 2 + 1;
    const auto bins = static_cast<size_t>(binCount);

    std::vector<float> window(size);
    for (size_t index = 0; index < size; ++index)
        window[index] = 0.5f
            * (1.0f
               - std::cos(
                   twoPi * static_cast<float>(index) / static_cast<float>(fftSize - 1)));

    std::vector<float> analysisPhase(bins, 0.0f);
    std::vector<float> synthesisPhase(bins, 0.0f);
    std::vector<float> magnitude(bins);
    std::vector<float> frequency(bins);
    std::vector<float> synthesisMagnitude(bins);
    std::vector<float> synthesisFrequency(bins);
    std::vector<float> output(static_cast<size_t>(numSamples) + 2u * size, 0.0f);
    std::vector<Complex> input(size);
    std::vector<Complex> spectrum(size);

    for (int position = 0; position < numSamples; position += hopSize)
    {
        for (size_t index = 0; index < size; ++index)
        {
            const auto sourceIndex = position - fftSize / 2 + static_cast<int>(index);
            const auto sample =
                sourceIndex >= 0 && sourceIndex < numSamples ? data[sourceIndex] : 0.0f;
            input[index] = { sample * window[index], 0.0f };
        }

        fft.perform(input.data(), spectrum.data(), false);
        for (size_t bin = 0; bin < bins; ++bin)
        {
            magnitude[bin] = std::hypot(spectrum[bin].real(), spectrum[bin].imag());
            const auto phase = std::atan2(spectrum[bin].imag(), spectrum[bin].real());
            const auto deviation = wrapPhase(
                phase
                - analysisPhase[bin]
                - static_cast<float>(bin) * twoPi * static_cast<float>(hop)
                    / static_cast<float>(fftSize));
            frequency[bin] = static_cast<float>(bin)
                + deviation * static_cast<float>(fftSize)
                    / (twoPi * static_cast<float>(hop));
            analysisPhase[bin] = phase;
        }

        std::fill(synthesisMagnitude.begin(), synthesisMagnitude.end(), 0.0f);
        std::fill(synthesisFrequency.begin(), synthesisFrequency.end(), 0.0f);
        for (size_t bin = 0; bin < bins; ++bin)
        {
            const auto outputBin = static_cast<int>(
                std::lround(static_cast<float>(bin) * ratio));
            if (outputBin >= 0
                && outputBin < binCount
                && magnitude[bin] > synthesisMagnitude[static_cast<size_t>(outputBin)])
            {
                synthesisMagnitude[static_cast<size_t>(outputBin)] = magnitude[bin];
                synthesisFrequency[static_cast<size_t>(outputBin)] = frequency[bin] * ratio;
            }
        }

        std::fill(input.begin(), input.end(), Complex { 0.0f, 0.0f });
        for (size_t bin = 0; bin < bins; ++bin)
        {
            synthesisPhase[bin] += synthesisFrequency[bin] * twoPi * static_cast<float>(hop)
                / static_cast<float>(fftSize);
            input[bin] = {
                synthesisMagnitude[bin] * std::cos(synthesisPhase[bin]),
                synthesisMagnitude[bin] * std::sin(synthesisPhase[bin])
            };
        }
        for (size_t bin = 1; bin < size / 2; ++bin)
            input[size - bin] = std::conj(input[bin]);

        fft.perform(input.data(), spectrum.data(), true);
        for (size_t index = 0; index < size; ++index)
        {
            const auto destinationIndex = position - fftSize / 2 + static_cast<int>(index);
            if (destinationIndex >= 0
                && destinationIndex < static_cast<int>(output.size()))
            {
                output[static_cast<size_t>(destinationIndex)] +=
                    spectrum[index].real() * window[index] * overlapGain;
            }
        }
    }

    std::copy_n(output.begin(), numSamples, data);
}

} // namespace vocal_annotation
