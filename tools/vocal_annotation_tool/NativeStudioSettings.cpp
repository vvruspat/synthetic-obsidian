#include "NativeStudioSettings.h"
#include "uikit/Controls.h"
#include "uikit/Theme.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace vocal_annotation
{
namespace
{
namespace ui = obsidian::ui;
using Properties = std::initializer_list<std::pair<juce::Identifier, juce::var>>;

juce::var message(const juce::String& type, const juce::String& action = {}, Properties properties = {})
{
    auto object = std::make_unique<juce::DynamicObject>();
    object->setProperty("type", type);
    if (action.isNotEmpty()) object->setProperty("action", action);
    for (const auto& property : properties) object->setProperty(property.first, property.second);
    return object.release();
}
juce::StringArray strings(const juce::var& values)
{
    juce::StringArray result;
    if (const auto* array = values.getArray()) for (const auto& value : *array) result.add(value.toString());
    return result;
}
double number(const juce::var& value)
{
    const auto result = static_cast<double>(value);
    return std::isfinite(result) ? result : 0.0;
}
juce::String duration(double seconds)
{
    const auto total = juce::jmax(0, juce::roundToInt(seconds));
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}
class Text final : public juce::Component
{
public:
    explicit Text(juce::String text = {}, bool heading = false) : text_(std::move(text)), heading_(heading)
    { setTitle(text_); setInterceptsMouseClicks(false, false); }
    void setText(juce::String text) { if (text == text_) return; text_ = std::move(text); setTitle(text_); repaint(); }
    void paint(juce::Graphics& g) override
    {
        g.setColour(ui::colour(heading_ ? ui::tokens::text : ui::tokens::muted));
        g.setFont(ui::font(heading_ ? ui::tokens::fontBody + 2.0f : ui::tokens::fontSmall));
        g.drawFittedText(text_, getLocalBounds(), juce::Justification::centredLeft, heading_ ? 1 : 3);
    }
protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    { return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::staticText); }
private:
    juce::String text_;
    bool heading_;
};

class StateRow final : public juce::Component
{
public:
    StateRow(juce::String id, bool job) : id_(std::move(id)), job_(job), action_(job ? "Show" : "Remove")
    {
        addAndMakeVisible(text_); addAndMakeVisible(action_);
        action_.setComponentID((job ? "settings.job.action." : "settings.source.remove.") + id_);
        action_.onClick = [this] { auto callback = onAction; if (callback) callback(); };
    }
    const juce::String& id() const noexcept { return id_; }
    void update(const juce::var& value, bool working)
    {
        if (job_)
        {
            const auto percent = juce::roundToInt(juce::jlimit(0.0, 1.0, number(value["progress"])) * 100.0);
            auto detail = value["presetName"].toString() + " / " + value["status"].toString() + " / " + juce::String(percent) + "%";
            const auto total = static_cast<int>(value["totalSteps"]);
            if (total > 0) detail += " / step " + value["currentStep"].toString() + "/" + juce::String(total);
            if (value.hasProperty("loss")) detail += " / loss " + juce::String(number(value["loss"]), 4);
            text_.setText(detail + "\n" + value["stage"].toString() + ": " + value["message"].toString());
            action_.setText(static_cast<bool>(value["canCancel"]) ? "Cancel" : "Show");
            progress_ = static_cast<float>(juce::jlimit(0.0, 1.0, number(value["progress"])));
        }
        else
        {
            text_.setText(value["name"].toString() + " / " + duration(number(value["durationSeconds"]))
                + " / " + juce::String(number(value["sampleRate"]) / 1000.0, 1) + " kHz / "
                + value["channels"].toString() + " ch / " + value["status"].toString()
                + "\n" + value["message"].toString());
            action_.setEnabled(!working);
        }
        setTitle(text_.getTitle()); repaint();
    }
    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 4);
        action_.setBounds(area.removeFromRight(76).withSizeKeepingCentre(76, ui::tokens::controlHeight));
        area.removeFromRight(8); text_.setBounds(area.reduced(0, job_ ? 4 : 0));
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(ui::colour(ui::tokens::raised)); g.fillRoundedRectangle(getLocalBounds().toFloat(), ui::tokens::radius);
        if (job_)
        {
            auto bar = getLocalBounds().reduced(8).removeFromBottom(3).toFloat();
            g.setColour(ui::colour(ui::tokens::border)); g.fillRect(bar);
            g.setColour(ui::colour(ui::tokens::olive)); g.fillRect(bar.withWidth(bar.getWidth() * progress_));
        }
    }
    std::function<void()> onAction;
