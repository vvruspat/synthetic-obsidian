#pragma once
#include <JuceHeader.h>
#include <vector>

// Forward declarations — avoid pulling in the full headers everywhere.
class RVCPythonBridge;
class WORLDResynthesizer;

/** PitchCorrectionModule
 *
 *  Real-time stub (prepare/process) + offline pitch correction
 *  (applyOfflineCorrections).
 *
 *  Offline correction has two paths (see ADR-006):
 *    PRIMARY  — RVC resynthesis via RVCPythonBridge (natural, no DSP artifacts).
 *    FALLBACK — Phase vocoder (FFT 2048, hop 512, Hann 4×) when no RVC preset
 *               is loaded or RVCPythonBridge is nullptr.
 *
 *  Thread safety:
 *    prepare() / reset()           — non-audio thread
 *    process()                     — audio thread only (allocation-free)
 *    applyOfflineCorrections()     — background thread only
 */
class PitchCorrectionModule
{
public:
    // ── Per-note correction descriptor ──────────────────────────────────────
    struct NoteCorrection
    {
        double startSec;
        double endSec;

        /** Frequency ratio for DSP fallback:
         *  2 ^ ((targetSemitones - detectedSemitones) / 12).  1.0 = no change. */
        float  pitchRatio;

        /** Target fundamental frequency in Hz for RVC resynthesis.
         *  = 440 * 2 ^ ((targetMidi + targetCents/100 - 69) / 12).
         *  Used to build the F0 contour passed to RVCPythonBridge. */
        float  targetPitchHz;
    };

    // ── Static offline API ───────────────────────────────────────────────────

    /** Build correction list from a note array.
     *  Each note must expose: midiPitch, centsOffset, origMidiPitch,
     *  origCentsOffset, startSeconds, durationSeconds. */
    template <typename MidiNoteT>
    static std::vector<NoteCorrection>
    buildCorrections (const std::vector<MidiNoteT>& notes);

    /** Apply per-note pitch correction to `buffer` in-place.
     *
     *  Correction path priority (ADR-006):
     *    1. WORLD vocoder  — if `world` is non-null and world->isAvailable()
     *                        (formant-preserving resynthesis from the clip itself)
     *    2. RVC resynthesis — if `rvc`   is non-null and rvc->isAvailable()
     *                        (neural resynthesis; reserved for voice conversion)
     *    3. Phase vocoder  — always-available DSP fallback
     *
     *  Blocking — must be called from a background thread. */
    static void applyOfflineCorrections (juce::AudioBuffer<float>&          buffer,
                                         double                             sampleRate,
                                         const std::vector<NoteCorrection>& corrections,
                                         WORLDResynthesizer*                world = nullptr,
                                         RVCPythonBridge*                   rvc   = nullptr);

    // ── Real-time API (Phase 1: pass-through) ────────────────────────────────
    PitchCorrectionModule();
    void prepare (double sampleRate, int maxBlockSize);
    void reset   ();
    void process (juce::AudioBuffer<float>& buffer,
                  float pitchShiftCents,
                  float formantShift);

    float getDetectedPitchHz() const { return detectedPitchHz_.load(); }

private:
    /** Phase-vocoder pitch shift of a single mono segment in-place. */
    static void phaseVocoderMono (float* data, int numSamples, float ratio);

    double sampleRate_   { 44100.0 };
    int    maxBlockSize_ { 512 };
    bool   isPrepared_   { false };

    std::atomic<float> detectedPitchHz_ { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchCorrectionModule)
};

// ── Template implementation ──────────────────────────────────────────────────
template <typename MidiNoteT>
std::vector<PitchCorrectionModule::NoteCorrection>
PitchCorrectionModule::buildCorrections (const std::vector<MidiNoteT>& notes)
{
    std::vector<NoteCorrection> out;
    out.reserve (notes.size());

    for (const auto& n : notes)
    {
        const float targetTotal   = (float)n.midiPitch     + n.centsOffset     / 100.0f;
        const float detectedTotal = (float)n.origMidiPitch + n.origCentsOffset / 100.0f;
        const float delta         = targetTotal - detectedTotal;    // semitones

        if (std::abs (delta) < 0.02f) continue;   // skip < ~2 cents

        // Target fundamental in Hz:  A4 = 440 Hz, MIDI 69, 12 semitones/octave.
        const float targetHz = 440.0f * std::pow (2.0f, (targetTotal - 69.0f) / 12.0f);

        out.push_back ({ n.startSeconds,
                         n.startSeconds + n.durationSeconds,
                         std::pow (2.0f, delta / 12.0f),
                         targetHz });
    }

    return out;
}
