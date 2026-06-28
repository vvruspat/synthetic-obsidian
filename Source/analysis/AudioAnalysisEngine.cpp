#include "AudioAnalysisEngine.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    juce::Colour noteColourForPitch (int midiPitch)
    {
        if (midiPitch >= 60) return juce::Colour (Theme::kAccentCyan);
        if (midiPitch >= 48) return juce::Colour (Theme::kAccentPurple);
        return juce::Colour (Theme::kSuccess);
    }

    void parsePitchContour (const juce::String& encodedCurve,
                            double noteDuration,
                            MidiNote& note)
    {
        if (encodedCurve.isEmpty())
            return;

        const auto pairs = juce::StringArray::fromTokens (encodedCurve, ";", {});
        for (const auto& pair : pairs)
        {
            if (note.pitchContourSize >= MidiNote::kMaxPitchContourPoints)
                break;

            const auto offsetText = pair.upToFirstOccurrenceOf (":", false, false);
            const auto midiText = pair.fromFirstOccurrenceOf (":", false, false);
            if (offsetText.isEmpty() || midiText.isEmpty())
                continue;

            const float offsetSeconds = offsetText.getFloatValue();
            const float midiPitch = midiText.getFloatValue();
            if (! std::isfinite (offsetSeconds) || ! std::isfinite (midiPitch))
                continue;
            if (offsetSeconds < 0.0f || offsetSeconds > (float)noteDuration + 0.001f)
                continue;
            if (midiPitch < 0.0f || midiPitch > 127.0f)
                continue;

            note.pitchContour[(size_t)note.pitchContourSize] = { offsetSeconds, midiPitch };
            ++note.pitchContourSize;
        }
    }

}

//==============================================================================
AudioAnalysisEngine::AudioAnalysisEngine()
    : juce::Thread ("AudioAnalysis")
{
    formatManager_.registerBasicFormats();   // wav, aiff, flac, ogg
    startThread (juce::Thread::Priority::background);
}

AudioAnalysisEngine::~AudioAnalysisEngine()
{
    workReady_.signal();
    stopThread (3000);
}

//==============================================================================
void AudioAnalysisEngine::analyzeFile (const juce::Uuid& trackId,
                                       const juce::File& audioFile,
                                       NotesReadyFn      callback)
{
    {
        const juce::ScopedLock lock (jobsLock_);
        jobs_.push ({ trackId, audioFile, std::move (callback), sensitivity_.load() });
    }
    workReady_.signal();
}

void AudioAnalysisEngine::setSensitivity (float sensitivity) noexcept
{
    sensitivity_.store (juce::jlimit (0.0f, 1.0f, sensitivity));
}

