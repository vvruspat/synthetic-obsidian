#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace ParamIDs
{
    static constexpr auto pitchDrift   = "pitch_drift";
    static constexpr auto formantShift = "formant_shift";
    static constexpr auto aiInfluence  = "ai_influence";
    static constexpr auto vibratoScale = "vibrato_scale";
}

namespace
{
    float midiPitchToHz (int midiPitch) noexcept
    {
        return 440.0f * std::pow (2.0f, (float)(midiPitch - 69) / 12.0f);
    }
}

SyntheticObsidianProcessor::SyntheticObsidianProcessor()
    : juce::AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_ (*this, nullptr, "Parameters", createParameterLayout()),
      projectState_ (),
      audioEngine_  ()
{
    playerFormatManager_.registerBasicFormats();
    readAheadThread_.startThread (juce::Thread::Priority::normal);
}

SyntheticObsidianProcessor::~SyntheticObsidianProcessor()
{
    // Stop the WORLD bridge subprocess before anything else tears down
    worldResynthesizer_.stopBridge();

    // Stop all transports before destroying readers
    {
        juce::ScopedLock lock (playerLock_);
        for (auto& [id, player] : trackPlayers_)
            if (player && player->transport)
                player->transport->stop();
        trackPlayers_.clear();
    }
    readAheadThread_.stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout
SyntheticObsidianProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::pitchDrift, "Pitch Drift",
        juce::NormalisableRange<float> (-100.0f, 100.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("cents")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::formantShift, "Formant Shift",
        juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::aiInfluence, "AI Influence",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.84f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::vibratoScale, "Vibrato Scale",
        juce::NormalisableRange<float> (0.0f, 10.0f), 3.4f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    return layout;
}

//==============================================================================
void SyntheticObsidianProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = samplesPerBlock;
    audioEngine_.prepare (sampleRate, samplesPerBlock);
    mixBuf_.setSize (2, samplesPerBlock);

    juce::ScopedLock lock (playerLock_);
    for (auto& [id, player] : trackPlayers_)
    {
        if (player && player->transport)
        {
            player->transport->prepareToPlay (samplesPerBlock, sampleRate);
            player->prepared = true;
        }
    }
}

void SyntheticObsidianProcessor::releaseResources()
{
    audioEngine_.release();

    juce::ScopedLock lock (playerLock_);
    for (auto& [id, player] : trackPlayers_)
    {
        if (player && player->transport)
        {
            player->transport->stop();
            player->transport->releaseResources();
            player->prepared = false;
        }
    }
}

void SyntheticObsidianProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                               juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // ── DSP effects ────────────────────────────────────────────────────────────
    audioEngine_.setPitchDriftCents (*apvts_.getRawParameterValue (ParamIDs::pitchDrift));
    audioEngine_.setFormantShift    (*apvts_.getRawParameterValue (ParamIDs::formantShift));
    audioEngine_.setAiInfluence     (*apvts_.getRawParameterValue (ParamIDs::aiInfluence));
    audioEngine_.setVibratoScale    (*apvts_.getRawParameterValue (ParamIDs::vibratoScale));
    audioEngine_.process (buffer);

    // ── Track file playback ────────────────────────────────────────────────────
    {
        const juce::GenericScopedTryLock<juce::CriticalSection> tryLock (playerLock_);
        if (tryLock.isLocked())
        {
            mixBuf_.setSize (buffer.getNumChannels(), numSamples, false, false, true);
            juce::AudioSourceChannelInfo info (&mixBuf_, 0, numSamples);

            // Solo check: is any track soloed?
            const bool anySoloed = std::any_of (
                trackPlayers_.begin(), trackPlayers_.end(),
                [] (const auto& kv) { return kv.second && kv.second->soloed; });

            for (auto& [id, player] : trackPlayers_)
            {
                if (!player || !player->prepared || !player->transport) continue;

                // Always advance transport to keep positions in sync
                mixBuf_.clear();
                player->transport->getNextAudioBlock (info);

                // Effective mute: explicitly muted, or solo-excluded
                const bool effectiveMuted = player->muted
                                         || (anySoloed && !player->soloed);
                if (effectiveMuted) continue;

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    const int srcCh = juce::jmin (ch, mixBuf_.getNumChannels() - 1);
                    buffer.addFrom (ch, 0, mixBuf_, srcCh, 0, numSamples, player->volume);
                }

                if (player->midiAuditionEnabled && ! player->detectedNotes.empty())
                {
                    const double blockStart = playbackPos_.load();
                    const double blockEnd = blockStart + (double)numSamples / sampleRate_;
                    constexpr float auditionGain = 0.16f;

                    for (const auto& note : player->detectedNotes)
                    {
                        const double noteStart = note.startSeconds;
                        const double noteEnd = note.startSeconds + note.durationSeconds;
                        if (noteEnd <= blockStart || noteStart >= blockEnd)
                            continue;

                        const int startSample = juce::jlimit (
                            0, numSamples,
                            (int)std::floor ((noteStart - blockStart) * sampleRate_));
                        const int endSample = juce::jlimit (
                            0, numSamples,
                            (int)std::ceil ((noteEnd - blockStart) * sampleRate_));
                        if (endSample <= startSample)
                            continue;

                        const float hz = midiPitchToHz (note.midiPitch);
                        const float amplitude = auditionGain * player->volume
                                              * (0.35f + 0.65f * juce::jlimit (0.0f, 1.0f, note.confidence));

                        for (int sample = startSample; sample < endSample; ++sample)
                        {
                            const double t = blockStart + (double)sample / sampleRate_ - noteStart;
                            const double notePos = (double)sample / sampleRate_ + blockStart;
                            const double rel = notePos - noteStart;
                            const double remain = noteEnd - notePos;
                            const float env = (float)juce::jlimit (
                                0.0, 1.0, juce::jmin (rel / 0.012, remain / 0.025));
                            const float value = amplitude * env * (float)std::sin (
                                juce::MathConstants<double>::twoPi * (double)hz * t);

                            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                                buffer.addSample (ch, sample, value);
                        }
                    }
                }
            }

            // Sync playback position from first prepared transport
            if (isPlaying_.load())
            {
                for (auto& [id, player] : trackPlayers_)
                {
                    if (player && player->prepared && player->transport)
                    {
                        playbackPos_.store (player->transport->getCurrentPosition());
                        break;
                    }
                }
            }
        }
    }

    // ── Manual playhead advance (when no files loaded) ─────────────────────────
    if (isPlaying_.load() && trackPlayers_.empty())
    {
        const double advance = numSamples / sampleRate_;
        playbackPos_.store (playbackPos_.load() + advance);
    }

    // ── CPU load estimate ──────────────────────────────────────────────────────
    if (numSamples > 0)
    {
        float rms = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            rms += buffer.getRMSLevel (ch, 0, numSamples);
        rms /= (float)juce::jmax (1, buffer.getNumChannels());
        cpuLoad_.store (juce::jlimit (0.0f, 1.0f, rms));
    }
}

bool SyntheticObsidianProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getChannelSet (false, 0);
    if (out != juce::AudioChannelSet::stereo()) return false;

    const auto& in = layouts.getChannelSet (true, 0);
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

juce::AudioProcessorEditor* SyntheticObsidianProcessor::createEditor()
{
    return new SyntheticObsidianEditor (*this);
}

//==============================================================================
void SyntheticObsidianProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts_.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    auto projectVt  = projectState_.toValueTree();
    auto projectXml = projectVt.createXml();

    auto wrapper = std::make_unique<juce::XmlElement> ("SyntheticObsidian");
    if (xml)        wrapper->addChildElement (xml.release());
    if (projectXml) wrapper->addChildElement (projectXml.release());

    copyXmlToBinary (*wrapper, destData);
}

void SyntheticObsidianProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (!xmlState) return;

    if (auto* el = xmlState->getChildByName (apvts_.state.getType()))
        apvts_.replaceState (juce::ValueTree::fromXml (*el));

    if (auto* el = xmlState->getChildByName ("ProjectState"))
        projectState_.fromValueTree (juce::ValueTree::fromXml (*el));
}

//==============================================================================
// Playback transport

