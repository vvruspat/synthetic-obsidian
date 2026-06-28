#include "PluginEditor.h"
#include "PluginProcessor.h"

SyntheticObsidianEditor::SyntheticObsidianEditor (SyntheticObsidianProcessor& p)
    : juce::AudioProcessorEditor (&p),
      processor_    (p),
      trackManager_ (p.getProjectState()),
      pianoRoll_    (),
      transportBar_ ()
{
    setLookAndFeel (&lookAndFeel_);

    addAndMakeVisible (trackManager_);
    addAndMakeVisible (pianoRoll_);
    addAndMakeVisible (transportBar_);

    // ── Transport callbacks ────────────────────────────────────────────────────
    transportBar_.onPlay  = [this] { processor_.setPlaying (true);  };
    transportBar_.onStop  = [this] { processor_.setPlaying (false); };
    transportBar_.onReset = [this]
    {
        processor_.resetPlaybackPosition();
        pianoRoll_.setPlayheadPosition (0.0);
        pianoRoll_.setVisibleTimeRange (0.0, 8.0);
    };
    transportBar_.onBpmChanged = [this] (double bpm)
    {
        processor_.getProjectState().setDetectedBpm (bpm);
        pianoRoll_.setBpm (bpm);
    };

    transportBar_.onRenderVocal = [this]
    {
        fileChooser_ = std::make_unique<juce::FileChooser> (
            "Choose Output Folder",
            juce::File::getSpecialLocation (juce::File::userDesktopDirectory));

        fileChooser_->launchAsync (
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (! dir.isDirectory()) return;

                // Run blocking render on a background thread.
                // Capture a raw pointer to the processor (outlives the editor).
                juce::Thread::launch ([proc = &processor_, dir]
                {
                    const int n = proc->renderTracks (dir);

                    juce::MessageManager::callAsync ([n, dir]
                    {
                        juce::AlertWindow::showMessageBoxAsync (
                            juce::MessageBoxIconType::InfoIcon,
                            "Render Complete",
                            juce::String (n) + " file(s) written to:\n"
                                + dir.getFullPathName());
                    });
                });
            });
    };

    transportBar_.onRenderBacks = [this]
    {
        if (selectedTrackId_.isNull())
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                "AI Backs",
                "Select a vocal track first.");
            return;
        }

        pianoRoll_.setProcessing (true);
        juce::Component::SafePointer<SyntheticObsidianEditor> safeThis (this);

        processor_.renderSeedVCBackVocals (
            selectedTrackId_,
            [safeThis] (bool success,
                        const SyntheticObsidianProcessor::SeedVCRenderedFiles& files,
                        const juce::String& error)
            {
                if (safeThis == nullptr)
                    return;

                safeThis->pianoRoll_.setProcessing (false);

                if (! success)
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "AI Backs Failed",
                        error);
                    return;
                }

                for (const auto& file : files)
                {
                    const auto semitoneText = file.semitones > 0
                        ? "+" + juce::String (file.semitones)
                        : juce::String (file.semitones);

                    safeThis->trackManager_.addGeneratedAudioTrack (
                        "AI Back " + semitoneText,
                        file.style,
                        file.file,
                        "Seed-VC");
                }

                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon,
                    "AI Backs Ready",
                    juce::String ((int)files.size()) + " Seed-VC backing vocal track(s) added.");
            });
    };

    // ── Per-track Piano Roll: update on selection change or analysis completion ──
    trackManager_.onTrackChanged = [this] (const juce::Uuid& id,
                                           const std::vector<MidiNote>& notes,
                                           juce::AudioThumbnail* thumb)
    {
        selectedTrackId_ = id;            // remember for pitch-correction callbacks
        pianoRoll_.setAnalyzing (false);   // analysis done — hide spinner
        pianoRoll_.setAudioThumbnail (thumb);
        pianoRoll_.clearNotes();

        if (notes.empty())
        {
            pianoRoll_.setNoNotesPlaceholder (true);
        }
        else
        {
            pianoRoll_.setNoNotesPlaceholder (false);
            for (const auto& n : notes)
                pianoRoll_.addNote (n);

            // Scroll to first detected note
            const double firstStart = notes.front().startSeconds;
            const double viewDur    = pianoRoll_.getViewEndSec() - pianoRoll_.getViewStartSec();
            pianoRoll_.setVisibleTimeRange (juce::jmax (0.0, firstStart - 0.5),
                                            juce::jmax (0.0, firstStart - 0.5) + viewDur);
        }
    };

    // ── Analysis spinner in piano roll ────────────────────────────────────────
    trackManager_.onAnalyzingChanged = [this] (bool analysing)
    {
        pianoRoll_.setAnalyzing (analysing);
        if (analysing)
            pianoRoll_.setNoNotesPlaceholder (false);  // hide "no notes" while analysing
    };

    pianoRoll_.onDetectionSensitivityChanged = [this] (float sensitivity)
    {
        trackManager_.setDetectionSensitivity (sensitivity);
    };

    // ── Audio file → Processor playback engine ─────────────────────────────────
    trackManager_.onAudioLoaded = [this] (const juce::Uuid& id, const juce::File& file)
    {
        processor_.loadTrackAudio (id, file);
    };

    trackManager_.onTrackRemoved = [this] (const juce::Uuid& id)
    {
        processor_.removeTrackAudio (id);
    };

    // ── Pitch correction: note drag committed → phase vocoder ─────────────────
    pianoRoll_.onNotesEdited = [this] (const std::vector<MidiNote>& notes)
    {
        if (selectedTrackId_.isNull()) return;

        pianoRoll_.setProcessing (true);

        processor_.applyPitchCorrection (
            selectedTrackId_,
            notes,
            /* onComplete — fires on message thread after transport is reloaded */
            [this] { pianoRoll_.setProcessing (false); });
    };

    // ── Per-track mix controls → Processor ────────────────────────────────────
    trackManager_.onTrackVolumeChanged = [this] (const juce::Uuid& id, float vol)
    {
        processor_.setTrackVolume (id, vol);
    };

    trackManager_.onTrackMuteChanged = [this] (const juce::Uuid& id, bool muted)
    {
        processor_.setTrackMuted (id, muted);
    };

    trackManager_.onTrackSoloChanged = [this] (const juce::Uuid& id, bool soloed)
    {
        processor_.setTrackSoloed (id, soloed);
    };

    trackManager_.onMidiAuditionChanged = [this] (const juce::Uuid& id, bool enabled)
    {
        processor_.setTrackMidiAuditionEnabled (id, enabled);
    };

    trackManager_.onTrackNotesChanged = [this] (const juce::Uuid& id,
                                                const std::vector<MidiNote>& notes)
    {
        processor_.setTrackDetectedNotes (id, notes);
    };

    // Seed BPM display from project state
    const double initBpm = p.getProjectState().getDetectedBpm();
    transportBar_.setBpm (initBpm);
    pianoRoll_.setBpm    (initBpm);

    // Seed piano roll preset state
    pianoRoll_.setHasVoicePreset (processor_.hasVoicePreset());

    setSize (kWindowWidth, kWindowHeight);
    setResizable (true, true);
    setResizeLimits (900, 600, 2560, 1600);

    startTimerHz (30);
}

