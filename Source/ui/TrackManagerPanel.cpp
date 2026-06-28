#include "TrackManagerPanel.h"

//==============================================================================
namespace
{
    juce::Colour trackTypeColour (TrackType t)
    {
        switch (t)
        {
            case TrackType::Guide:   return juce::Colour (Theme::kAccentPurple);
            case TrackType::MainVox: return juce::Colour (Theme::kAccentCyan);
            case TrackType::BackVox: return juce::Colour (Theme::kSuccess);
        }
        return juce::Colour (Theme::kSuccess);
    }
}

//==============================================================================
// TrackRow
//==============================================================================

TrackRow::TrackRow (const TrackModel& model)
    : model_ (model)
{
    // ── M / S buttons ──────────────────────────────────────────────────────────
    auto setupToggleBtn = [] (juce::TextButton& btn, uint32_t onColour)
    {
        btn.setClickingTogglesState (true);
        btn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff1e1e1e));
        btn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (onColour));
        btn.setColour (juce::TextButton::textColourOffId,
                       juce::Colour (Theme::kTextSecondary));
        btn.setColour (juce::TextButton::textColourOnId,
                       juce::Colour (Theme::kBackground));
    };

    setupToggleBtn (muteBtn_, Theme::kWarning);
    setupToggleBtn (soloBtn_, Theme::kAccentCyan);
    setupToggleBtn (midiBtn_, Theme::kSuccess);

    muteBtn_.setToggleState (model_.isMuted, juce::dontSendNotification);
    soloBtn_.setToggleState (model_.isSolo,  juce::dontSendNotification);

    // Use onClick (fires after click finishes) — safe even if panel rebuilds rows
    muteBtn_.onClick = [this]
    {
        model_.isMuted = muteBtn_.getToggleState();
        if (onMuteChanged) onMuteChanged (model_.isMuted);
    };
    soloBtn_.onClick = [this]
    {
        model_.isSolo = soloBtn_.getToggleState();
        if (onSoloChanged) onSoloChanged (model_.isSolo);
    };
    midiBtn_.onClick = [this]
    {
        if (onMidiAuditionChanged) onMidiAuditionChanged (midiBtn_.getToggleState());
    };

    addAndMakeVisible (muteBtn_);
    addAndMakeVisible (soloBtn_);
    addAndMakeVisible (midiBtn_);

    // ── Volume slider ──────────────────────────────────────────────────────────
    volumeSlider_.setSliderStyle  (juce::Slider::LinearHorizontal);
    volumeSlider_.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    volumeSlider_.setRange        (0.0, 1.0, 0.0);
    volumeSlider_.setValue        (model_.volume, juce::dontSendNotification);
    volumeSlider_.setColour (juce::Slider::trackColourId,
                             juce::Colour (Theme::kAccentCyan).withAlpha (0.6f));
    volumeSlider_.setColour (juce::Slider::backgroundColourId,
                             juce::Colour (Theme::kBorder).withAlpha (0.4f));
    volumeSlider_.setColour (juce::Slider::thumbColourId,
                             juce::Colour (Theme::kAccentCyan));
    volumeSlider_.onValueChange = [this]
    {
        model_.volume = (float)volumeSlider_.getValue();
        if (onVolumeChanged) onVolumeChanged (model_.volume);
    };
    addAndMakeVisible (volumeSlider_);

    // ── Inline name editor ─────────────────────────────────────────────────────
    nameEditor_.setMultiLine             (false);
    nameEditor_.setReturnKeyStartsNewLine (false);
    nameEditor_.setScrollbarsShown       (false);
    nameEditor_.setColour (juce::TextEditor::backgroundColourId,
                           juce::Colour (Theme::kSurface).brighter (0.1f));
    nameEditor_.setColour (juce::TextEditor::textColourId,
                           juce::Colour (Theme::kTextPrimary));
    nameEditor_.setColour (juce::TextEditor::outlineColourId,
                           juce::Colour (Theme::kAccentCyan));
    nameEditor_.setFont (juce::FontOptions (13.0f).withStyle ("Bold"));

    auto commitEdit = [this]
    {
        model_.name = nameEditor_.getText();
        if (onNameChanged) onNameChanged (model_.name);
        nameEditor_.setVisible (false);
        repaint();
    };
    nameEditor_.onReturnKey = commitEdit;
    nameEditor_.onFocusLost = commitEdit;

    addChildComponent (nameEditor_);
    setSize (260, kRowH);
}