void SyntheticObsidianProcessor::setPlaying (bool shouldPlay)
{
    juce::ScopedLock lock (playerLock_);
    for (auto& [id, player] : trackPlayers_)
    {
        if (!player || !player->transport) continue;

        if (shouldPlay)
        {
            // Sync position before starting
            player->transport->setPosition (playbackPos_.load());
            player->transport->start();
        }
        else
        {
            player->transport->stop();
        }
    }
    isPlaying_.store (shouldPlay);
}

void SyntheticObsidianProcessor::resetPlaybackPosition()
{
    isPlaying_.store (false);

    juce::ScopedLock lock (playerLock_);
    for (auto& [id, player] : trackPlayers_)
    {
        if (!player || !player->transport) continue;
        player->transport->stop();
        player->transport->setPosition (0.0);
    }
    playbackPos_.store (0.0);
}

//==============================================================================
// Per-track audio file loading

void SyntheticObsidianProcessor::loadTrackAudio (const juce::Uuid& id,
                                                  const juce::File& file)
{
    // Create reader on calling thread (message thread)
    auto reader = std::unique_ptr<juce::AudioFormatReader> (
        playerFormatManager_.createReaderFor (file));

    if (reader == nullptr) return;

    auto player = std::make_unique<TrackPlayer>();
    player->file         = file;
    player->originalFile = file;   // keep pristine reference for pitch correction
    player->reader       = std::move (reader);
    player->readerSource = std::make_unique<juce::AudioFormatReaderSource> (
        player->reader.get(), false);
    player->transport   = std::make_unique<juce::AudioTransportSource>();

    // Hook up with read-ahead buffering (32k samples = ~0.7s at 44.1 kHz)
    player->transport->setSource (player->readerSource.get(),
                                  32768, &readAheadThread_,
                                  player->reader->sampleRate,
                                  (int)player->reader->numChannels);

    if (sampleRate_ > 0)
    {
        player->transport->prepareToPlay (maxBlockSize_, sampleRate_);
        player->prepared = true;
    }

    // Seek to current position so it's in sync when play is pressed
    player->transport->setPosition (playbackPos_.load());

    // If already playing, start immediately
    if (isPlaying_.load())
        player->transport->start();

    {
        juce::ScopedLock lock (playerLock_);
        trackPlayers_[id] = std::move (player);
    }
}

void SyntheticObsidianProcessor::removeTrackAudio (const juce::Uuid& id)
{
    juce::ScopedLock lock (playerLock_);
    if (auto it = trackPlayers_.find (id); it != trackPlayers_.end())
    {
        if (it->second && it->second->transport)
            it->second->transport->stop();
        trackPlayers_.erase (it);
    }
}

//==============================================================================
// Per-track mix controls

void SyntheticObsidianProcessor::setTrackVolume (const juce::Uuid& id, float vol)
{
    juce::ScopedLock lock (playerLock_);
    if (auto it = trackPlayers_.find (id); it != trackPlayers_.end() && it->second)
        it->second->volume = juce::jlimit (0.0f, 2.0f, vol);
}

void SyntheticObsidianProcessor::setTrackMuted (const juce::Uuid& id, bool muted)
{
    juce::ScopedLock lock (playerLock_);
    if (auto it = trackPlayers_.find (id); it != trackPlayers_.end() && it->second)
        it->second->muted = muted;
}

void SyntheticObsidianProcessor::setTrackSoloed (const juce::Uuid& id, bool soloed)
{
    juce::ScopedLock lock (playerLock_);
    if (auto it = trackPlayers_.find (id); it != trackPlayers_.end() && it->second)
        it->second->soloed = soloed;
}

void SyntheticObsidianProcessor::setTrackDetectedNotes (const juce::Uuid& id,
                                                        const std::vector<MidiNote>& notes)
{
    juce::ScopedLock lock (playerLock_);
    if (auto it = trackPlayers_.find (id); it != trackPlayers_.end() && it->second)
        it->second->detectedNotes = notes;
}

void SyntheticObsidianProcessor::setTrackMidiAuditionEnabled (const juce::Uuid& id, bool enabled)
{
    juce::ScopedLock lock (playerLock_);
    if (auto it = trackPlayers_.find (id); it != trackPlayers_.end() && it->second)
        it->second->midiAuditionEnabled = enabled;
}