//==============================================================================
void AudioAnalysisEngine::run()
{
    while (! threadShouldExit())
    {
        workReady_.wait (-1);   // sleep until new work arrives

        while (true)
        {
            Job job;
            {
                const juce::ScopedLock lock (jobsLock_);
                if (jobs_.empty()) break;
                job = std::move (jobs_.front());
                jobs_.pop();
            }

            if (threadShouldExit()) return;

            // ── Read audio file ───────────────────────────────────────────────
            std::unique_ptr<juce::AudioFormatReader> reader (
                formatManager_.createReaderFor (job.audioFile));

            if (reader == nullptr)
            {
                // Post empty result on message thread
                const auto trackId = job.trackId;
                auto emptyCallback = std::move (job.callback);
                juce::MessageManager::callAsync ([trackId, cb = std::move (emptyCallback)]
                {
                    cb (trackId, {});
                });
                continue;
            }

            const double sampleRate = reader->sampleRate;

            // Limit to first 120 seconds to avoid memory/time blowout
            const juce::int64 maxSamples =
                juce::jmin (reader->lengthInSamples,
                            (juce::int64)(sampleRate * 120.0));

            juce::AudioBuffer<float> fileBuffer (
                (int)reader->numChannels, (int)maxSamples);
            reader->read (&fileBuffer, 0, (int)maxSamples, 0, true, true);

            if (threadShouldExit()) return;

            // ── Downmix to mono ───────────────────────────────────────────────
            juce::AudioBuffer<float> mono (1, (int)maxSamples);
            mono.clear();
            for (int ch = 0; ch < fileBuffer.getNumChannels(); ++ch)
                mono.addFrom (0, 0,
                              fileBuffer, ch, 0,
                              (int)maxSamples,
                              1.0f / (float)fileBuffer.getNumChannels());

            if (threadShouldExit()) return;

            if (auto aiNotes = analyzeWithPythonAi (job.audioFile, job.sensitivity);
                aiNotes.has_value() && ! aiNotes->empty())
            {
                const auto trackId = job.trackId;
                auto resultCallback = std::move (job.callback);
                juce::MessageManager::callAsync (
                    [trackId, notes = std::move (*aiNotes), cb = std::move (resultCallback)]
                    {
                        cb (trackId, std::move (notes));
                    });
                continue;
            }

            if (auto pyinNotes = analyzeWithPythonPyin (job.audioFile, job.sensitivity);
                pyinNotes.has_value() && ! pyinNotes->empty())
            {
                const auto trackId = job.trackId;
                auto resultCallback = std::move (job.callback);
                juce::MessageManager::callAsync (
                    [trackId, notes = std::move (*pyinNotes), cb = std::move (resultCallback)]
                    {
                        cb (trackId, std::move (notes));
                    });
                continue;
            }

            // ── Pitch detection ───────────────────────────────────────────────
            auto frames = detectPitchFrames (mono, sampleRate);

            if (threadShouldExit()) return;

            // ── Frame → note events ───────────────────────────────────────────
            const double hopSec    = kHopMs / 1000.0;
            auto stableFrames      = stabilisePitchFrames (frames, hopSec);
            auto mergedNotes       = mergeToNotes (stableFrames, hopSec);

            // ── Post result on message thread ─────────────────────────────────
            const auto trackId     = job.trackId;
            auto resultCallback    = std::move (job.callback);
            juce::MessageManager::callAsync (
                [trackId, notes = std::move (mergedNotes), cb = std::move (resultCallback)]
                {
                    cb (trackId, std::move (notes));
                });
        }
    }
}

//==============================================================================
// YIN pitch estimator
// Reference: De Cheveigné & Kawahara (2002) — "YIN, a fundamental frequency estimator
//            for speech and music"

float AudioAnalysisEngine::yinEstimate (const float* frame,
                                         int          frameSize,
                                         float        sampleRate,
                                         float&       outConfidence) noexcept
{
    // Search range: 50 Hz (sub-bass) … 1 000 Hz (above soprano high C)
    const int tauMin = juce::jmax (2, (int)(sampleRate / 1000.0f));
    const int tauMax = juce::jmin (frameSize / 2 - 1,
                                   (int)(sampleRate / 50.0f));

    if (tauMin >= tauMax)
    {
        outConfidence = 0.0f;
        return 0.0f;
    }

    const int halfN = frameSize / 2;

    // ── Step 1: difference function d(τ) ─────────────────────────────────────
    juce::HeapBlock<float> d (tauMax + 1, true);
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        for (int i = 0; i < halfN; ++i)
        {
            const float diff = frame[i] - frame[i + tau];
            d[tau] += diff * diff;
        }
    }

    // ── Step 2: cumulative mean normalised difference d'(τ) ──────────────────
    juce::HeapBlock<float> dp (tauMax + 1);
    dp[0] = 1.0f;
    float runSum = 0.0f;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        runSum += d[tau];
        dp[tau] = (runSum > 1e-10f)
                  ? d[tau] * (float)tau / runSum
                  : 1.0f;
    }

    // ── Step 3: absolute threshold — first dip < kYinThresh ──────────────────
    int bestTau = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (dp[tau] < kYinThresh)
        {
            // Walk to local minimum
            while (tau + 1 <= tauMax && dp[tau + 1] < dp[tau])
                ++tau;
            bestTau = tau;
            break;
        }
    }

    // Fallback: use global minimum in search range
    if (bestTau < 0)
    {
        float minVal = dp[tauMin];
        bestTau = tauMin;
        for (int tau = tauMin + 1; tau <= tauMax; ++tau)
        {
            if (dp[tau] < minVal) { minVal = dp[tau]; bestTau = tau; }
        }
        outConfidence = 1.0f - dp[bestTau];
        if (outConfidence < kMinConfid)
        {
            outConfidence = 0.0f;
            return 0.0f;   // unvoiced
        }
    }
    else
    {
        outConfidence = 1.0f - dp[bestTau];
    }

    // ── Step 4: parabolic interpolation ──────────────────────────────────────
    float refinedTau = (float)bestTau;
    if (bestTau > tauMin && bestTau < tauMax)
    {
        const float s0 = dp[bestTau - 1];
        const float s1 = dp[bestTau];
        const float s2 = dp[bestTau + 1];
        const float denom = 2.0f * s1 - s0 - s2;
        if (std::abs (denom) > 1e-8f)
            refinedTau = (float)bestTau + (s0 - s2) / (2.0f * denom);
    }

    return (refinedTau > 0.5f) ? sampleRate / refinedTau : 0.0f;
}

