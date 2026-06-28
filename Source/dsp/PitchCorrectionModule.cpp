#include "PitchCorrectionModule.h"
#include "WORLDResynthesizer.h"
#include "../ai/RVCPythonBridge.h"
#include <cmath>
#include <vector>
#include <complex>

//==============================================================================
PitchCorrectionModule::PitchCorrectionModule() {}

void PitchCorrectionModule::prepare (double sampleRate, int maxBlockSize)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;
    isPrepared_   = true;
}

void PitchCorrectionModule::reset()
{
    detectedPitchHz_.store (0.0f, std::memory_order_relaxed);
    isPrepared_ = false;
}

void PitchCorrectionModule::process (juce::AudioBuffer<float>& /*buffer*/,
                                     float /*pitchShiftCents*/,
                                     float /*formantShift*/)
{
    // Phase 1: pass-through stub — real-time engine wired in Phase 2
    detectedPitchHz_.store (0.0f, std::memory_order_relaxed);
}

//==============================================================================
// Phase vocoder — offline mono pitch shift
//==============================================================================

namespace
{

inline float wrapPhase (float p) noexcept
{
    const float pi  = juce::MathConstants<float>::pi;
    const float pi2 = juce::MathConstants<float>::twoPi;
    while (p >  pi) p -= pi2;
    while (p < -pi) p += pi2;
    return p;
}

} // namespace

//------------------------------------------------------------------------------
void PitchCorrectionModule::phaseVocoderMono (float* data, int numSamples, float ratio)
{
    // ── Constants ──────────────────────────────────────────────────────────────
    constexpr int kOrder = 11;
    constexpr int N      = 1 << kOrder;   // 2048
    constexpr int H      = N / 4;         // 512  →  75 % overlap (4×)

    if (std::abs (ratio - 1.0f) < 1e-4f || numSamples < N) return;

    using Cx = juce::dsp::Complex<float>;
    juce::dsp::FFT fft (kOrder);

    const float pi2 = juce::MathConstants<float>::twoPi;

    // juce::dsp::FFT::perform (isInverse=true) normalises by N on all backends
    // (Accelerate/vDSP, Kiss FFT, IPP).  We must NOT divide by N again.
    // WOLA Hann 4× overlap: sum of w²(n) over 4 frames ≈ 1.5.
    // So the only correction needed is the WOLA normalisation.
    const float gain = 1.0f / 1.5f;

    // Use size_t aliases for all vector sizes to avoid sign-conversion warnings
    const auto szN    = static_cast<size_t> (N);
    const auto szH    = static_cast<size_t> (H);

    // ── Hann window ────────────────────────────────────────────────────────────
    std::vector<float> win (szN);
    for (size_t i = 0; i < szN; ++i)
        win[i] = 0.5f * (1.0f - std::cos (pi2 * (float)i / (float)(N - 1)));

    // ── Phase vocoder state (positive-frequency bins: 0 … N/2) ────────────────
    const int    nBins  = N / 2 + 1;
    const size_t szBins = static_cast<size_t> (nBins);
    std::vector<float> anlPh (szBins, 0.0f);   // last analysis phase per bin
    std::vector<float> synPh (szBins, 0.0f);   // accumulated synthesis phase per bin

    // ── Output accumulation buffer ─────────────────────────────────────────────
    std::vector<float> outBuf (static_cast<size_t> (numSamples) + 2u * szN, 0.0f);

    // ── FFT work buffers ───────────────────────────────────────────────────────
    std::vector<Cx> inBuf (szN), spectrum (szN);

    // ── Main loop — one analysis hop per iteration ─────────────────────────────
    for (int pos = 0; pos < numSamples; pos += H)
    {
        // 1. Load and window the analysis frame (centred at pos)
        for (size_t i = 0; i < szN; ++i)
        {
            const int si = pos - N / 2 + (int)i;
            const float s = (si >= 0 && si < numSamples) ? data[si] : 0.0f;
            inBuf[i] = { s * win[i], 0.0f };
        }

        // 2. Forward FFT
        fft.perform (inBuf.data(), spectrum.data(), false);

        // 3. Magnitude + instantaneous frequency for positive bins
        std::vector<float> mag (szBins), omega (szBins);
        for (size_t k = 0; k < szBins; ++k)
        {
            const float re  = spectrum[k].real();
            const float im  = spectrum[k].imag();
            mag[k]          = std::hypot (re, im);

            const float phi = std::atan2 (im, re);
            // Phase deviation from expected (bin k advances k·2π·H/N per hop)
            const float delta = wrapPhase (phi - anlPh[k]
                                           - (float)k * pi2 * (float)szH / (float)N);
            // True frequency in bin units
            omega[k]  = (float)k + delta * (float)N / (pi2 * (float)szH);
            anlPh[k]  = phi;
        }

        // 4. Pitch-scale: map analysis bins → synthesis bins
        //    Take the dominant (loudest) contributor per output bin.
        std::vector<float> synMag (szBins, 0.0f), synOmega (szBins, 0.0f);
        for (size_t k = 0; k < szBins; ++k)
        {
            const int kOut = (int)std::round ((float)k * ratio);
            if (kOut >= 0 && kOut < nBins && mag[k] > synMag[static_cast<size_t> (kOut)])
            {
                synMag  [static_cast<size_t> (kOut)] = mag[k];
                synOmega[static_cast<size_t> (kOut)] = omega[k] * ratio;
            }
        }

        // 5. Accumulate synthesis phases, build conjugate-symmetric spectrum
        std::fill (inBuf.begin(), inBuf.end(), Cx { 0.0f, 0.0f });
        for (size_t k = 0; k < szBins; ++k)
        {
            synPh[k] += synOmega[k] * pi2 * (float)szH / (float)N;
            inBuf[k]  = { synMag[k] * std::cos (synPh[k]),
                          synMag[k] * std::sin (synPh[k]) };
        }
        // Mirror bins N/2+1 … N-1 (conjugate symmetry for real output)
        for (size_t k = 1; k < szN / 2; ++k)
            inBuf[szN - k] = std::conj (inBuf[k]);

        // 6. Inverse FFT
        fft.perform (inBuf.data(), spectrum.data(), true);

        // 7. Windowed overlap-add into output buffer
        for (size_t i = 0; i < szN; ++i)
        {
            const int di = pos - N / 2 + (int)i;
            if (di >= 0 && di < (int)outBuf.size())
                outBuf[static_cast<size_t> (di)] += spectrum[i].real() * win[i] * gain;
        }
    }

    // Copy corrected samples back
    for (int i = 0; i < numSamples; ++i)
        data[i] = outBuf[static_cast<size_t> (i)];
}