//==============================================================================
void TrackRow::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // ── Background ────────────────────────────────────────────────────────────
    g.setColour (isSelected_ ? juce::Colour (Theme::kSurface).brighter (0.10f)
                             : juce::Colour (Theme::kSurface));
    g.fillRect  (bounds);

    // ── Left accent bar ───────────────────────────────────────────────────────
    g.setColour (trackTypeColour (model_.type).withAlpha (isSelected_ ? 1.0f : 0.55f));
    g.fillRect  (bounds.removeFromLeft (kAccentBarW).toFloat());

    // ── Border ────────────────────────────────────────────────────────────────
    g.setColour (isSelected_ ? juce::Colour (Theme::kAccentCyan).withAlpha (0.4f)
                             : juce::Colour (Theme::kBorder));
    g.drawRect  (getLocalBounds(), 1);

    const int rightGap = kBtnSize * 3 + 12;

    // ── Track name ────────────────────────────────────────────────────────────
    if (! nameEditor_.isVisible())
    {
        auto nameArea = bounds.withLeft (8).withTop (7)
                              .withHeight (17).withRight (getWidth() - rightGap);
        g.setColour (juce::Colour (Theme::kTextPrimary));
        g.setFont   (juce::FontOptions (13.0f).withStyle ("Bold"));
        g.drawText  (model_.name, nameArea, juce::Justification::centredLeft, true);
    }

    // ── Waveform OR subtitle ──────────────────────────────────────────────────
    auto midArea = bounds.withLeft (8).withTop (26)
                         .withHeight (22).withRight (getWidth() - rightGap);

    if (thumbnail_ != nullptr && thumbnail_->getTotalLength() > 0.0)
    {
        g.setColour (trackTypeColour (model_.type).withAlpha (0.5f));
        thumbnail_->drawChannels (g, midArea, 0.0, thumbnail_->getTotalLength(), 0.8f);
    }
    else if (isAnalyzing_)
    {
        g.setColour (juce::Colour (Theme::kTextSecondary));
        g.setFont   (juce::FontOptions (10.0f));
        g.drawText  ("analysing...", midArea, juce::Justification::centredLeft, true);
    }
    else
    {
        juce::String subtitle = model_.voicePresetName.isEmpty()
                                ? "No preset" : model_.voicePresetName;
        g.setColour (juce::Colour (Theme::kTextSecondary));
        g.setFont   (juce::FontOptions (10.0f));
        g.drawText  (subtitle, midArea, juce::Justification::centredLeft, true);
    }

    // ── Green dot when audio file is loaded ───────────────────────────────────
    if (model_.audioFile.existsAsFile())
    {
        g.setColour (juce::Colour (Theme::kSuccess).withAlpha (0.9f));
        g.fillEllipse ((float)(kAccentBarW + 5),
                       (float)(getHeight() - 20), 5.0f, 5.0f);
    }

    // ── Drag-over overlay ─────────────────────────────────────────────────────
    if (isDragOver_)
    {
        g.setColour (juce::Colour (Theme::kAccentCyan).withAlpha (0.18f));
        g.fillRect  (getLocalBounds());
        g.setColour (juce::Colour (Theme::kAccentCyan));
        g.drawRect  (getLocalBounds(), 2);
        g.setFont   (juce::FontOptions (11.0f).withStyle ("Bold"));
        g.setColour (juce::Colour (Theme::kAccentCyan));
        g.drawText  ("DROP AUDIO", getLocalBounds(), juce::Justification::centred, true);
    }
}

void TrackRow::resized()
{
    const int right = getWidth() - 4;
    muteBtn_.setBounds (right - kBtnSize,         6, kBtnSize, kBtnSize);
    soloBtn_.setBounds (right - kBtnSize * 2 - 2, 6, kBtnSize, kBtnSize);
    midiBtn_.setBounds (right - kBtnSize * 3 - 4, 6, kBtnSize, kBtnSize);

    volumeSlider_.setBounds (kAccentBarW + 4, getHeight() - 16,
                             getWidth() - kAccentBarW - 8, 13);
}

//==============================================================================
void TrackRow::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown()) { showContextMenu(); return; }
    if (onSelected) onSelected();
}

void TrackRow::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto nameArea = getLocalBounds().withLeft (kAccentBarW).withTop (0).withHeight (28);
    if (nameArea.contains (e.getPosition())) startNameEdit();
}