//==============================================================================
std::vector<AudioAnalysisEngine::PitchFrame>
AudioAnalysisEngine::detectPitchFrames (const juce::AudioBuffer<float>& mono,
                                         double sampleRate) const
{
    const int  frameSize  = (int)(sampleRate * kFrameMs / 1000.0);
    const int  hopSize    = (int)(sampleRate * kHopMs   / 1000.0);
    const int  numSamples = mono.getNumSamples();
    const auto* data      = mono.getReadPointer (0);

    std::vector<PitchFrame> frames;
    frames.reserve ((size_t)(numSamples / hopSize) + 4);

    // Hann window
    juce::HeapBlock<float> window (frameSize);
    for (int i = 0; i < frameSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi
                                              * (float)i / (float)(frameSize - 1)));

    juce::HeapBlock<float> windowed (frameSize);

    for (int offset = 0; offset + frameSize <= numSamples; offset += hopSize)
    {
        if (threadShouldExit()) break;

        float energy = 0.0f;
        for (int i = 0; i < frameSize; ++i)
        {
            const float sample = data[offset + i];
            energy += sample * sample;
        }
        const float rms = std::sqrt (energy / (float)frameSize);

        // Apply Hann window
        for (int i = 0; i < frameSize; ++i)
            windowed[i] = data[offset + i] * window[i];

        float confidence = 0.0f;
        const float hz   = yinEstimate (windowed.getData(), frameSize,
                                        (float)sampleRate, confidence);

        // Convert Hz → MIDI note (12 * log2(f / 440) + 69)
        int midiPitch = -1;
        if (hz > 0.0f && confidence >= kMinConfid)
        {
            const float midi = 12.0f * std::log2 (hz / 440.0f) + 69.0f;
            midiPitch = (int)std::round (midi);
            // Clamp to valid MIDI range
            if (midiPitch < 0 || midiPitch > 127) midiPitch = -1;
        }

        const double timeSec = (double)offset / sampleRate;
        frames.push_back ({ timeSec, midiPitch, confidence, hz, rms });
    }

    return frames;
}

//==============================================================================
std::optional<std::vector<MidiNote>>
AudioAnalysisEngine::analyzeWithPythonAi (const juce::File& audioFile,
                                          float sensitivity)
{
    return runPythonNoteAnalyzer (audioFile, "analyze_notes_ai.py", sensitivity);
}

std::optional<std::vector<MidiNote>>
AudioAnalysisEngine::analyzeWithPythonPyin (const juce::File& audioFile,
                                            float sensitivity)
{
    return runPythonNoteAnalyzer (audioFile, "analyze_notes_pyin.py", sensitivity);
}