private:
    juce::String id_;
    bool job_;
    float progress_ = 0.0f;
    Text text_;
    ui::Button action_;
};
}

class NativeStudioSettings::Content final : public juce::Component
{
public:
    explicit Content(std::function<void(const juce::var&)> handler) : handler_(std::move(handler))
    {
        const auto add = [this](juce::Component& component, const char* id, const char* title)
        { addAndMakeVisible(component); component.setComponentID(id); component.setTitle(title); };
        for (auto* text : { &audioHeading_, &audioHelp_, &inputLabel_, &outputLabel_, &audioStatus_, &voiceHeading_,
                           &runtime_, &nameLabel_, &qualityLabel_, &sourceHelp_, &coverage_, &voiceStatus_, &jobsHeading_,
                           &profilesHeading_, &backingHeading_, &parentLabel_, &styleLabel_, &generatorLabel_,
                           &targetLabel_, &voiceLabel_, &status_ }) addAndMakeVisible(*text);
        add(input_, "settings.audio.input", "Audio input device");
        add(output_, "settings.audio.output", "Audio output device");
        add(refreshAudio_, "settings.audio.refresh", "Refresh audio devices");
        add(applyAudio_, "settings.audio.apply", "Apply audio devices");
        add(testOutput_, "settings.audio.test", "Test audio output");
        add(name_, "settings.voice.name", "Voice preset name");
        add(quality_, "settings.voice.quality", "Voice profile and training quality");
        add(addFiles_, "settings.voice.add-files", "Add vocal recordings");
        add(currentTrack_, "settings.voice.current", "Use current main vocal recording");
        add(clearFiles_, "settings.voice.clear", "Clear vocal recordings");
        add(create_, "settings.voice.create", "Create reference voice profile");
        add(train_, "settings.voice.train", "Start LoRA training");
        add(cancel_, "settings.voice.cancel", "Cancel profile preparation");
        add(reveal_, "settings.voice.reveal", "Reveal voice profile folder");
        add(profile_, "settings.profile.select", "Saved voice profiles");
        add(deleteProfile_, "settings.profile.delete", "Move selected voice profile to Trash");
        add(parent_, "settings.backing.parent", "Backing source track");
        add(style_, "settings.backing.style", "Backing harmony style");
        add(generator_, "settings.backing.generator", "Backing notes generator");
        add(generate_, "settings.backing.generate", "Generate backing notes");
        add(target_, "settings.backing.target", "Existing backing track");
        add(voice_, "settings.backing.voice", "Voice profile for backing track");
        add(regenerate_, "settings.backing.regenerate", "Regenerate selected backing notes");
        add(render_, "settings.backing.render", "Render selected backing audio");
        add(removeBacking_, "settings.backing.remove", "Remove selected backing track");
        quality_.setItems({ "Balanced", "High" }); generator_.setItems({ "Local", "OpenAI Luna", "OpenAI Terra" });
        name_.setText("My Voice", false); name_.setInputRestrictions(64); name_.setFont(ui::font());
        name_.setColour(juce::TextEditor::backgroundColourId, ui::colour(ui::tokens::raised));
        name_.setColour(juce::TextEditor::textColourId, ui::colour(ui::tokens::text));
        name_.setColour(juce::TextEditor::outlineColourId, ui::colour(ui::tokens::border));
        name_.setColour(juce::TextEditor::focusedOutlineColourId, ui::colour(ui::tokens::focus));
        name_.onTextChange = [this] { updateEnabled(); };
        refreshAudio_.onClick = [this] { emit("audio-settings", "request"); };
        testOutput_.onClick = [this] { emit("audio-settings", "test-output"); };
        output_.onChange = [this](int) { updateEnabled(); };
        applyAudio_.onClick = [this]
        {
            emit("audio-settings", "set-devices", { { "inputDevice", input_.getSelectedIndex() <= 0 ? juce::String() : input_.getSelectedText() },
                                                     { "outputDevice", output_.getSelectedText() } });
        };
        addFiles_.onClick = [this] { emit("voice-training", "add-files"); };
        currentTrack_.onClick = [this] { emit("voice-training", "add-current-track"); };
        clearFiles_.onClick = [this] { emit("voice-training", "clear-files"); };
        cancel_.onClick = [this] { emit("voice-training", "cancel"); };
        reveal_.onClick = [this] { emit("voice-training", "reveal-profile"); };
        create_.onClick = [this] { startVoice("create-profile"); };
        train_.onClick = [this] { startVoice("start-training"); };
        profile_.onChange = [this](int) { deleteArmed_ = false; deleteProfile_.setText("Move to Trash"); updateEnabled(); };
        deleteProfile_.onClick = [this]
        {
            if (!deleteArmed_) { deleteArmed_ = true; deleteProfile_.setText("Confirm Trash"); return; }
            const auto id = selectedId(profile_, profileIds_);
            deleteArmed_ = false; deleteProfile_.setText("Move to Trash");
            emit("delete-voice-profile", {}, { { "profileId", id } });
        };
        generate_.onClick = [this]
        {
            emit("add-backing-track", {}, { { "track", style_.getSelectedText() }, { "generator", generatorId() },
                                            { "parentId", selectedId(parent_, parentIds_) } });
        };
        regenerate_.onClick = [this] { emit("regenerate-backing-track", {}, { { "track", selectedId(target_, targetIds_) }, { "generator", generatorId() } }); };
        render_.onClick = [this] { emit("render-backing-track", {}, { { "track", selectedId(target_, targetIds_) } }); };
        removeBacking_.onClick = [this] { emit("remove-backing-track", {}, { { "track", selectedId(target_, targetIds_) } }); };
        target_.onChange = [this](int) { syncSelectedVoice(); updateEnabled(); };
        voice_.onChange = [this](int)
        { emit("set-backing-voice-profile", {}, { { "track", selectedId(target_, targetIds_) }, { "profileId", selectedId(voice_, profileIds_) } }); };
        for (auto* button : { &create_, &train_, &generate_ }) { button->setVariant(ui::Button::Variant::accent); button->setAccent(ui::colour(ui::tokens::olive)); }
        updateEnabled();
    }
    void dispatch(const juce::var& event)
    {
        const auto type = event["type"].toString();
        if (type == "audio-settings-state")
        {
            audio_ = event["state"];
            auto inputs = strings(audio_["inputDevices"]); inputs.insert(0, "No input");
            input_.setItems(inputs); input_.setSelectedIndex(juce::jmax(0, inputs.indexOf(audio_["inputDevice"].toString())));
            const auto outputs = strings(audio_["outputDevices"]); output_.setItems(outputs);
            output_.setSelectedIndex(outputs.indexOf(audio_["outputDevice"].toString()));
            auto detail = audio_["deviceType"].toString() + " / " + juce::String(number(audio_["sampleRate"]) / 1000.0, 1)
                + " kHz / " + audio_["bufferSize"].toString() + " samples";
            const auto error = audio_["error"].toString(); if (error.isNotEmpty()) detail += "\n" + error;
            audioStatus_.setText(detail);
        }
        else if (type == "voice-training-state")
        {
            training_ = event["state"];
            const auto phase = training_["phase"].toString(); working_ = phase == "preparing" || phase == "cancelling";
            runtime_.setText(training_["device"].toString() + " / " + training_["runtimeStatus"].toString() + "\n" + training_["runtimeMessage"].toString());
            const auto active = training_["activeProfileName"].toString();
            if (active.isNotEmpty() && active != lastActiveName_) { lastActiveName_ = active; name_.setText(active, false); }
            coverage_.setText(duration(number(training_["totalDurationSeconds"])) + " usable / "
                + duration(number(training_["minimumDurationSeconds"])) + " minimum / "
                + duration(number(training_["recommendedDurationSeconds"])) + " recommended");
            voiceStatus_.setText(phase + " / " + juce::String(juce::roundToInt(number(training_["progress"]) * 100.0)) + "% / "
                + training_["message"].toString() + "\n" + training_["outputDirectory"].toString());
            syncRows(training_["sources"], sources_, false); syncRows(training_["jobs"], jobs_, true);
        }
        else if (type == "project-state")
        {
            project_ = event["project"];
            syncOptions(project_["voiceProfiles"], profile_, profileIds_, "id", "name");
            const auto oldVoice = selectedId(voice_, oldProfileIds_);
            voice_.setItems(profile_.getItems()); voice_.setSelectedIndex(profileIds_.indexOf(oldVoice)); oldProfileIds_ = profileIds_;
            const auto initialParent = parentIds_.isEmpty();
            syncOptions(project_["tracks"], parent_, parentIds_, "id", "name");
            if (initialParent)
            {
                const auto vocal = parent_.getItems().indexOf("Voice Main");
                if (vocal >= 0) parent_.setSelectedIndex(vocal);
            }
            applyBackingParent();
            syncOptions(project_["initialBackingTracks"], target_, targetIds_, "id", "name");
            const auto oldStyle = style_.getSelectedText(); const auto styles = strings(project_["backingTrackOptions"]);
            style_.setItems(styles); style_.setSelectedIndex(juce::jmax(0, styles.indexOf(oldStyle)));
            deleteArmed_ = false; deleteProfile_.setText("Move to Trash"); syncSelectedVoice();
        }
        else if (type == "status-state")
        {
            busy_ = static_cast<bool>(event["vocalAnalysisRunning"]) || static_cast<bool>(event["instrumentalAnalysisRunning"])
                || static_cast<bool>(event["backingGenerationRunning"]) || static_cast<bool>(event["backingAudioRenderRunning"]);
            status_.setText(event["message"].toString());
        }
        else return;
        updateEnabled(); resized(); if (onLayoutChange) onLayoutChange(); repaint();
    }
    void setBackingParent(const juce::String& trackId)
    {
        pendingBackingParent_ = trackId;
        applyBackingParent();
        updateEnabled();
    }
    std::function<void()> onLayoutChange;
    void paint(juce::Graphics& g) override
    {
        g.setColour(ui::colour(ui::tokens::border)); g.fillRect(preparationProgress_);
        g.setColour(ui::colour(ui::tokens::olive));
        const auto value = static_cast<float>(juce::jlimit(0.0, 1.0, number(training_["progress"])));
        g.fillRect(preparationProgress_.withWidth(preparationProgress_.getWidth() * value));
    }
    void resized() override
    {
        auto y = 8;
        const auto width = juce::jmax(1, getWidth() - 32);
        const auto row = [&](juce::Component& component, int height) { component.setBounds(16, y, width, height); y += height + 8; };
        const auto pair = [&](juce::Component& left, juce::Component& right, int height)
        {
            const auto half = juce::jmax(0, (width - 12) / 2);
            left.setBounds(16, y, half, height); right.setBounds(28 + half, y, juce::jmax(0, width - half - 12), height); y += height + 8;
        };
        row(audioHeading_, 24); row(audioHelp_, 30); pair(inputLabel_, outputLabel_, 18); pair(input_, output_, 32);
        const auto third = juce::jmax(0, (width - 16) / 3);
        refreshAudio_.setBounds(16, y, third, 32); applyAudio_.setBounds(24 + third, y, third, 32);
        testOutput_.setBounds(32 + third * 2, y, juce::jmax(0, width - third * 2 - 16), 32); y += 40;
        row(audioStatus_, 42); y += 12;
        row(voiceHeading_, 24); row(runtime_, 42); pair(nameLabel_, qualityLabel_, 18); pair(name_, quality_, 32);
        addFiles_.setBounds(16, y, third, 32); currentTrack_.setBounds(24 + third, y, third, 32);
        clearFiles_.setBounds(32 + third * 2, y, juce::jmax(0, width - third * 2 - 16), 32); y += 40;
        row(sourceHelp_, 30);
        for (auto& source : sources_) row(*source, 58);
        row(coverage_, 26); row(voiceStatus_, 48);
        preparationProgress_ = juce::Rectangle<float>(16.0f, static_cast<float>(y), static_cast<float>(width), 4.0f); y += 12;
        pair(create_, train_, 32); pair(cancel_, reveal_, 32);
        row(jobsHeading_, jobs_.empty() ? 0 : 24);
        for (auto& job : jobs_) row(*job, 76);
        y += 12; row(profilesHeading_, 24); pair(profile_, deleteProfile_, 32);
        y += 12; row(backingHeading_, 24); pair(parentLabel_, styleLabel_, 18); pair(parent_, style_, 32);
        pair(generatorLabel_, generator_, 32); row(generate_, 32);
        pair(targetLabel_, voiceLabel_, 18); pair(target_, voice_, 32); pair(regenerate_, render_, 32); row(removeBacking_, 32);
        row(status_, 44);
        laidOutHeight_ = y + 8;
    }
    int laidOutHeight() const noexcept { return laidOutHeight_; }
private:
    void emit(const juce::String& type, const juce::String& action = {}, Properties properties = {})
    { auto callback = handler_; if (callback) callback(message(type, action, properties)); }
    static juce::String selectedId(const ui::Select& select, const juce::StringArray& ids)
    { const auto index = select.getSelectedIndex(); return juce::isPositiveAndBelow(index, ids.size()) ? ids[index] : juce::String(); }
    void syncOptions(const juce::var& values, ui::Select& select, juce::StringArray& ids, const char* idKey, const char* nameKey)
    {
        const auto old = selectedId(select, ids); ids.clear(); juce::StringArray names;
        if (const auto* array = values.getArray()) for (const auto& value : *array)
        { ids.add(value[idKey].toString()); names.add(value[nameKey].toString()); }
        select.setItems(names); select.setSelectedIndex(juce::jmax(0, ids.indexOf(old)));
    }
    void applyBackingParent()
    {
        const auto index = parentIds_.indexOf(pendingBackingParent_);
        if (pendingBackingParent_.isNotEmpty() && index >= 0)
        {
            parent_.setSelectedIndex(index, juce::dontSendNotification);
            pendingBackingParent_.clear();
        }
    }
    juce::String generatorId() const
    { return generator_.getSelectedIndex() == 1 ? "luna" : generator_.getSelectedIndex() == 2 ? "terra" : "local"; }
    void startVoice(const juce::String& action)
    { emit("voice-training", action, { { "presetName", name_.getText().trim() }, { "quality", quality_.getSelectedIndex() == 1 ? "high" : "balanced" } }); }
    void syncSelectedVoice()
    {
        const auto target = selectedId(target_, targetIds_);
        if (const auto* array = project_["backingTrackContents"].getArray()) for (const auto& value : *array)
            if (value["id"].toString() == target) { voice_.setSelectedIndex(profileIds_.indexOf(value["voiceProfileId"].toString())); return; }
        voice_.setSelectedIndex(profileIds_.isEmpty() ? -1 : 0);
    }
    void syncRows(const juce::var& values, std::vector<std::unique_ptr<StateRow>>& rows, bool jobs)
    {
        auto old = std::move(rows); rows.clear();
        juce::Component::SafePointer<Content> safe(this);
        if (const auto* array = values.getArray()) for (const auto& value : *array)
        {
            const auto id = value[jobs ? "jobId" : "id"].toString();
            auto found = std::find_if(old.begin(), old.end(), [&id](const auto& row) { return row && row->id() == id; });
            auto row = found != old.end() ? std::move(*found) : std::make_unique<StateRow>(id, jobs);
            row->update(value, working_);
            const auto action = jobs ? (static_cast<bool>(value["canCancel"]) ? "cancel-training" : "reveal-training") : "remove-file";
            row->onAction = [safe, id, action, jobs]
            { if (safe != nullptr) safe->emit("voice-training", action, { { jobs ? "jobId" : "sourceId", id } }); };
            if (row->getParentComponent() != this) addAndMakeVisible(*row);
            rows.push_back(std::move(row));
        }
    }
    void updateEnabled()
    {
        const auto hasName = name_.getText().trim().isNotEmpty();
        name_.setEnabled(!working_); quality_.setEnabled(!working_);
        addFiles_.setEnabled(!working_); currentTrack_.setEnabled(!working_); clearFiles_.setEnabled(!working_ && !sources_.empty());
        create_.setEnabled(!working_ && hasName && static_cast<bool>(training_["canCreateProfile"]));
        train_.setEnabled(!working_ && hasName && static_cast<bool>(training_["canStartTraining"]));
        cancel_.setEnabled(working_); reveal_.setEnabled(training_["outputDirectory"].toString().isNotEmpty());
        applyAudio_.setEnabled(output_.getSelectedIndex() >= 0); testOutput_.setEnabled(output_.getSelectedIndex() >= 0);
        deleteProfile_.setEnabled(!working_ && !busy_ && selectedId(profile_, profileIds_).isNotEmpty() && selectedId(profile_, profileIds_) != "source-timbre");
        generate_.setEnabled(!busy_ && style_.getSelectedIndex() >= 0 && parent_.getSelectedIndex() >= 0);
        const auto hasTarget = target_.getSelectedIndex() >= 0;
        regenerate_.setEnabled(!busy_ && hasTarget); render_.setEnabled(!busy_ && hasTarget); removeBacking_.setEnabled(!busy_ && hasTarget);
        voice_.setEnabled(!busy_ && hasTarget && !profileIds_.isEmpty());
    }
    std::function<void(const juce::var&)> handler_;
    juce::var audio_, training_, project_;
    bool working_ = false, busy_ = false, deleteArmed_ = false;
    int laidOutHeight_ = 1010;
    juce::Rectangle<float> preparationProgress_;
    juce::String lastActiveName_, pendingBackingParent_;
    juce::StringArray profileIds_, oldProfileIds_, parentIds_, targetIds_;
    Text audioHeading_ { "Audio devices", true }, audioHelp_ { "Choose the existing audio devices, then Apply. Settings are saved by the audio engine." };
    Text inputLabel_ { "Input" }, outputLabel_ { "Output" }, audioStatus_ { "Waiting for audio device state..." };
    Text voiceHeading_ { "Voice profiles and LoRA training", true }, runtime_ { "Waiting for training runtime state..." };
    Text nameLabel_ { "Preset name" }, qualityLabel_ { "Quality" };
    Text sourceHelp_ { "Clean solo vocal recordings: 30 seconds minimum. LoRA jobs continue after the app closes." };
    Text coverage_, voiceStatus_ { "Waiting for voice training state..." }, jobsHeading_ { "Training processes", true };
    Text profilesHeading_ { "Saved voice profiles", true }, backingHeading_ { "Backing generation and rendering", true };
    Text parentLabel_ { "Source track" }, styleLabel_ { "Harmony style" }, generatorLabel_ { "Notes generator" };
    Text targetLabel_ { "Existing backing track" }, voiceLabel_ { "Rendering voice" }, status_;
    ui::Select input_, output_, quality_, profile_, parent_, style_, generator_, target_, voice_;
    juce::TextEditor name_;
    ui::Button refreshAudio_ { "Refresh devices" }, applyAudio_ { "Apply" }, testOutput_ { "Test output" };
    ui::Button addFiles_ { "Add files", ui::Icon::plus }, currentTrack_ { "Current vocal" }, clearFiles_ { "Clear files" };
    ui::Button create_ { "Create reference" }, train_ { "Train LoRA" }, cancel_ { "Cancel preparation" }, reveal_ { "Show profile" };
    ui::Button deleteProfile_ { "Move to Trash" }, generate_ { "Generate backing" }, regenerate_ { "Regenerate notes" }, render_ { "Render audio" }, removeBacking_ { "Remove backing track" };
    std::vector<std::unique_ptr<StateRow>> sources_, jobs_;
};

