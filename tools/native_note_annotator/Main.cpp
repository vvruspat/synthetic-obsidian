#include "DatasetExport.h"
#include "NoteBoundaryModel.h"
#include "WaveformEditor.h"

#include "AnnotationJson.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

namespace native_annotation
{
namespace UIConstants
{
constexpr int analysisModeWidth = 180;
constexpr int modelSelectorWidth = 300;
constexpr int modelBrowseWidth = 100;
constexpr int generateDraftWidth = 150;
}

struct LoadedClip
{
    juce::AudioBuffer<float> audio;
    vocal_annotation::AnnotationDocument document;
    NoteAnalysis analysis;
    DatasetMetadata metadata;
    juce::File projectFile;
};

class MainComponent final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private juce::AudioIODeviceCallback,
                            private juce::AsyncUpdater,
                            private juce::Timer
{
    struct NoteSnapshot
    {
        std::vector<vocal_annotation::NoteBlock> notes;
        juce::String modelId, modelHash;
    };
public:
    MainComponent() : editor_(document_, analysis_)
    {
        setSize(1180, 680);
        setWantsKeyboardFocus(true);
        for (auto* button : { &open_, &play_, &save_, &saveAs_, &export_, &undo_, &redo_, &fit_, &zoomIn_, &zoomOut_ })
            addAndMakeVisible(button);
        for (auto* field : { &singer_, &session_ })
        {
            addAndMakeVisible(field);
            field->setMultiLine(false);
            field->onTextChange = [this] { markDirty(false); };
        }
        singer_.setTextToShowWhenEmpty("Singer ID (for dataset splits)", juce::Colour(0xff9eabb9));
        session_.setTextToShowWhenEmpty("Song / recording session ID", juce::Colour(0xff9eabb9));
        addAndMakeVisible(reviewed_);
        reviewed_.onClick = [this] { markDirty(false); };
        addAndMakeVisible(mode_);
        addAndMakeVisible(modelChoices_);
        addAndMakeVisible(modelButton_);
        addAndMakeVisible(draftButton_);
        addAndMakeVisible(modelLabel_);
        mode_.addItem("Math (YIN + energy)", 1);
        mode_.addItem("AI note boundaries", 2);
        juce::PropertiesFile::Options options;
        options.applicationName = "NativeNoteAnnotator";
        options.folderName = "SyntheticObsidian";
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        preferences_ = std::make_unique<juce::PropertiesFile>(options);
        selectedModel_ = juce::File(preferences_->getValue("noteModelPath", defaultModel().getFullPathName()));
        refreshModelChoices();
        mode_.setSelectedId(preferences_->getBoolValue("useNoteModel", selectedModel_.existsAsFile()) ? 2 : 1, juce::dontSendNotification);
        mode_.onChange = [this]
        {
            preferences_->setValue("useNoteModel", mode_.getSelectedId() == 2);
            preferences_->saveIfNeeded();
            updateModelLabel();
        };
        modelChoices_.onChange = [this]
        {
            const auto index = modelChoices_.getSelectedItemIndex();
            if (! busy_ && juce::isPositiveAndBelow(index, static_cast<int>(modelFiles_.size())))
                selectModel(modelFiles_[static_cast<size_t>(index)]);
        };
        modelButton_.onClick = [this] { chooseModel(); };
        draftButton_.onClick = [this] { generateDraft(); };
        draftButton_.setTooltip("Generate a new draft in a separate sidecar. Original annotations are kept; Undo restores the notes.");
        updateModelLabel();
        addAndMakeVisible(editor_);
        addAndMakeVisible(status_);
        addAndMakeVisible(help_);
        status_.setText("Open a dry, isolated vocal to create a first-pass draft.", juce::dontSendNotification);
        help_.setText("Cmd/Ctrl + click: start or split note   |   Drag edges: timing   |   Click note: listen   |   Space: play/stop   |   Up/Down: pitch   |   M: merge   |   Delete: remove", juce::dontSendNotification);
        help_.setFont(juce::FontOptions(12.0f));
        open_.onClick = [this] { chooseAudio(); };
        play_.onClick = [this] { togglePlayback(); };
        save_.onClick = [this] { save(false); };
        saveAs_.onClick = [this] { chooseSaveAs(); };
        export_.onClick = [this] { chooseExport(); };
        undo_.onClick = [this] { undo(false); };
        redo_.onClick = [this] { undo(true); };
        fit_.onClick = [this] { editor_.fit(); };
        zoomIn_.onClick = [this] { editor_.zoom(1.5); };
        zoomOut_.onClick = [this] { editor_.zoom(1.0 / 1.5); };
        editor_.beginEdit = [this]
        {
            undo_.setEnabled(true);
            undoStack_.push_back({ document_.notes, metadata_.modelId, metadata_.modelHash });
            if (undoStack_.size() > 100) undoStack_.erase(undoStack_.begin());
            redoStack_.clear();
        };
        editor_.changed = [this] { markDirty(true); };
        editor_.seek = [this](double time)
        {
            playing_.store(false);
            seekRequest_.store(time);
            displayPosition_.store(time);
            editor_.setPlayhead(time);
        };
        editor_.audition = [this](double start, double end) { startPlayback(start, end); };
        editor_.shortcut = [this](const auto& key) { return keyPressed(key); };
        updateControls();
        startTimerHz(30);
    }

    ~MainComponent() override
    {
        stopTimer();
        cancelled_.store(true);
        pool_.removeAllJobs(true, -1);
        cancelPendingUpdate();
        devices_.removeAudioCallback(this);
        devices_.closeAudioDevice();
    }

    bool requestClose()
    {
        if (busy_)
        {
            status_.setText("Finishing the current operation. Close when it completes.", juce::dontSendNotification);
            return false;
        }
        if (dirty_)
        {
            save(false);
            closeAfterSave_ = true;
            return false;
        }
        return true;
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override { return files.size() == 1; }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        if (! busy_ && files.size() == 1) openFile(juce::File(files[0]));
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff16202b)); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(14);
        auto toolbar = area.removeFromTop(34);
        for (auto* button : { &open_, &play_, &save_, &saveAs_, &export_, &undo_, &redo_, &fit_, &zoomOut_, &zoomIn_ })
        {
            button->setBounds(toolbar.removeFromLeft(button == &export_ ? 130 : 84).reduced(3, 0));
        }
        area.removeFromTop(10);
        auto metadata = area.removeFromTop(30);
        singer_.setBounds(metadata.removeFromLeft(270).reduced(3, 0));
        session_.setBounds(metadata.removeFromLeft(270).reduced(3, 0));
        reviewed_.setBounds(metadata.removeFromLeft(310));
        area.removeFromTop(10);
        auto analysis = area.removeFromTop(30);
        mode_.setBounds(analysis.removeFromLeft(UIConstants::analysisModeWidth).reduced(3, 0));
        modelChoices_.setBounds(analysis.removeFromLeft(UIConstants::modelSelectorWidth).reduced(3, 0));
        modelButton_.setBounds(analysis.removeFromLeft(UIConstants::modelBrowseWidth).reduced(3, 0));
        draftButton_.setBounds(analysis.removeFromLeft(UIConstants::generateDraftWidth).reduced(3, 0));
        modelLabel_.setBounds(analysis);
        status_.setBounds(area.removeFromBottom(28));
        help_.setBounds(area.removeFromBottom(26));
        editor_.setBounds(area.reduced(0, 8));
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::spaceKey) { togglePlayback(); return true; }
        if (key.getModifiers().isCommandDown())
        {
            if (key.getKeyCode() == 'Z') { undo(key.getModifiers().isShiftDown()); return true; }
            if (key.getKeyCode() == 'S')
            {
                if (key.getModifiers().isShiftDown()) chooseSaveAs();
                else save(false);
                return true;
            }
            if (key.getKeyCode() == 'O') { chooseAudio(); return true; }
        }
        return false;
    }