std::optional<std::vector<MidiNote>>
AudioAnalysisEngine::runPythonNoteAnalyzer (const juce::File& audioFile,
                                            const juce::String& scriptName,
                                            float sensitivity)
{
    if (! audioFile.existsAsFile())
        return std::nullopt;

    const auto projectRoot = findProjectRoot();
    if (! projectRoot.isDirectory())
        return std::nullopt;

    const auto python = projectRoot.getChildFile ("research/.venv_seed_vc/bin/python");
    const auto script = projectRoot.getChildFile ("research/svc_pitch").getChildFile (scriptName);
    if (! python.existsAsFile() || ! script.existsAsFile())
        return std::nullopt;

    const juce::StringArray args {
        python.getFullPathName(),
        script.getFullPathName(),
        audioFile.getFullPathName(),
        "--sensitivity",
        juce::String (juce::jlimit (0.0f, 1.0f, sensitivity), 3)
    };

    juce::ChildProcess process;
    if (! process.start (args, juce::ChildProcess::wantStdOut))
        return std::nullopt;

    juce::String output;
    while (process.isRunning())
    {
        output << process.readAllProcessOutput();
        juce::Thread::sleep (100);
    }
    output << process.readAllProcessOutput();

    if (process.getExitCode() != 0)
        return std::nullopt;

    const auto parsed = juce::JSON::parse (output);
    const auto* root = parsed.getDynamicObject();
    if (root == nullptr)
        return std::nullopt;

    const auto notesVar = root->getProperty ("notes");
    const auto* notesArray = notesVar.getArray();
    if (notesArray == nullptr)
        return std::nullopt;

    std::vector<MidiNote> notes;
    notes.reserve ((size_t)notesArray->size());

    for (const auto& item : *notesArray)
    {
        const auto* obj = item.getDynamicObject();
        if (obj == nullptr)
            continue;

        const int pitch = (int)obj->getProperty ("pitch");
        const double start = (double)obj->getProperty ("start");
        const double duration = (double)obj->getProperty ("duration");
        if (pitch < 0 || pitch > 127 || duration <= 0.0)
            continue;

        const float confidence = (float)(double)obj->getProperty ("confidence");
        const float cents = (float)(double)obj->getProperty ("cents");

        MidiNote note;
        note.midiPitch = pitch;
        note.origMidiPitch = pitch;
        note.startSeconds = start;
        note.durationSeconds = duration;
        note.confidence = juce::jlimit (0.0f, 1.0f, confidence);
        note.centsOffset = juce::jlimit (-50.0f, 50.0f, cents);
        note.origCentsOffset = note.centsOffset;
        note.colour = noteColourForPitch (pitch);
        parsePitchContour (obj->getProperty ("curve").toString(), duration, note);
        note.syllableStart = (bool)obj->getProperty ("syllable_start");
        note.lyric = obj->getProperty ("lyric").toString().substring (0, 32);

        notes.push_back (note);
    }

    return notes;
}

juce::File AudioAnalysisEngine::findProjectRoot()
{
    juce::Array<juce::File> starts;
    starts.add (juce::File::getCurrentWorkingDirectory());
    starts.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
    starts.add (juce::File ("/Users/aleksandrkolesov/synthetic-obsidian"));

    for (auto start : starts)
    {
        for (int depth = 0; depth < 8 && start.isDirectory(); ++depth)
        {
            if (start.getChildFile ("research/svc_pitch/analyze_notes_ai.py").existsAsFile()
                && start.getChildFile ("research/.venv_seed_vc/bin/python").existsAsFile())
                return start;

            start = start.getParentDirectory();
        }
    }

    return {};
}