//==============================================================================
// Offline render

int SyntheticObsidianProcessor::renderTracks (const juce::File& outputDir)
{
    // Called from a background thread — do NOT lock the audio thread lock here
    // instead snapshot the readers we need while briefly locking.
    struct RenderJob
    {
        juce::File                               srcFile;
        juce::String                             name;
    };

    std::vector<RenderJob> jobs;
    {
        juce::ScopedLock lock (playerLock_);
        for (auto& [id, player] : trackPlayers_)
        {
            if (!player || !player->reader || !player->file.existsAsFile()) continue;
            jobs.push_back ({ player->file,
                              player->file.getFileNameWithoutExtension() });
        }
    }

    if (jobs.empty()) return 0;

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    juce::WavAudioFormat wav;

    int filesWritten = 0;

    for (auto& job : jobs)
    {
        // Open a fresh reader for this file
        std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (job.srcFile));
        if (!reader) continue;

        const juce::int64  totalSamples = reader->lengthInSamples;
        const double       sr           = reader->sampleRate;
        const unsigned int nch          = (unsigned int)reader->numChannels;

        // Unique output name — avoid collisions
        auto outFile = outputDir.getChildFile (job.name + ".wav");
        {
            int suffix = 1;
            while (outFile.existsAsFile())
                outFile = outputDir.getChildFile (job.name + "_" + juce::String (++suffix) + ".wav");
        }

        auto outStream = std::make_unique<juce::FileOutputStream> (outFile);
        if (!outStream->openedOk()) continue;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (outStream.get(), sr, nch, 24, {}, 0));
        if (!writer) continue;
        outStream.release();   // writer takes ownership

        const int blockSize = 4096;
        juce::AudioBuffer<float> buf ((int)nch, blockSize);

        for (juce::int64 pos = 0; pos < totalSamples; pos += blockSize)
        {
            const int thisBlock = (int)juce::jmin ((juce::int64)blockSize, totalSamples - pos);
            reader->read (&buf, 0, thisBlock, pos, true, true);
            writer->writeFromAudioSampleBuffer (buf, 0, thisBlock);
        }

        ++filesWritten;
    }

    return filesWritten;
}

//==============================================================================
// Pitch correction

void SyntheticObsidianProcessor::applyPitchCorrection (
    const juce::Uuid&            trackId,
    const std::vector<MidiNote>& editedNotes,
    std::function<void()>        onComplete)
{
    // Build the correction list (only notes that differ from detected pitch)
    auto corrections = PitchCorrectionModule::buildCorrections (editedNotes);
    if (corrections.empty())
    {
        if (onComplete) juce::MessageManager::callAsync (onComplete);
        return;
    }

    // Grab the original (pre-correction) file path under lock
    juce::File srcFile;
    {
        juce::ScopedLock lock (playerLock_);
        auto it = trackPlayers_.find (trackId);
        if (it == trackPlayers_.end() || ! it->second) return;
        srcFile = it->second->originalFile;
    }
    if (! srcFile.existsAsFile()) return;

    // Everything below runs on a background thread
    juce::Thread::launch ([this, trackId, srcFile,
                           corr = std::move (corrections),
                           cb   = std::move (onComplete)]
    {
        // ── Read original audio ───────────────────────────────────────────────
        juce::AudioFormatManager fmt;
        fmt.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (srcFile));
        if (! reader) return;

        const int    nCh  = (int)reader->numChannels;
        const int    nSmp = (int)reader->lengthInSamples;
        const double sr   = reader->sampleRate;

        juce::AudioBuffer<float> buf (nCh, nSmp);
        reader->read (&buf, 0, nSmp, 0, true, true);
        reader.reset();

        // ── Apply phase-vocoder corrections ───────────────────────────────────
        // Lazy-start the WORLD bridge on first use (Python import takes ~1s).
        if (! worldResynthesizer_.isAvailable())
            worldResynthesizer_.startBridge();

        // Priority: WORLD (primary) → RVC (secondary) → phase vocoder (fallback).
        PitchCorrectionModule::applyOfflineCorrections (buf, sr, corr,
                                                        &worldResynthesizer_,
                                                        &rvcBridge_);

        // ── Write corrected audio to a temp WAV ───────────────────────────────
        auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("synobs_" + trackId.toString() + "_corrected.wav");
        tempFile.deleteFile();

        {
            auto outStream = std::make_unique<juce::FileOutputStream> (tempFile);
            if (! outStream->openedOk()) return;

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (outStream.get(), sr,
                                     (unsigned int)nCh, 24, {}, 0));
            if (! writer) return;
            outStream.release();   // writer owns it now

            writer->writeFromAudioSampleBuffer (buf, 0, nSmp);
        }   // writer flushed + closed here

        // ── Reload transport from corrected file (message thread) ─────────────
        juce::MessageManager::callAsync ([this, trackId, srcFile, tempFile, cb]
        {
            loadTrackAudio (trackId, tempFile);

            // loadTrackAudio sets both file and originalFile to tempFile.
            // Restore originalFile so the next drag-correction always starts
            // from the user's true source (not a previously corrected temp).
            {
                juce::ScopedLock lock (playerLock_);
                auto it = trackPlayers_.find (trackId);
                if (it != trackPlayers_.end() && it->second)
                    it->second->originalFile = srcFile;
            }

            if (cb) cb();
        });
    });
}