private:
    struct Completion
    {
        std::shared_ptr<LoadedClip> loaded;
        std::optional<NoteDraftResult> draft;
        juce::String error;
        juce::String message;
        bool saved = false;
        bool autosave = false;
        juce::uint64 revision = 0;
    };

    void post(Completion result)
    {
        {
            const std::scoped_lock lock(completionMutex_);
            completion_ = std::move(result);
        }
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        Completion result;
        {
            const std::scoped_lock lock(completionMutex_);
            result = std::move(completion_);
        }
        busy_ = false;
        if (result.loaded != nullptr)
        {
            // Detach before replacing the immutable playback buffer. Destruction stays on the UI thread.
            devices_.removeAudioCallback(this);
            clip_ = std::move(result.loaded);
            document_ = clip_->document;
            analysis_ = clip_->analysis;
            metadata_ = clip_->metadata;
            projectFile_ = clip_->projectFile;
            editor_.setAudio(clip_->audio);
            singer_.setText(metadata_.singer, false);
            session_.setText(metadata_.session, false);
            reviewed_.setToggleState(metadata_.reviewed, juce::dontSendNotification);
            dirty_ = false;
            revision_ = 0;
            undoStack_.clear();
            redoStack_.clear();
            playing_.store(false);
            seekRequest_.store(0.0);
            displayPosition_.store(0.0);
            if (devices_.getCurrentAudioDevice() == nullptr)
                audioError_ = devices_.initialise(0, 2, nullptr, true);
            if (audioError_.isEmpty()) devices_.addAudioCallback(this);
        }
        if (result.draft)
        {
            if (editor_.beginEdit) editor_.beginEdit();
            analysis_ = std::move(result.draft->analysis);
            document_.notes = analysis_.notes;
            metadata_.modelId = result.draft->modelId;
            metadata_.modelHash = result.draft->modelHash;
            // Generating a draft never autosaves over the user's existing gold annotation.
            projectFile_ = projectFile_.getParentDirectory().getNonexistentChildFile(
                document_.audioFile.getFileNameWithoutExtension() + ".proposal", ".annotation.json", true);
            editor_.clearSelection();
            markDirty(true);
        }
        if (result.saved && result.revision == revision_)
        {
            dirty_ = false;
            autosaveFailed_ = false;
        }
        if (result.error.isNotEmpty())
        {
            closeAfterSave_ = false;
            if (dirty_) autosaveFailed_ = true;
            pendingOpen_ = juce::File();
            status_.setText(result.error, juce::dontSendNotification);
            if (! result.autosave)
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    "Vocal Note Annotator", result.error);
        }
        else if (audioError_.isNotEmpty())
            status_.setText("Audio loaded. Playback unavailable: " + audioError_, juce::dontSendNotification);
        else
            status_.setText(result.message, juce::dontSendNotification);
        updateControls();
        if (pendingOpen_.existsAsFile() && result.saved && ! dirty_)
        {
            const auto file = pendingOpen_;
            pendingOpen_ = juce::File();
            openFile(file);
            return;
        }
        if (closeAfterSave_ && result.saved && ! dirty_)
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    DatasetMetadata currentMetadata() const
    {
        auto metadata = metadata_;
        metadata.singer = singer_.getText().trim();
        metadata.session = session_.getText().trim();
        metadata.reviewed = reviewed_.getToggleState();
        return metadata;
    }

    void markDirty(bool notesChanged)
    {
        if (clip_ == nullptr) return;
        dirty_ = true;
        autosaveFailed_ = false;
        ++revision_;
        lastEditTime_ = juce::Time::getMillisecondCounterHiRes();
        if (notesChanged) reviewed_.setToggleState(false, juce::dontSendNotification);
        updateControls();
    }

    void updateControls()
    {
        const auto hasClip = clip_ != nullptr;
        open_.setEnabled(! busy_);
        for (auto* button : { &play_, &save_, &saveAs_, &export_, &fit_, &zoomIn_, &zoomOut_ })
            button->setEnabled(hasClip && ! busy_);
        undo_.setEnabled(hasClip && ! busy_ && ! undoStack_.empty());
        redo_.setEnabled(hasClip && ! busy_ && ! redoStack_.empty());
        editor_.setEnabled(! busy_);
        singer_.setEnabled(hasClip && ! busy_);
        session_.setEnabled(hasClip && ! busy_);
        reviewed_.setEnabled(hasClip && ! busy_);
        mode_.setEnabled(! busy_);
        modelChoices_.setEnabled(! busy_);
        modelButton_.setEnabled(! busy_);
        draftButton_.setEnabled(hasClip && ! busy_);
    }

    void beginOperation(const juce::String& text)
    {
        busy_ = true;
        playing_.store(false);
        progress_.store(0.0f);
        operationText_ = text;
        status_.setText(text, juce::dontSendNotification);
        updateControls();
    }

    void chooseAudio()
    {
        if (busy_) return;
        chooser_ = std::make_unique<juce::FileChooser>("Open vocal or saved annotation", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
            {
                if (safe != nullptr && chooser.getResult().existsAsFile()) safe->openFile(chooser.getResult());
            });
    }

    static juce::File defaultModel()
    {
        const auto installed = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile(".synthetic_obsidian/models/note-boundaries/sasha-v1/model.json");
        if (installed.existsAsFile()) return installed;
        const auto directory = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        const auto macResource = directory.getParentDirectory().getChildFile("Resources/note-model/model.json");
        if (macResource.existsAsFile()) return macResource;
        return directory.getChildFile("models/note-boundaries/model.json");
    }

    juce::File activeModel() const { return mode_.getSelectedId() == 2 ? selectedModel_ : juce::File(); }

    void updateModelLabel()
    {
        modelLabel_.setText(mode_.getSelectedId() == 2
            ? "AI draft | needs review"
            : "Math draft | no model selected", juce::dontSendNotification);
    }

    void refreshModelChoices()
    {
        const auto directory = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile(".synthetic_obsidian/models/note-boundaries");
        const auto installed = directory.findChildFiles(juce::File::findFiles, true, "model.json");
        modelFiles_.assign(installed.begin(), installed.end());
        for (const auto& file : { defaultModel(), selectedModel_ })
            if (file != juce::File() && std::find(modelFiles_.begin(), modelFiles_.end(), file) == modelFiles_.end())
                modelFiles_.push_back(file);
        std::sort(modelFiles_.begin(), modelFiles_.end(), [](const auto& left, const auto& right)
        {
            return left.getFullPathName().compareNatural(right.getFullPathName()) < 0;
        });
        modelChoices_.clear(juce::dontSendNotification);
        modelChoices_.setTextWhenNothingSelected("Select model");
        for (size_t i = 0; i < modelFiles_.size(); ++i)
        {
            const auto& file = modelFiles_[i];
            auto label = file.getFileName() == "model.json" ? file.getParentDirectory().getFileName()
                                                           : file.getFileNameWithoutExtension();
            if (! file.existsAsFile()) label += " (missing)";
            const auto id = static_cast<int>(i) + 1;
            modelChoices_.addItem(label, id);
            if (file == selectedModel_) modelChoices_.setSelectedId(id, juce::dontSendNotification);
        }
        modelChoices_.setTooltip(selectedModel_.getFullPathName() + "\nChoose a model, then Generate draft.");
    }

    void selectModel(const juce::File& file)
    {
        selectedModel_ = file;
        preferences_->setValue("noteModelPath", selectedModel_.getFullPathName());
        mode_.setSelectedId(2, juce::sendNotificationSync);
        preferences_->saveIfNeeded();
        refreshModelChoices();
        updateModelLabel();
        status_.setText("Model selected. Generate draft to use it on the current vocal.", juce::dontSendNotification);
    }

    void chooseModel()
    {
        if (busy_) return;
        chooser_ = std::make_unique<juce::FileChooser>("Select a native note model", selectedModel_, "*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
            {
                if (safe == nullptr || ! chooser.getResult().existsAsFile()) return;
                safe->selectModel(chooser.getResult());
            });
    }

    void generateDraft()
    {
        if (busy_ || clip_ == nullptr) return;
        const auto clip = clip_;
        const auto file = activeModel();
        beginOperation("Generating a new note draft...");
        pool_.addJob([this, clip, file]
        {
            Completion result;
            try
            {
                auto draft = analyzeDraft(clip->audio, clip->document.sampleRate, file, cancelled_, progress_);
                if (cancelled_.load()) return;
                result.message = draft.name + ": " + juce::String(static_cast<int>(draft.analysis.notes.size()))
                    + " draft notes. Original annotation kept; Undo restores previous notes.";
                if (draft.warning.isNotEmpty()) result.message = draft.warning;
                result.draft = std::move(draft);
            }
            catch (const std::exception& exception) { result.error = "Could not generate notes: " + juce::String(exception.what()); }
            post(std::move(result));
        });
    }

    void openFile(const juce::File& file)
    {
        if (busy_) return;
        if (dirty_)
        {
            pendingOpen_ = file;
            save(false);
            return;
        }
        beginOperation("Loading vocal and detecting notes...");
        const auto modelFile = activeModel();
        pool_.addJob([this, file, modelFile]
        {
            Completion result;
            try
            {
                auto loaded = std::make_shared<LoadedClip>();
                juce::File audioFile = file;
                auto jsonFile = file.hasFileExtension("json") ? file : file.getSiblingFile(file.getFileNameWithoutExtension() + ".notes.annotation.json");
                const auto exportedSidecar = file.getSiblingFile(file.getFileNameWithoutExtension() + ".annotation.json");
                if (! file.hasFileExtension("json") && ! jsonFile.existsAsFile() && exportedSidecar.existsAsFile())
                    jsonFile = exportedSidecar;
                if (jsonFile.existsAsFile())
                {
                    const auto read = vocal_annotation::AnnotationJson::load(jsonFile, loaded->document);
                    if (read.failed()) { result.error = read.getErrorMessage(); post(std::move(result)); return; }
                    audioFile = loaded->document.audioFile;
                    loaded->metadata = DatasetExport::readMetadata(jsonFile);
                }
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audioFile));
                if (reader == nullptr || ! std::isfinite(reader->sampleRate) || reader->sampleRate < 8000.0
                    || reader->sampleRate > 192000.0 || reader->numChannels < 1 || reader->numChannels > 2
                    || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max()
                    || static_cast<juce::uint64>(reader->lengthInSamples) * reader->numChannels * sizeof(float) > 512ULL * 1024ULL * 1024ULL)
                {
                    result.error = "Use mono or stereo audio at 8-192 kHz, with less than 512 MB of decoded audio.";
                    post(std::move(result)); return;
                }
                loaded->audio.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
                for (int offset = 0; offset < loaded->audio.getNumSamples(); offset += 65536)
                {
                    if (cancelled_.load()) return;
                    const auto count = juce::jmin(65536, loaded->audio.getNumSamples() - offset);
                    if (! reader->read(&loaded->audio, offset, count, offset, true, true))
                    {
                        result.error = "Could not decode audio."; post(std::move(result)); return;
                    }
                    for (int channel = 0; channel < loaded->audio.getNumChannels(); ++channel)
                        for (int i = offset; i < offset + count; ++i)
                            if (! std::isfinite(loaded->audio.getSample(channel, i)))
                            {
                                result.error = "Audio contains non-finite samples."; post(std::move(result)); return;
                            }
                }
                const auto duration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
                if (jsonFile.existsAsFile()
                    && (loaded->document.sampleRate != juce::roundToInt(reader->sampleRate)
                        || ! std::isfinite(loaded->document.duration) || std::abs(loaded->document.duration - duration) > 1.0 / reader->sampleRate))
                {
                    result.error = "Saved annotation does not match this audio's duration or sample rate.";
                    post(std::move(result)); return;
                }
                loaded->document.audioFile = audioFile;
                loaded->document.sampleRate = juce::roundToInt(reader->sampleRate);
                loaded->document.duration = duration;
                auto draft = analyzeDraft(loaded->audio, reader->sampleRate, modelFile, cancelled_, progress_);
                loaded->analysis = std::move(draft.analysis);
                if (cancelled_.load()) return;
                if (! jsonFile.existsAsFile())
                {
                    loaded->document.notes = loaded->analysis.notes;
                    loaded->metadata.modelId = draft.modelId;
                    loaded->metadata.modelHash = draft.modelHash;
                }
                std::sort(loaded->document.notes.begin(), loaded->document.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
                if (const auto valid = DatasetExport::validate(loaded->document); valid.failed())
                {
                    result.error = valid.getErrorMessage(); post(std::move(result)); return;
                }
                const auto audioHash = juce::SHA256(audioFile).toHexString();
                if (loaded->metadata.audioHash.isNotEmpty() && loaded->metadata.audioHash != audioHash)
                {
                    result.error = "The audio has changed since this annotation was saved.";
                    post(std::move(result)); return;
                }
                loaded->metadata.audioHash = audioHash;
                if (loaded->metadata.sourceHash.isEmpty()) loaded->metadata.sourceHash = audioHash;
                loaded->projectFile = jsonFile;
                result.message = audioFile.getFileName() + " - " + juce::String(static_cast<int>(loaded->document.notes.size()))
                    + " notes. " + (jsonFile.existsAsFile() ? "Saved annotations kept. " : draft.name + ". ")
                    + "Review attacks and offsets before marking the clip checked.";
                if (draft.warning.isNotEmpty()) result.message += " " + draft.warning;
                result.loaded = std::move(loaded);
            }
            catch (const std::exception& exception) { result.error = "Could not load vocal: " + juce::String(exception.what()); }
            post(std::move(result));
        });
    }

    void chooseSaveAs()
    {
        if (busy_ || clip_ == nullptr) return;
        chooser_ = std::make_unique<juce::FileChooser>("Save annotation as", projectFile_, "*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
            {
                if (safe == nullptr || chooser.getResult() == juce::File() || safe->busy_) return;
                safe->projectFile_ = chooser.getResult().withFileExtension("json");
                safe->save(false);
            });
    }

    void save(bool autosave)
    {
        if (busy_ || clip_ == nullptr) return;
        const auto document = document_;
        const auto metadata = currentMetadata();
        const auto file = projectFile_;
        const auto revision = revision_;
        beginOperation(autosave ? "Autosaving annotation..." : "Saving annotation...");
        pool_.addJob([this, document, metadata, file, revision, autosave]
        {
            Completion completion;
            try
            {
                const auto result = DatasetExport::saveProject(document, metadata, file);
                completion.saved = result.wasOk();
                completion.error = result.getErrorMessage();
            }
            catch (const std::exception& exception) { completion.error = "Could not save annotation: " + juce::String(exception.what()); }
            completion.message = "Saved " + file.getFileName();
            completion.autosave = autosave;
            completion.revision = revision;
            post(std::move(completion));
        });
    }

    void chooseExport()
    {
        if (busy_ || clip_ == nullptr) return;
        chooser_ = std::make_unique<juce::FileChooser>("Choose dataset folder", document_.audioFile.getParentDirectory());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [safe = juce::Component::SafePointer<MainComponent>(this)](const auto& chooser)
            {
                if (safe == nullptr || ! chooser.getResult().isDirectory() || safe->busy_) return;
                const auto document = safe->document_;
                const auto metadata = safe->currentMetadata();
                const auto clip = safe->clip_;
                const auto directory = chooser.getResult();
                safe->beginOperation("Exporting audio and note labels...");
                safe->pool_.addJob([safe, document, metadata, clip, directory]
                {
                    if (safe == nullptr) return;
                    Completion completion;
                    try
                    {
                        juce::File published;
                        const auto result = DatasetExport::write(document, metadata, clip->audio, directory, published);
                        completion.error = result.getErrorMessage();
                        completion.message = "Exported " + juce::String(metadata.reviewed ? "checked" : "draft") + " bundle: " + published.getFullPathName();
                    }
                    catch (const std::exception& exception) { completion.error = "Could not export dataset: " + juce::String(exception.what()); }
                    safe->post(std::move(completion));
                });
            });
    }

    void undo(bool redo)
    {
        if (busy_ || clip_ == nullptr) return;
        auto& source = redo ? redoStack_ : undoStack_;
        auto& target = redo ? undoStack_ : redoStack_;
        if (source.empty()) return;
        target.push_back({ document_.notes, metadata_.modelId, metadata_.modelHash });
        document_.notes = std::move(source.back().notes);
        metadata_.modelId = source.back().modelId;
        metadata_.modelHash = source.back().modelHash;
        source.pop_back();
        editor_.clearSelection();
        markDirty(true);
    }

    void startPlayback(double start, double end)
    {
        if (busy_ || clip_ == nullptr || audioError_.isNotEmpty()) return;
        playing_.store(false);
        playbackEnd_.store(end);
        seekRequest_.store(start);
        displayPosition_.store(start);
        playing_.store(true);
    }

    void togglePlayback()
    {
        if (playing_.load()) playing_.store(false);
        else
        {
            const auto position = displayPosition_.load();
            startPlayback(position >= document_.duration - 0.001 ? 0.0 : position, document_.duration);
        }
    }

    void timerCallback() override
    {
        editor_.setPlayhead(displayPosition_.load(std::memory_order_relaxed));
        play_.setButtonText(playing_.load(std::memory_order_relaxed) ? "Stop" : "Play");
        if (busy_ && progress_.load(std::memory_order_relaxed) > 0.0f)
            status_.setText(operationText_ + " " + juce::String(juce::roundToInt(progress_.load() * 100.0f)) + "%", juce::dontSendNotification);
        if (dirty_ && ! autosaveFailed_ && ! busy_ && juce::Time::getMillisecondCounterHiRes() - lastEditTime_ > 2500.0)
            save(true);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        deviceRate_ = device->getCurrentSampleRate();
        sourcePosition_ = 0.0;
        playbackStart_ = 0.0;
        playbackGain_ = 0.0f;
    }
    void audioDeviceStopped() override { playing_.store(false); }

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels,
                                         int samples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int channel = 0; channel < channels; ++channel)
            if (outputs[channel] != nullptr) juce::FloatVectorOperations::clear(outputs[channel], samples);
        // clip_ is replaced only while the callback is detached. No reference-count operations here.
        if (clip_ == nullptr || deviceRate_ <= 0.0) return;
        const auto seek = seekRequest_.exchange(-1.0, std::memory_order_relaxed);
        if (seek >= 0.0) { sourcePosition_ = seek * clip_->document.sampleRate; playbackStart_ = sourcePosition_; }
        const auto requestedPlayback = playing_.load();
        if (! requestedPlayback && playbackGain_ <= 0.0f) return;
        const auto& audio = clip_->audio;
        const auto ratio = clip_->document.sampleRate / deviceRate_;
        const auto endSample = juce::jmin(static_cast<double>(audio.getNumSamples()), playbackEnd_.load() * clip_->document.sampleRate);
        const auto fadeSamples = clip_->document.sampleRate * 0.005;
        for (int i = 0; i < samples; ++i)
        {
            if (sourcePosition_ >= endSample) { playing_.store(false); playbackGain_ = 0.0f; break; }
            const auto index = static_cast<int>(sourcePosition_);
            const auto next = juce::jmin(index + 1, audio.getNumSamples() - 1);
            const auto fraction = static_cast<float>(sourcePosition_ - index);
            playbackGain_ = juce::jlimit(0.0f, 1.0f, playbackGain_ + (requestedPlayback ? 1.0f : -1.0f) / static_cast<float>(deviceRate_ * 0.005));
            const auto gain = playbackGain_ * static_cast<float>(juce::jlimit(0.0, 1.0,
                juce::jmin(sourcePosition_ - playbackStart_, endSample - sourcePosition_) / fadeSamples));
            for (int channel = 0; channel < channels; ++channel)
                if (outputs[channel] != nullptr)
                {
                    const auto* data = audio.getReadPointer(juce::jmin(channel, audio.getNumChannels() - 1));
                    outputs[channel][i] = juce::jlimit(-1.0f, 1.0f, (data[index] + fraction * (data[next] - data[index])) * gain);
                }
            sourcePosition_ += ratio;
            if (! requestedPlayback && playbackGain_ <= 0.0f) break;
        }
        displayPosition_.store(sourcePosition_ / clip_->document.sampleRate, std::memory_order_relaxed);
    }

    vocal_annotation::AnnotationDocument document_;
    NoteAnalysis analysis_;
    DatasetMetadata metadata_;
    std::shared_ptr<LoadedClip> clip_;
    juce::File projectFile_, pendingOpen_;
    WaveformEditor editor_;
    juce::TextButton open_ { "Open..." }, play_ { "Play" }, save_ { "Save" }, export_ { "Export dataset..." };
    juce::TextButton saveAs_ { "Save as..." };
    juce::TextButton undo_ { "Undo" }, redo_ { "Redo" }, fit_ { "Fit" }, zoomIn_ { "+" }, zoomOut_ { "-" };
    juce::TextEditor singer_, session_;
    juce::ToggleButton reviewed_ { "Entire clip checked by hand" };
    juce::ComboBox mode_, modelChoices_;
    juce::TextButton modelButton_ { "Model..." }, draftButton_ { "Generate draft" };
    juce::Label modelLabel_;
    juce::File selectedModel_;
    std::vector<juce::File> modelFiles_;
    std::unique_ptr<juce::PropertiesFile> preferences_;
    juce::Label status_, help_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::vector<NoteSnapshot> undoStack_, redoStack_;
    juce::AudioDeviceManager devices_;
    juce::ThreadPool pool_ { 1 };
    std::mutex completionMutex_;
    Completion completion_;
    std::atomic<bool> cancelled_ { false }, playing_ { false };
    std::atomic<float> progress_ { 0.0f };
    std::atomic<double> seekRequest_ { -1.0 }, playbackEnd_ { 0.0 }, displayPosition_ { 0.0 };
    // The following scalars belong exclusively to the device callback thread.
    double deviceRate_ = 0.0, sourcePosition_ = 0.0, playbackStart_ = 0.0;
    float playbackGain_ = 0.0f;
    juce::String audioError_, operationText_;
    bool busy_ = false, dirty_ = false, closeAfterSave_ = false, autosaveFailed_ = false;
    juce::uint64 revision_ = 0;
    double lastEditTime_ = 0.0;
};

class Application final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Vocal Note Annotator"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    void initialise(const juce::String&) override { window_ = std::make_unique<Window>(); }
    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override
    {
        if (window_ != nullptr && static_cast<MainComponent*>(window_->getContentComponent())->requestClose()) quit();
    }
private:
    class Window final : public juce::DocumentWindow
    {
    public:
        Window() : DocumentWindow("Vocal Note Annotator", juce::Colour(0xff16202b), allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, false);
            setResizeLimits(1050, 480, 2400, 1600);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window_;
};
}

START_JUCE_APPLICATION(native_annotation::Application)