SyntheticObsidianEditor::~SyntheticObsidianEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SyntheticObsidianEditor::timerCallback()
{
    const double pos = processor_.getPlaybackPosition();

    // ── Playhead ───────────────────────────────────────────────────────────────
    pianoRoll_.setPlayheadPosition (pos);

    // ── Auto-scroll: page forward when playhead enters final 15% of view ──────
    if (processor_.isPlayingNow())
    {
        const double viewStart = pianoRoll_.getViewStartSec();
        const double viewEnd   = pianoRoll_.getViewEndSec();
        const double viewDur   = viewEnd - viewStart;
        const double threshold = viewEnd - viewDur * 0.15;

        if (pos >= threshold)
            pianoRoll_.setVisibleTimeRange (pos - viewDur * 0.1,
                                            pos + viewDur * 0.9);
    }

    // ── CPU load bar ───────────────────────────────────────────────────────────
    transportBar_.setCpuLoad (processor_.getCpuLoad());

    // (RVC preset status sync kept for back-vocal UI — Phase 2)

    // ── Header repaint (key may have changed after guide analysis) ─────────────
    repaint (0, 0, getWidth(), kHeaderHeight);
}

//==============================================================================
void SyntheticObsidianEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (Theme::kBackground));

    // ── Header ────────────────────────────────────────────────────────────────
    g.setColour (juce::Colour (0xff12121e));
    g.fillRect  (0, 0, getWidth(), kHeaderHeight);

    // Logo
    g.setColour (juce::Colour (Theme::kAccentCyan));
    g.setFont   (juce::FontOptions (20.0f).withStyle ("Bold"));
    g.drawText  ("SYNTHETIC OBSIDIAN",
                 14, 0, 320, kHeaderHeight,
                 juce::Justification::centredLeft, true);

    // Key / BPM / time-sig — right-aligned in header
    const auto& ps  = processor_.getProjectState();
    const juce::String infoText =
        ps.getDetectedKey()
        + "   \xe2\x80\xa2   "                    // bullet separator
        + juce::String ((int)ps.getDetectedBpm()) + " BPM"
        + "   \xe2\x80\xa2   "
        + juce::String (ps.getTimeSigNum()) + "/" + juce::String (ps.getTimeSigDen());

    g.setColour (juce::Colour (Theme::kTextSecondary));
    g.setFont   (juce::FontOptions (12.0f).withStyle ("Bold"));
    g.drawText  (infoText,
                 340, 0, getWidth() - 360, kHeaderHeight,
                 juce::Justification::centredRight, true);

    // ── Separators ────────────────────────────────────────────────────────────
    const auto sepColour = juce::Colour (Theme::kBorder);

    // Below header
    g.setColour (sepColour);
    g.drawHorizontalLine (kHeaderHeight,
                          0.0f, (float)getWidth());

    // Between track panel and piano roll
    g.drawVerticalLine (kTrackPanelWidth,
                        (float)kHeaderHeight,
                        (float)(getHeight() - kTransportH));

    // Above transport
    g.drawHorizontalLine (getHeight() - kTransportH,
                          0.0f, (float)getWidth());
}

void SyntheticObsidianEditor::resized()
{
    const int contentTop    = kHeaderHeight;
    const int contentHeight = getHeight() - kHeaderHeight - kTransportH;

    trackManager_.setBounds (0, contentTop,
                              kTrackPanelWidth, contentHeight);

    pianoRoll_.setBounds    (kTrackPanelWidth, contentTop,
                              getWidth() - kTrackPanelWidth, contentHeight);

    transportBar_.setBounds (0, getHeight() - kTransportH,
                              getWidth(), kTransportH);
}