//==============================================================================
void TrackRow::showContextMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Voice");
    menu.addItem (2, "Style");
    menu.addSeparator();
    menu.addItem (5, "Load Audio...");
    menu.addSeparator();
    menu.addItem (3, "Duplicate");
    menu.addItem (4, "Remove");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [this] (int result)
        {
            switch (result)
            {
                case 1: if (onVoiceClicked)        onVoiceClicked();        break;
                case 2: if (onStyleClicked)        onStyleClicked();        break;
                case 3: if (onDuplicate)           onDuplicate();           break;
                case 4: if (onRemove)              onRemove();              break;
                case 5: if (onLoadAudioRequested)  onLoadAudioRequested();  break;
                default: break;
            }
        });
}

void TrackRow::startNameEdit()
{
    const int rightGap = kBtnSize * 3 + 14;
    nameEditor_.setBounds (kAccentBarW + 6, 5,
                           getWidth() - kAccentBarW - rightGap - 6, 20);
    nameEditor_.setText   (model_.name, false);
    nameEditor_.setVisible (true);
    nameEditor_.grabKeyboardFocus();
    nameEditor_.selectAll();
}

void TrackRow::loadAudioFile (const juce::File& f)
{
    model_.audioFile = f;
    if (model_.voicePresetName.isEmpty())
        model_.voicePresetName = f.getFileNameWithoutExtension();
    if (onAudioFileChanged) onAudioFileChanged (f);
    repaint();
}

void TrackRow::setAudioFile (const juce::File& f)
{
    model_.audioFile = f;
    if (model_.voicePresetName.isEmpty())
        model_.voicePresetName = f.getFileNameWithoutExtension();
    repaint();
}

//==============================================================================
// FileDragAndDropTarget

bool TrackRow::isInterestedInFileDrag (const juce::StringArray& files)
{
    static const juce::StringArray kExts { ".wav",".aiff",".aif",".mp3",".flac",".ogg",".m4a" };
    for (const auto& p : files)
        if (kExts.contains (juce::File (p).getFileExtension().toLowerCase()))
            return true;
    return false;
}

void TrackRow::fileDragEnter (const juce::StringArray&, int, int)
{
    isDragOver_ = true;  repaint();
}

void TrackRow::fileDragExit (const juce::StringArray&)
{
    isDragOver_ = false; repaint();
}

void TrackRow::filesDropped (const juce::StringArray& files, int, int)
{
    isDragOver_ = false;
    for (const auto& p : files)
        if (isInterestedInFileDrag ({ p })) { loadAudioFile (juce::File (p)); break; }
}

//==============================================================================
void TrackRow::setSelected (bool s) { if (isSelected_ != s) { isSelected_ = s; repaint(); } }
void TrackRow::updateModel (const TrackModel& m) { model_ = m; repaint(); }
void TrackRow::setThumbnail (juce::AudioThumbnail* t) { thumbnail_ = t; repaint(); }
void TrackRow::setMidiAuditionEnabled (bool enabled)
{
    midiBtn_.setToggleState (enabled, juce::dontSendNotification);
}

//==============================================================================
//==============================================================================
// TrackManagerPanel
//==============================================================================

TrackManagerPanel::TrackManagerPanel (ProjectState& state)
    : state_ (state)
{
    state_.addChangeListener (this);

    addAndMakeVisible (addTrackBtn_);
    addTrackBtn_.onClick = [this]
    {
        state_.addTrack (TrackModel::makeBackVox (
            "Back " + juce::String (state_.getTracks().size())));
    };

    rebuildTrackRows();
    setSize (260, 600);
}

TrackManagerPanel::~TrackManagerPanel()
{
    for (auto& [id, thumb] : thumbnails_)
        thumb->removeChangeListener (this);

    state_.removeChangeListener (this);
}

//==============================================================================
void TrackManagerPanel::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (Theme::kBackground));
    g.fillRect  (getLocalBounds());

    auto header = getLocalBounds().removeFromTop (kHeaderH);
    g.setColour (juce::Colour (Theme::kSurface));
    g.fillRect  (header);
    g.setColour (juce::Colour (Theme::kTextPrimary));
    g.setFont   (juce::FontOptions (11.0f).withStyle ("Bold"));
    g.drawText  ("TRACKS", header.reduced (10, 0), juce::Justification::centredLeft, true);
    g.setColour (juce::Colour (Theme::kBorder));
    g.drawHorizontalLine (kHeaderH, 0.0f, (float)getWidth());
}