//==============================================================================
std::vector<AudioAnalysisEngine::PitchFrame>
AudioAnalysisEngine::stabilisePitchFrames (const std::vector<PitchFrame>& frames,
                                           double hopSec)
{
    if (frames.empty())
        return {};

    auto stable = frames;
    const int maxGapFrames = juce::jmax (1, (int)std::round ((double)kFillGapS / hopSec));
    float maxRms = 0.0f;
    for (const auto& frame : stable)
        maxRms = juce::jmax (maxRms, frame.rms);

    const float minBridgeRms = juce::jmax (1.0e-5f, maxRms * kEnergyBridgeRatio);

    auto gapHasVocalEnergy = [&stable, minBridgeRms] (int first, int last) -> bool
    {
        if (first > last)
            return false;

        int energeticFrames = 0;
        for (int idx = first; idx <= last; ++idx)
            if (stable[(size_t)idx].rms >= minBridgeRms)
                ++energeticFrames;

        return energeticFrames >= juce::jmax (1, (last - first + 1) / 3);
    };

    // Bridge brief YIN dropouts inside a continuous vocal note. This is the main
    // fix for "dotted" notes where vibrato or breath noise creates tiny holes.
    int i = 0;
    while (i < (int)stable.size())
    {
        if (stable[(size_t)i].midiPitch >= 0)
        {
            ++i;
            continue;
        }

        const int gapStart = i;
        while (i < (int)stable.size() && stable[(size_t)i].midiPitch < 0)
            ++i;

        const int gapEnd = i - 1;
        const int left = gapStart - 1;
        const int right = i;
        const int gapLen = gapEnd - gapStart + 1;

        if (gapLen > maxGapFrames || ! gapHasVocalEnergy (gapStart, gapEnd))
            continue;

        if (left < 0 && right >= (int)stable.size())
            continue;

        if (left < 0 || right >= (int)stable.size())
        {
            const int anchor = (left >= 0) ? left : right;
            if (anchor < 0 || anchor >= (int)stable.size() || stable[(size_t)anchor].midiPitch < 0)
                continue;

            // Extend note edges slightly through energetic frames. This keeps a
            // sung syllable from visually disappearing at its fade-in/fade-out.
            for (int g = gapStart; g <= gapEnd; ++g)
            {
                auto& frame = stable[(size_t)g];
                frame.midiPitch = stable[(size_t)anchor].midiPitch;
                frame.confidence = stable[(size_t)anchor].confidence * 0.5f;
                frame.freqHz = stable[(size_t)anchor].freqHz;
            }
            continue;
        }

        const auto& leftFrame = stable[(size_t)left];
        const auto& rightFrame = stable[(size_t)right];
        if (leftFrame.midiPitch < 0 || rightFrame.midiPitch < 0)
            continue;

        if (std::abs (leftFrame.midiPitch - rightFrame.midiPitch) > kGapBridgeSemitoneTolerance)
            continue;

        for (int g = gapStart; g <= gapEnd; ++g)
        {
            const float alpha = (float)(g - left) / (float)(right - left);
            auto& frame = stable[(size_t)g];
            const float interpolatedPitch = (float)leftFrame.midiPitch
                                          + (float)(rightFrame.midiPitch - leftFrame.midiPitch) * alpha;
            frame.midiPitch = (int)std::round (interpolatedPitch);
            frame.confidence = juce::jmin (leftFrame.confidence, rightFrame.confidence) * 0.65f;

            if (leftFrame.freqHz > 0.0f && rightFrame.freqHz > 0.0f)
                frame.freqHz = leftFrame.freqHz + (rightFrame.freqHz - leftFrame.freqHz) * alpha;
        }
    }

    auto medianPitchAround = [&stable] (int center) -> int
    {
        std::array<int, kMedianRadiusFrames * 2 + 1> pitches {};
        int count = 0;
        const int first = juce::jmax (0, center - kMedianRadiusFrames);
        const int last = juce::jmin ((int)stable.size() - 1, center + kMedianRadiusFrames);

        for (int idx = first; idx <= last; ++idx)
        {
            const int pitch = stable[(size_t)idx].midiPitch;
            if (pitch >= 0)
                pitches[(size_t)count++] = pitch;
        }

        if (count < 3)
            return stable[(size_t)center].midiPitch;

        std::sort (pitches.begin(), pitches.begin() + count);
        return pitches[(size_t)(count / 2)];
    };

    auto smoothed = stable;
    for (int idx = 0; idx < (int)stable.size(); ++idx)
    {
        if (stable[(size_t)idx].midiPitch >= 0)
            smoothed[(size_t)idx].midiPitch = medianPitchAround (idx);
    }

    return smoothed;
}