//==============================================================================
// Voice preset (RVC)

bool SyntheticObsidianProcessor::loadVoicePreset (
    const juce::File& modelFile,
    std::function<void (bool, const juce::String&)> onComplete)
{
    if (rvcBridge_.isAvailable()) return false;   // already loading / loaded

    juce::Thread::launch ([this, modelFile, cb = std::move (onComplete)]
    {
        const juce::String err = rvcBridge_.loadPreset (modelFile);
        const bool ok = err.isEmpty();

        juce::MessageManager::callAsync ([cb, ok, err]
        {
            if (cb) cb (ok, err);
        });
    });

    return true;
}

void SyntheticObsidianProcessor::unloadVoicePreset()
{
    rvcBridge_.unloadPreset();
}

//==============================================================================
// Seed-VC offline backing vocals

void SyntheticObsidianProcessor::renderSeedVCBackVocals (
    const juce::Uuid& trackId,
    std::function<void (bool, const SeedVCRenderedFiles&, const juce::String&)> onComplete)
{
    juce::File srcFile;
    {
        juce::ScopedLock lock (playerLock_);
        auto it = trackPlayers_.find (trackId);
        if (it == trackPlayers_.end() || ! it->second)
        {
            if (onComplete)
                juce::MessageManager::callAsync ([cb = std::move (onComplete)]
                {
                    cb (false, {}, "No audio is loaded on the selected track.");
                });
            return;
        }

        srcFile = it->second->originalFile;
    }

    if (! srcFile.existsAsFile())
    {
        if (onComplete)
            juce::MessageManager::callAsync ([cb = std::move (onComplete)]
            {
                cb (false, {}, "Selected track source file does not exist.");
            });
        return;
    }

    juce::Thread::launch ([this, srcFile, completion = std::move (onComplete)]
    {
        const auto baseDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                .getChildFile ("Synthetic Obsidian")
                                .getChildFile ("SeedVC");
        const auto safeName = srcFile.getFileNameWithoutExtension()
                                .retainCharacters ("abcdefghijklmnopqrstuvwxyz"
                                                   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                   "0123456789-_ ");
        const auto outputRoot = baseDir.getChildFile (
            safeName + "_" + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S"));

        const std::vector<SeedVCBridge::RenderSpec> specs {
            { "harmony_minor_third", 3,  BackVocalStyle::Third  },
            { "harmony_major_third", 4,  BackVocalStyle::Third  },
            { "harmony_fifth",       7,  BackVocalStyle::Fifth  },
            { "harmony_octave",      12, BackVocalStyle::Octave },
        };

        SeedVCRenderedFiles rendered;
        const auto error = seedVCBridge_.render (srcFile, outputRoot, specs, rendered);
        const bool ok = error.isEmpty();

        juce::MessageManager::callAsync ([done = std::move (completion), ok, rendered, error]
        {
            if (done) done (ok, rendered, error);
        });
    });
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SyntheticObsidianProcessor();
}