void TrackManagerPanel::resized()
{
    int y = kHeaderH;
    for (auto* row : trackRows_)
    {
        row->setBounds (0, y, getWidth(), kRowH);
        y += kRowH;
    }
    addTrackBtn_.setBounds (0, getHeight() - 36, getWidth(), 36);
}

//==============================================================================
void TrackManagerPanel::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    // Check whether the change came from a thumbnail or from ProjectState
    for (auto& [id, thumb] : thumbnails_)
    {
        if (source == thumb.get())
        {
            // Just repaint rows so the waveform appears; notes are managed
            // by selectTrack() and analysis completion only.
            for (auto* row : trackRows_) row->repaint();
            return;
        }
    }

    // ProjectState change → rebuild rows
    rebuildTrackRows();
}

//==============================================================================
void TrackManagerPanel::rebuildTrackRows()
{
    trackRows_.clear();

    for (const auto& track : state_.getTracks())
    {
        auto* row = new TrackRow (track);
        trackRows_.add (row);
        addAndMakeVisible (row);

        if (track.id == selectedId_)
            row->setSelected (true);
        row->setMidiAuditionEnabled (midiAuditionTracks_.count (track.id) > 0);

        if (auto it = thumbnails_.find (track.id); it != thumbnails_.end())
            row->setThumbnail (it->second.get());

        // ── Callbacks ───────────────────────────────────────────────────────
        row->onSelected = [this, trackId = track.id]
            { selectTrack (trackId); };

        row->onNameChanged = [this, trackId = track.id] (const juce::String& n)
        {
            if (auto* t = state_.findTrack (trackId)) { t->name = n; state_.updateTrack (*t); }
        };

        row->onRemove = [this, trackId = track.id]
        {
            thumbnails_.erase (trackId);
            trackNotes_.erase (trackId);
            if (onTrackRemoved) onTrackRemoved (trackId);
            state_.removeTrack (trackId);
            if (selectedId_ == trackId)
            {
                selectedId_ = {};
                if (onTrackChanged) onTrackChanged ({}, {}, nullptr);
            }
        };

        row->onDuplicate = [this, trackId = track.id]
        {
            if (auto* t = state_.findTrack (trackId))
            {
                auto copy  = *t;
                copy.id    = juce::Uuid();
                copy.name += " (copy)";
                state_.addTrack (copy);
            }
        };

        row->onLoadAudioRequested = [this, trackId = track.id]
            { openFileChooserForTrack (trackId); };

        row->onAudioFileChanged = [this, trackId = track.id] (const juce::File& f)
        {
            if (auto* t = state_.findTrack (trackId)) { t->audioFile = f; state_.updateTrack (*t); }
            triggerAnalysis (trackId, f);
            if (onAudioLoaded) onAudioLoaded (trackId, f);
        };

        row->onVolumeChanged = [this, trackId = track.id] (float v)
        {
            // Silent field update — avoids triggering rebuildTrackRows while
            // the row's slider callback is still on the call stack.
            if (auto* t = state_.findTrack (trackId)) t->volume = v;
            if (onTrackVolumeChanged) onTrackVolumeChanged (trackId, v);
        };

        row->onMuteChanged = [this, trackId = track.id] (bool m)
        {
            if (auto* t = state_.findTrack (trackId)) t->isMuted = m;
            if (onTrackMuteChanged) onTrackMuteChanged (trackId, m);
        };

        row->onSoloChanged = [this, trackId = track.id] (bool s)
        {
            if (auto* t = state_.findTrack (trackId)) t->isSolo = s;
            if (onTrackSoloChanged) onTrackSoloChanged (trackId, s);
        };

        row->onMidiAuditionChanged = [this, trackId = track.id] (bool enabled)
        {
            if (enabled)
                midiAuditionTracks_.insert (trackId);
            else
                midiAuditionTracks_.erase (trackId);

            if (onMidiAuditionChanged)
                onMidiAuditionChanged (trackId, enabled);
        };

        row->onVoiceClicked = []
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                "Voice", "Voice preset picker - Phase 2");
        };

        row->onStyleClicked = []
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                "Style", "Back vocal style picker - Phase 2");
        };
    }

    resized();
    repaint();
}