//==============================================================================
std::vector<MidiNote>
AudioAnalysisEngine::mergeToNotes (const std::vector<PitchFrame>& frames,
                                    double hopSec)
{
    std::vector<MidiNote> notes;
    if (frames.empty()) return notes;

    int    currentPitch  = -1;
    double noteStart     = 0.0;
    double lastFrameTime = 0.0;
    float  maxConfidence = 0.0f;
    double freqSum       = 0.0;   // accumulate frame frequencies for avg cents
    int    freqCount     = 0;
    std::array<int, 128> pitchHistogram {};

    // Helper: cents offset from ideal MIDI frequency
    auto centsBetween = [] (float avgHz, int midiPitch) -> float
    {
        if (avgHz <= 0.0f || midiPitch < 0) return 0.0f;
        const float idealHz = 440.0f * std::pow (2.0f, (float)(midiPitch - 69) / 12.0f);
        return juce::jlimit (-50.0f, 50.0f,
                             1200.0f * std::log2 (avgHz / idealHz));
    };

    auto commitNote = [&] (double endTime)
    {
        const double dur = endTime - noteStart;
        if (currentPitch >= 0 && dur >= kMinNoteS)
        {
            int dominantPitch = currentPitch;
            int dominantCount = 0;
            for (int pitch = 0; pitch < (int)pitchHistogram.size(); ++pitch)
            {
                if (pitchHistogram[(size_t)pitch] > dominantCount)
                {
                    dominantCount = pitchHistogram[(size_t)pitch];
                    dominantPitch = pitch;
                }
            }

            const float avgHz = (freqCount > 0) ? (float)(freqSum / freqCount) : 0.0f;

            const float detectedCents = centsBetween (avgHz, dominantPitch);

            MidiNote n;
            n.midiPitch        = dominantPitch;
            n.origMidiPitch    = dominantPitch;    // immutable — correction reference
            n.startSeconds     = noteStart;
            n.durationSeconds  = dur;
            n.confidence       = juce::jlimit (0.0f, 1.0f, maxConfidence);
            n.centsOffset      = detectedCents;
            n.origCentsOffset  = detectedCents;   // immutable
            n.colour           = noteColourForPitch (dominantPitch);
            notes.push_back (n);
        }
    };

    for (const auto& frame : frames)
    {
        const bool pitched   = (frame.midiPitch >= 0);
        const bool samePitch = pitched
                            && currentPitch >= 0
                            && std::abs (frame.midiPitch - currentPitch) <= kVibratoSemitoneTolerance;
        const bool gapOK     = (frame.timeSeconds - lastFrameTime) <= (kMergeGapS + hopSec);

        if (samePitch && gapOK)
        {
            // Extend current note
            maxConfidence = juce::jmax (maxConfidence, frame.confidence);
            lastFrameTime = frame.timeSeconds;
            if (frame.freqHz > 0.0f) { freqSum += frame.freqHz; ++freqCount; }
            if (frame.midiPitch >= 0 && frame.midiPitch < (int)pitchHistogram.size())
                ++pitchHistogram[(size_t)frame.midiPitch];
        }
        else
        {
            // Commit previous note
            if (currentPitch >= 0)
                commitNote (lastFrameTime + hopSec);

            if (pitched)
            {
                currentPitch  = frame.midiPitch;
                noteStart     = frame.timeSeconds;
                lastFrameTime = frame.timeSeconds;
                maxConfidence = frame.confidence;
                freqSum       = frame.freqHz;
                freqCount     = (frame.freqHz > 0.0f) ? 1 : 0;
                pitchHistogram.fill (0);
                if (frame.midiPitch >= 0 && frame.midiPitch < (int)pitchHistogram.size())
                    ++pitchHistogram[(size_t)frame.midiPitch];
            }
            else
            {
                currentPitch = -1;
                freqSum      = 0.0;
                freqCount    = 0;
                pitchHistogram.fill (0);
            }
        }
    }

    // Commit final note
    if (currentPitch >= 0 && ! frames.empty())
        commitNote (frames.back().timeSeconds + hopSec);

    return notes;
}