NativeStudioSettings::NativeStudioSettings(std::function<void(const juce::var&)> handler)
    : content_(std::make_unique<Content>(handler)), commandHandler_(std::move(handler))
{
    setTitle("Studio settings and voice lab"); setComponentID("settings.panel"); setVisible(false);
    setWantsKeyboardFocus(true); setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
    auto close = std::make_unique<ui::IconButton>(ui::Icon::close, "Close studio settings");
    close->setComponentID("settings.close");
    close->onClick = [this] { auto callback = onClose; if (callback) callback(); };
    addAndMakeVisible(*close); close_ = std::move(close);
    viewport_.setViewedComponent(content_.get(), false); viewport_.setScrollBarsShown(true, false);
    viewport_.setComponentID("settings.scroll"); addAndMakeVisible(viewport_);
    content_->onLayoutChange = [this] { resized(); };
}
NativeStudioSettings::~NativeStudioSettings() { viewport_.setViewedComponent(nullptr, false); }
void NativeStudioSettings::dispatch(const juce::var& event) { content_->dispatch(event); }
void NativeStudioSettings::setBackingParent(const juce::String& trackId) { content_->setBackingParent(trackId); }
void NativeStudioSettings::refresh()
{
    auto callback = commandHandler_;
    if (!callback) return;
    juce::Component::SafePointer<NativeStudioSettings> safe(this);
    callback(message("audio-settings", "request"));
    if (safe != nullptr) callback(message("voice-training", "request-state"));
}
void NativeStudioSettings::visibilityChanged() { if (isShowing()) refresh(); }
void NativeStudioSettings::paint(juce::Graphics& g)
{
    g.fillAll(ui::colour(ui::tokens::surface));
    g.setColour(ui::colour(ui::tokens::text)); g.setFont(ui::font(ui::tokens::fontTitle));
    g.drawText("Studio settings / Voice lab", getLocalBounds().removeFromTop(58).reduced(20, 0).withTrimmedRight(44), juce::Justification::centredLeft);
    g.setColour(ui::colour(ui::tokens::border)); g.drawHorizontalLine(57, 0.0f, static_cast<float>(getWidth()));
}
void NativeStudioSettings::resized()
{
    close_->setBounds(getWidth() - 48, 14, 30, 30);
    viewport_.setBounds(getLocalBounds().withTrimmedTop(62));
    content_->setSize(juce::jmax(1, viewport_.getWidth() - viewport_.getScrollBarThickness()), content_->getHeight());
    content_->resized();
    content_->setSize(content_->getWidth(), content_->laidOutHeight());
}
bool NativeStudioSettings::keyPressed(const juce::KeyPress& key)
{
    if (key != juce::KeyPress::escapeKey) return false;
    auto callback = onClose; if (callback) callback(); return true;
}
std::unique_ptr<juce::AccessibilityHandler> NativeStudioSettings::createAccessibilityHandler()
{ return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::dialogWindow); }
}