//==============================================================================
void TrackManagerPanel::selectTrack (const juce::Uuid& id)
{
    selectedId_ = id;
    for (auto* row : trackRows_)
        row->setSelected (row->getTrackId() == id);

    // Tell piano roll whether this track is currently being analysed
    if (onAnalyzingChanged)
        onAnalyzingChanged (analyzingTracks_.count (id) > 0);

    if (onTrackChanged)
    {
        auto* thumb = thumbnails_.count (id) ? thumbnails_.at (id).get() : nullptr;
        const auto& notes = trackNotes_.count (id)
                            ? trackNotes_.at (id) : std::vector<MidiNote>{};
        onTrackChanged (id, notes, thumb);
    }
}

//==============================================================================
void TrackManagerPanel::triggerAnalysis (const juce::Uuid& trackId, const juce::File& f)
{
    auto& thumb = thumbnails_[trackId];
    if (thumb == nullptr)
    {
        thumb = std::make_unique<juce::AudioThumbnail> (
            512, analysisEngine_.getFormatManager(), thumbnailCache_);
        thumb->addChangeListener (this);
    }

    thumb->setSource (new juce::FileInputSource (f));

    for (auto* row : trackRows_)
        if (row->getTrackId() == trackId)
            row->setThumbnail (thumb.get());

    setTrackAnalyzing (trackId, true);

    analysisEngine_.analyzeFile (trackId, f,
        [this, trackId] (const juce::Uuid& id, std::vector<MidiNote> notes)
        {
            // Fires on message thread
            setTrackAnalyzing (id, false);
            trackNotes_[id] = std::move (notes);
            if (onTrackNotesChanged)
                onTrackNotesChanged (id, trackNotes_[id]);

            // Update piano roll only if this is the selected track
            if (id == selectedId_ && onTrackChanged)
            {
                auto* selectedThumb = thumbnails_.count (id) ? thumbnails_.at (id).get() : nullptr;
                onTrackChanged (id, trackNotes_[id], selectedThumb);
            }
        });
}

//==============================================================================
void TrackManagerPanel::openFileChooserForTrack (const juce::Uuid& trackId)
{
    fileChooser_ = std::make_unique<juce::FileChooser> (
        "Select Audio File",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.aiff;*.aif;*.mp3;*.flac;*.ogg;*.m4a");

    fileChooser_->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles,
        [this, trackId] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;

            // Update state
            if (auto* t = state_.findTrack (trackId))
            {
                t->audioFile = f;
                if (t->voicePresetName.isEmpty())
                    t->voicePresetName = f.getFileNameWithoutExtension();
                state_.updateTrack (*t);
            }

            // Update row display (state_.updateTrack will rebuild rows via change listener,
            // but also update immediately so the green dot appears without waiting)
            for (auto* row : trackRows_)
                if (row->getTrackId() == trackId)
                    row->setAudioFile (f);

            // Trigger analysis + thumbnail
            triggerAnalysis (trackId, f);

            // Notify processor for playback
            if (onAudioLoaded) onAudioLoaded (trackId, f);
        });
}

//==============================================================================
void TrackManagerPanel::setTrackAnalyzing (const juce::Uuid& id, bool analysing)
{
    if (analysing)
        analyzingTracks_.insert (id);
    else
        analyzingTracks_.erase (id);

    for (auto* row : trackRows_)
        if (row->getTrackId() == id)
            row->setAnalyzing (analysing);

    // Notify piano roll if this affects the currently selected track
    if (id == selectedId_ && onAnalyzingChanged)
        onAnalyzingChanged (analysing);
}

void TrackManagerPanel::setDetectionSensitivity (float sensitivity)
{
    detectionSensitivity_ = juce::jlimit (0.0f, 1.0f, sensitivity);
    analysisEngine_.setSensitivity (detectionSensitivity_);

    if (selectedId_.isNull())
        return;

    if (auto* track = state_.findTrack (selectedId_);
        track != nullptr && track->audioFile.existsAsFile())
        triggerAnalysis (selectedId_, track->audioFile);
}

juce::Uuid TrackManagerPanel::addGeneratedAudioTrack (const juce::String& name,
                                                      BackVocalStyle style,
                                                      const juce::File& file,
                                                      const juce::String& voicePresetName)
{
    auto track = TrackModel::makeBackVox (name, style);
    track.audioFile = file;
    track.voicePresetName = voicePresetName;
    track.volume = 0.75f;

    const auto id = track.id;
    state_.addTrack (track);

    triggerAnalysis (id, file);
    if (onAudioLoaded)
        onAudioLoaded (id, file);

    selectTrack (id);
    return id;
}