//==============================================================================
void PitchCorrectionModule::applyOfflineCorrections (
    juce::AudioBuffer<float>&           buffer,
    double                              sampleRate,
    const std::vector<NoteCorrection>&  corrections,
    WORLDResynthesizer*                 world,
    RVCPythonBridge*                    rvc)
{
    if (corrections.empty()) return;

    const bool useWORLD = (world != nullptr && world->isAvailable());
    const bool useRVC   = (rvc   != nullptr && rvc->isAvailable());

    // ── PRIMARY: WORLD — whole-buffer mode ───────────────────────────────────
    //
    // Build one F0 contour for the entire buffer and call WORLD once.
    // This avoids all segment-boundary artifacts: WORLD sees the full audio
    // context, so transitions between notes and silence are natural.
    //
    // Contour convention (interpreted by world_bridge.py):
    //   > 0  → correct this frame to the given Hz
    //   == 0 → leave original pitch (Python keeps the harvest F0 for that frame)
    if (useWORLD)
    {
        DBG ("PitchCorrection: using WORLD (primary) - whole-buffer mode");

        const int numSamples = buffer.getNumSamples();
        const int nFrames    = (numSamples + WORLDResynthesizer::kF0HopSamples - 1)
                               / WORLDResynthesizer::kF0HopSamples;

        std::vector<float> f0Contour (static_cast<size_t> (nFrames), 0.0f);

        for (const auto& corr : corrections)
        {
            if (std::abs (corr.pitchRatio - 1.0f) < 1e-4f) continue;

            const int startFrame = juce::jmax (0,
                (int)(corr.startSec * sampleRate) / WORLDResynthesizer::kF0HopSamples);
            const int endFrame = juce::jmin (nFrames,
                (int)(corr.endSec * sampleRate) / WORLDResynthesizer::kF0HopSamples);

            for (int f = startFrame; f < endFrame; ++f)
                f0Contour[static_cast<size_t> (f)] = corr.targetPitchHz;
        }

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            juce::AudioBuffer<float> inBuf  (1, numSamples);
            juce::AudioBuffer<float> outBuf (1, numSamples);
            inBuf.copyFrom (0, 0, buffer, ch, 0, numSamples);

            if (world->resynthesizeAtPitch (inBuf, sampleRate, f0Contour, outBuf))
                buffer.copyFrom (ch, 0, outBuf, 0, 0, numSamples);
            else
                DBG ("PitchCorrection: WORLD failed on ch " + juce::String (ch));
        }
        return;
    }

    // ── SECONDARY / FALLBACK: per-segment (RVC or phase vocoder) ─────────────
    //
    // These paths cannot process the whole buffer at once, so we keep the
    // original per-note loop with crossfade at boundaries.
    for (const auto& corr : corrections)
    {
        if (std::abs (corr.pitchRatio - 1.0f) < 1e-4f) continue;

        const int startSample = juce::jmax (0,
            (int)std::round (corr.startSec * sampleRate));
        const int endSample   = juce::jmin (buffer.getNumSamples(),
            (int)std::round (corr.endSec   * sampleRate));
        const int segLen      = endSample - startSample;
        if (segLen < 512) continue;

        const int fade = juce::jmin (segLen / 4,
                                     (int)std::round (0.008 * sampleRate));

        const int nFrames = (segLen + WORLDResynthesizer::kF0HopSamples - 1)
                            / WORLDResynthesizer::kF0HopSamples;
        const std::vector<float> f0Contour (static_cast<size_t> (nFrames),
                                            corr.targetPitchHz);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const int padStart = juce::jmax (0, startSample - fade);
            const int padEnd   = juce::jmin (buffer.getNumSamples(), endSample + fade);
            const int padLen   = padEnd - padStart;

            const auto szPad = static_cast<size_t> (padLen);
            std::vector<float> orig (szPad);
            const float* src = buffer.getReadPointer (ch);
            for (int i = 0; i < padLen; ++i)
                orig[static_cast<size_t> (i)] = src[padStart + i];

            std::vector<float> seg (szPad);
            std::copy (orig.begin(), orig.end(), seg.begin());

            bool corrected = false;

            if (useRVC)
            {
                juce::AudioBuffer<float> inBuf  (1, padLen);
                juce::AudioBuffer<float> outBuf (1, padLen);
                inBuf.copyFromWithRamp (0, 0, orig.data(), padLen, 1.0f, 1.0f);

                corrected = rvc->resynthesizeAtPitch (inBuf, sampleRate,
                                                      f0Contour, outBuf);
                if (corrected)
                {
                    DBG ("PitchCorrection: using RVC (secondary)");
                    const float* rvcOut = outBuf.getReadPointer (0);
                    for (int i = 0; i < padLen; ++i)
                        seg[static_cast<size_t> (i)] = rvcOut[i];
                }
            }

            if (! corrected)
            {
                DBG ("PitchCorrection: using phase vocoder (fallback)");
                phaseVocoderMono (seg.data(), padLen, corr.pitchRatio);
            }

            float* dst = buffer.getWritePointer (ch);
            for (int i = 0; i < padLen; ++i)
            {
                float blend = 1.0f;
                if      (i < fade)           blend = (float)i / (float)fade;
                else if (i >= padLen - fade)  blend = (float)(padLen - 1 - i) / (float)fade;

                dst[padStart + i] = orig[static_cast<size_t> (i)] * (1.0f - blend)
                                  + seg [static_cast<size_t> (i)] * blend;
            }
        }
    }
}
