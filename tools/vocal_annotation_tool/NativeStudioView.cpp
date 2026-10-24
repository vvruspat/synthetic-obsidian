#include "NativeStudioView.h"
#include "editor_ui/Chrome.h"
#include "editor_ui/PitchEditor.h"
#include "uikit/Layout.h"
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace vocal_annotation
{
namespace
{
using namespace obsidian::ui;
using namespace obsidian::editor;
constexpr int kHeaderWidth = 272, kKeyboardWidth = 78, kTransportHeight = 60;
constexpr int kToolbarHeight = 42, kRulerHeight = 24, kStatusHeight = 24, kRowHeight = 72;
using Fields = std::initializer_list<std::pair<juce::Identifier, juce::var>>;
juce::var object(Fields fields)
{
    auto value = std::make_unique<juce::DynamicObject>();
    for (const auto& [key, field] : fields) value->setProperty(key, field);
    return juce::var(value.release());
}
juce::var command(const juce::String& type, Fields fields = {})
{
    auto value = object(fields); value.getDynamicObject()->setProperty("type", type); return value;
}
const juce::Array<juce::var>& array(const juce::var& value)
{
    static const juce::Array<juce::var> empty;
    if (const auto* result = value.getArray()) return *result;
    return empty;
}
double number(const juce::var& value, double fallback = 0.0)
{
    if (!value.isDouble() && !value.isInt() && !value.isInt64()) return fallback;
    const auto result = static_cast<double>(value); return std::isfinite(result) ? result : fallback;
}
juce::Colour trackColour(const juce::String& id, bool backing)
{
    return colour(id == "Instrumental" ? tokens::blue : backing ? tokens::pink : tokens::olive);
}
std::vector<float> decodePeaks(const juce::var& waveform)
{
    std::vector<float> peaks;
    if (waveform.isArray())
    {
        peaks.reserve(static_cast<std::size_t>(array(waveform).size()));
        for (const auto& point : array(waveform))
        {
            const auto& pair = array(point);
            if (pair.size() >= 2) peaks.push_back(static_cast<float>(std::clamp(std::max(std::abs(number(pair[0])), std::abs(number(pair[1]))), 0.0, 1.0)));
        }
    }
    else if (waveform["encoding"].toString() == "i16le-base64")
    {
        juce::MemoryOutputStream decoded;
        if (!juce::Base64::convertFromBase64(decoded, waveform["data"].toString())) return peaks;
        const auto count = std::min(static_cast<std::size_t>(std::max(0.0, number(waveform["pointCount"]))), decoded.getDataSize() / 4);
        peaks.reserve(count);
        const auto* bytes = static_cast<const juce::uint8*>(decoded.getData());
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto low = static_cast<std::int16_t>(juce::ByteOrder::littleEndianShort(bytes + i * 4));
            const auto high = static_cast<std::int16_t>(juce::ByteOrder::littleEndianShort(bytes + i * 4 + 2));
            peaks.push_back(std::clamp(std::max(std::abs(static_cast<float>(low)), std::abs(static_cast<float>(high))) / 32768.0f, 0.0f, 1.0f));
        }
    }
    return peaks;
}
struct TempoChange { double start = 0.0, bpm = 120.0; };
class TempoMap
{
public:
    void set(const juce::var& project)
    {
        fallback_ = std::clamp(number(project["tempo"], 120.0), 20.0, 300.0);
        const auto duration = std::max(0.001, number(project["timelineDurationSeconds"]));
        std::vector<TempoChange> changes;
        for (const auto& item : array(project["tempoSegments"]))
        {
            const auto bpm = number(item["bpm"]);
            if (bpm >= 20.0 && bpm <= 300.0) changes.push_back({ std::clamp(number(item["start"]), 0.0, duration), bpm });
        }
        std::stable_sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        changes_.clear();
        for (const auto& change : changes)
        {
            if (!changes_.empty() && std::abs(changes_.back().start - change.start) < 1.0e-7) changes_.back().bpm = change.bpm;
            else changes_.push_back(change);
        }
    }
    double beatAt(double seconds) const
    {
        seconds = std::max(0.0, seconds);
        double time = 0.0, beat = 0.0, bpm = fallback_;
        for (const auto& change : changes_)
        {
            if (change.start > seconds) break;
            beat += (change.start - time) * bpm / 60.0;
            time = change.start; bpm = change.bpm;
        }
        return beat + (seconds - time) * bpm / 60.0;
    }
    double secondsAt(double beat) const
    {
        beat = std::max(0.0, beat);
        double time = 0.0, current = 0.0, bpm = fallback_;
        for (const auto& change : changes_)
        {
            const auto length = (change.start - time) * bpm / 60.0;
            if (beat <= current + length) return time + (beat - current) * 60.0 / bpm;
            current += length; time = change.start; bpm = change.bpm;
        }
        return time + (beat - current) * 60.0 / bpm;
    }
    double tempoAt(double seconds) const
    {
        auto bpm = fallback_;
        for (const auto& change : changes_) { if (change.start > seconds) break; bpm = change.bpm; }
        return bpm;
    }
private:
    double fallback_ = 120.0;
    std::vector<TempoChange> changes_;
};
class ArrangementPanel final : public juce::Component
{
public:
    double tempo = 0.0;
    juce::String meter;
    bool globalsExpanded = true;
    void paint(juce::Graphics& g) override
    {
        g.fillAll(colour(tokens::background));
        if (!globalsExpanded) return;
        const juce::String labels[] { "Tempo", "Signature", "Chords" };
        const juce::String values[] { juce::String(tempo, 1), meter, {} };
        const Icon icons[] { Icon::clock, Icon::signature, Icon::chords };
        g.setFont(font());
        for (int i = 0; i < 3; ++i)
        {
            auto bounds = juce::Rectangle<int>(8, 36 + i * 32, kHeaderWidth - 10, 32);
            g.setColour(colour(tokens::surface)); g.fillRect(bounds);
            g.setColour(colour(tokens::border)); g.drawRect(bounds);
            drawIcon(g, icons[i], bounds.withTrimmedLeft(24).withWidth(18).toFloat().withSizeKeepingCentre(17, 17), colour(tokens::muted));
            g.setColour(colour(tokens::text)); g.drawText(labels[i], bounds.withTrimmedLeft(54).withTrimmedRight(46), juce::Justification::centredLeft);
            g.setColour(colour(tokens::muted)); g.drawText(values[i], bounds.removeFromRight(48).reduced(5, 0), juce::Justification::centredRight);
        }
    }
};
class MusicalEventsOverlay final : public juce::Component
{
public:
    explicit MusicalEventsOverlay(const TimeViewport& view) : view_(view) { setInterceptsMouseClicks(false, false); setAccessible(false); }
    std::vector<std::pair<double, juce::String>> tempos, signatures;
    void paint(juce::Graphics& g) override
    {
        g.setFont(font(tokens::fontSmall)); g.setColour(colour(tokens::muted));
        for (const auto& [beat, label] : tempos)
        {
            const auto x = view_.xForBeat(beat);
            if (x >= -50.0f && x < static_cast<float>(getWidth())) g.drawText(label, juce::Rectangle<float>(x + 5.0f, 1.0f, 48.0f, 13.0f), juce::Justification::left);
        }
        for (const auto& [beat, label] : signatures)
        {
            const auto x = view_.xForBeat(beat);
            if (beat > 0.0 && x >= -50.0f && x < static_cast<float>(getWidth())) g.drawText(label, juce::Rectangle<float>(x + 5.0f, 32.0f, 48.0f, 32.0f), juce::Justification::centredLeft);
        }
    }
private:
    const TimeViewport& view_;
};
class CycleOverlay final : public juce::Component
{
public:
    explicit CycleOverlay(const TimeViewport& view) : view_(view) { setInterceptsMouseClicks(false, false); setAccessible(false); }
    void setRange(double first, double last, bool active) { first_ = first; last_ = last; active_ = active; repaint(); }
    void paint(juce::Graphics& g) override
    {
        if (!active_ || last_ <= first_) return;
        const auto firstX = view_.xForBeat(first_), lastX = view_.xForBeat(last_);
        g.setColour(colour(tokens::olive).withAlpha(0.6f));
        g.fillRect(firstX, static_cast<float>(getHeight() - 4), lastX - firstX, 3.0f);
    }
private:
    const TimeViewport& view_;
    double first_ = 0.0, last_ = 0.0;
    bool active_ = false;
};
class ScrollViewport final : public juce::Viewport
{
public:
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    { setViewPosition(0, getViewPositionY() - juce::roundToInt(wheel.deltaY * 220.0f)); }
};
}

class NativeStudioView::Content final : public juce::Component
{
public:
    struct Track
    {
        juce::String id, name, parent;
        bool collapsed = false, muted = false, solo = false, audioMuted = false, notesMuted = false;
        juce::var source;
    };
    struct Note
    {
        int localId = -1;
        juce::String id;
        double x = 0.0, width = 0.0, pitch = 60.0;
        juce::var source;
    };
    class Row;
    class ClipItem final : public juce::Component, private juce::KeyListener
    {
    public:
        ClipItem(Content& content, const juce::String& track, const juce::var& clip, juce::Colour tint)
            : content_(content), track_(track), id_(clip["id"].toString()), start_(std::max(0.0, number(clip["start"]))),
              duration_(std::max(0.0, number(clip["duration"]))), audio_(localView_)
        {
            setComponentID(id_);
            addAndMakeVisible(audio_);
            audio_.setComponentID("studio.clip." + id_);
            const auto sourcePeaks = decodePeaks(clip["waveform"]);
            const auto offset = std::max(0.0, number(clip["offset"]));
            const auto sourceDuration = number(clip["waveformDurationSeconds"], number(clip["sourceDurationSeconds"], offset + duration_));
            AudioClipData data;
            data.name = clip["name"].toString(); data.colour = tint;
            data.startBeat = content_.tempoMap_.beatAt(start_);
            data.lengthBeats = content_.tempoMap_.beatAt(start_ + duration_) - data.startBeat;
            if (!sourcePeaks.empty() && sourceDuration > 0.0 && data.lengthBeats > 0.0)
            {
                // Audio peaks are uniform in source seconds, whereas AudioClip
                // renders uniform beats. Reduce each beat bin over its source
                // time interval so tempo changes preserve transient positions.
                const auto count = sourcePeaks.size();
                const auto targetCount = std::clamp(static_cast<std::size_t>(std::ceil(static_cast<double>(count) * std::min(1.0, duration_ / sourceDuration))), std::size_t{2}, std::max(std::size_t{2}, count));
                data.peaks.reserve(targetCount);
                for (std::size_t i = 0; i < targetCount; ++i)
                {
                    const auto fromBeat = data.startBeat + data.lengthBeats * static_cast<double>(i) / static_cast<double>(targetCount);
                    const auto toBeat = data.startBeat + data.lengthBeats * static_cast<double>(i + 1) / static_cast<double>(targetCount);
                    const auto sourceFrom = offset + content_.tempoMap_.secondsAt(fromBeat) - start_;
                    const auto sourceTo = offset + content_.tempoMap_.secondsAt(toBeat) - start_;
                    const auto first = std::min(count - 1, static_cast<std::size_t>(std::floor(std::clamp(sourceFrom / sourceDuration, 0.0, 1.0) * static_cast<double>(count))));
                    const auto last = std::clamp(static_cast<std::size_t>(std::ceil(std::clamp(sourceTo / sourceDuration, 0.0, 1.0) * static_cast<double>(count))), first + 1, count);
                    float peak = 0.0f;
                    for (auto sourceIndex = first; sourceIndex < last; ++sourceIndex) peak = std::max(peak, sourcePeaks[sourceIndex]);
                    data.peaks.push_back(peak);
                }
            }
            localView_.firstBeat = data.startBeat; localView_.pixelsPerBeat = content_.view_.pixelsPerBeat;
            audio_.setData(std::move(data));
            audio_.onSelect = [this] { content_.selectClip(track_, id_); };
            audio_.onSeek = [this](double beat) { content_.seek(beat); };
            audio_.addMouseListener(this, false);
            audio_.addKeyListener(this);
        }
        ~ClipItem() override { audio_.removeMouseListener(this); audio_.removeKeyListener(this); }
        void refresh()
        {
            localView_.pixelsPerBeat = content_.view_.pixelsPerBeat;
            const auto startBeat = content_.tempoMap_.beatAt(start_);
            const auto endBeat = content_.tempoMap_.beatAt(start_ + duration_);
            setBounds(juce::roundToInt(content_.view_.xForBeat(startBeat)), 0,
                      std::max(1, juce::roundToInt((endBeat - startBeat) * content_.view_.pixelsPerBeat)), kRowHeight - 2);
            audio_.viewportChanged();
        }
        void setSelected(bool selected) { audio_.setSelected(selected); }
        void syncPosition() { audio_.setCurrentBeat(content_.playheadBeat_); }
        void resized() override { audio_.setBounds(getLocalBounds()); }
    private:
        using juce::Component::keyPressed;
        bool keyPressed(const juce::KeyPress& key, juce::Component*) override { return content_.arrangementKey(key); }
        void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
        { content_.arrangementWheel(event, wheel, static_cast<float>(getX()) + event.position.x); }
        void mouseDown(const juce::MouseEvent& event) override
        {
            dragOrigin_ = event.getScreenPosition(); moved_ = false;
            if (event.mods.isPopupMenu()) content_.showClipMenu(track_, id_, audio_);
        }
        void mouseDrag(const juce::MouseEvent& event) override
        {
            const auto delta = event.getScreenPosition().x - dragOrigin_.x;
            if (std::abs(delta) < 4 && !moved_) return;
            moved_ = true;
            const auto beat = content_.tempoMap_.beatAt(start_) + static_cast<double>(delta) / content_.view_.pixelsPerBeat;
            setTopLeftPosition(juce::roundToInt(content_.view_.xForBeat(content_.snapBeat(beat))), getY());
        }
        void mouseUp(const juce::MouseEvent& event) override
        {
            if (!moved_) return;
            moved_ = false;
            const auto beat = content_.snapBeat(content_.tempoMap_.beatAt(start_) + static_cast<double>(event.getScreenPosition().x - dragOrigin_.x) / content_.view_.pixelsPerBeat);
            const auto screenPoint = event.getScreenPosition();
            const auto track = content_.trackAt(content_.getLocalPoint(nullptr, screenPoint));
            auto payload = command("move-arrangement-clip", { { "track", track_ }, { "clipId", id_ },
                { "targetTrack", track.isNotEmpty() ? track : track_ }, { "start", content_.tempoMap_.secondsAt(beat) } });
            content_.send(payload);
        }
        Content& content_;
        juce::String track_, id_;
        double start_, duration_;
        TimeViewport localView_;
        AudioClip audio_;
        juce::Point<int> dragOrigin_;
        bool moved_ = false;
    };
    class Row final : public juce::Component
    {
    public:
        Row(Content& content, const Track& track, int index) : content_(content), track_(track), grid_(content.view_)
        {
            setComponentID("studio.track." + track.id);
            addAndMakeVisible(header_); addAndMakeVisible(body_); body_.addAndMakeVisible(grid_);
            header_.setTrack(index + 1, track.name, trackColour(track.id, track.parent.isNotEmpty()),
                track.id == "Instrumental" ? Icon::waveform : track.parent.isEmpty() ? Icon::microphone : Icon::backing);
            header_.setMuted(track.muted); header_.setSolo(track.solo);
            header_.getMuteButton().setComponentID("studio.mute." + track.id);
            header_.getSoloButton().setComponentID("studio.solo." + track.id);
            header_.getGainSlider().setComponentID("studio.gain." + track.id);
            header_.getGainSlider().setEnabled(track.id == "Instrumental");
            header_.getGainSlider().setDescription(track.id == "Instrumental" ? "Instrumental gain" : "This track has no independent gain control.");
            header_.setGain(track.id == "Instrumental" ? 20.0 * std::log10(std::max(0.0001, content.instrumentalVolume_ / 100.0)) : 0.0);
            header_.onMute = [this](bool value) { content_.setTrackState(track_.id, value, track_.solo); };
            header_.onSolo = [this](bool value) { content_.setTrackState(track_.id, track_.muted, value); };
            header_.onGain = [this](double db) { content_.send(command("set-instrumental-volume", { { "value", std::clamp(std::pow(10.0, db / 20.0) * 100.0, 0.0, 100.0) } })); };
            header_.onMenu = [this] { content_.showTrackMenu(track_.id, header_); };
            header_.addMouseListener(this, true);
            grid_.onSeek = [this](double beat) { content_.seek(beat); };
            grid_.addMouseListener(this, false);
            for (const auto& value : array(track.source["clips"]))
            {
                auto clip = std::make_unique<ClipItem>(content, track.id, value, trackColour(track.id, track.parent.isNotEmpty()));
                body_.addAndMakeVisible(*clip); clips_.push_back(std::move(clip));
            }
        }
        ~Row() override { header_.removeMouseListener(this); grid_.removeMouseListener(this); }
        void resized() override
        {
            header_.setBounds(8, 0, kHeaderWidth - 10, getHeight() - 2);
            body_.setBounds(getLocalBounds().withTrimmedLeft(kHeaderWidth)); grid_.setBounds(body_.getLocalBounds()); refresh();
        }
        void refresh()
        {
            grid_.viewportChanged(); grid_.setCurrentBeat(content_.playheadBeat_);
            for (auto& clip : clips_) clip->refresh();
        }
        void syncPosition()
        {
            grid_.setCurrentBeat(content_.playheadBeat_);
            for (auto& clip : clips_) clip->syncPosition();
        }
        void updateState(const Track& track)
        {
            track_ = track; header_.setMuted(track.muted); header_.setSolo(track.solo); repaint();
        }
        void updateSelection()
        {
            for (auto& clip : clips_) clip->setSelected(content_.selectedTrack_ == track_.id && content_.selectedClip_ == clip->getComponentID());
            repaint();
        }
        void paintOverChildren(juce::Graphics& g) override
        {
            if (content_.selectedTrack_ == track_.id)
            { g.setColour(trackColour(track_.id, track_.parent.isNotEmpty()).withAlpha(0.2f)); g.drawRect(8, 0, kHeaderWidth - 10, getHeight() - 2, 1); }
        }
        juce::String id() const { return track_.id; }
        TrackHeader& header() { return header_; }
    private:
        void mouseDown(const juce::MouseEvent& event) override
        {
            if (event.eventComponent == &grid_ || header_.isParentOf(event.eventComponent) || event.eventComponent == &header_) content_.selectTrack(track_.id);
        }
        void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
        { if (event.eventComponent == &grid_) content_.arrangementWheel(event, wheel, event.position.x); }
        Content& content_; Track track_; TrackHeader header_; juce::Component body_; TimelineGrid grid_;
        std::vector<std::unique_ptr<ClipItem>> clips_;
    };

    Content(NativeStudioView& owner, CommandHandler handler) : owner_(owner), handler_(std::move(handler)),
        ruler_(view_), tempo_(view_), signature_(view_), chords_(view_), arrangementPlayhead_(view_), musicalEvents_(view_), cycleOverlay_(view_),
        lowerRuler_(lowerView_), pitch_(lowerView_), lowerPlayhead_(lowerView_)
    {
        view_.firstBeat = 0.0; view_.pixelsPerBeat = 40.0; lowerView_ = view_;
        for (auto* child : std::initializer_list<juce::Component*> { &transport_, &scroll_, &toolbar_, &lowerRuler_, &keyboard_, &pitch_, &lowerPlayhead_, &horizontal_, &status_ }) addAndMakeVisible(child);
        scroll_.setViewedComponent(&arrangement_, false); scroll_.setScrollBarsShown(false, false);
        for (auto* child : std::initializer_list<juce::Component*> { &ruler_, &tempo_, &signature_, &chords_, &global_, &addTrack_, &arrangementPlayhead_, &musicalEvents_, &cycleOverlay_ }) arrangement_.addAndMakeVisible(child);
        transport_.setComponentID("studio.transport"); ruler_.setComponentID("studio.ruler"); lowerRuler_.setComponentID("studio.pitchRuler"); pitch_.setComponentID("studio.pitch");
        transport_.setProjectTitle("Obsidian"); transport_.setRecordingAvailable(false);
        transport_.setProjectActions({ "New project", "Open project…", "Save project", "Import vocal audio…", "Import instrumental…", "Load annotation JSON…", "Load lyrics…", "Analyze selected track", "Analyze AI parts", "Generate backing", "Render backing audio", "Undo", "Redo", "Export all tracks…", "Export MIDI…", "Validate", "Audio and voice settings…", "Set loop start at playhead", "Set loop end at playhead", "Loop entire project" });
        transport_.onProjectAction = [this](int index)
        {
            static const char* actions[] { "new-project", "open-project", "save-project", "open-audio", "open-instrumental", "load-json", "load-lyrics", "analyze", "ai-parts", "generate-backing", "render-backing", "undo", "redo", "export-all-tracks", "export-midi", "validate" };
            if (index == 16) { const auto callback = owner_.onShowSettings; if (callback) callback(); }
            else if (index >= 17 && index <= 19) changeLoop(index - 17);
            else if (juce::isPositiveAndBelow(index, static_cast<int>(std::size(actions)))) send(command("project-action", { { "action", actions[index] } }));
        };
        transport_.onPlay = [this](bool play) { send(command("transport", { { "action", play ? "play" : "pause" } })); };
        transport_.onStop = [this] { send(command("transport", { { "action", "stop" } })); };
        transport_.onRewind = [this] { send(command("transport", { { "action", "return-to-start" } })); };
        transport_.onMasterVolume = [this](double value) { send(command("set-volume", { { "value", value * 100.0 } })); };
        transport_.onLoop = [this](bool active) { send(command("set-loop", { { "active", active }, { "range", loopRange_ } })); };
        global_.setVariant(Button::Variant::subtle); global_.setTextJustification(juce::Justification::centredLeft);
        global_.onClick = [this] { arrangement_.globalsExpanded = !arrangement_.globalsExpanded; global_.setIcon(arrangement_.globalsExpanded ? Icon::chevronDown : Icon::chevronRight); resized(); };
        addTrack_.setComponentID("studio.addTrack"); addTrack_.onClick = [this] { send(command("add-track")); };
        for (auto* item : std::initializer_list<juce::Component*> { &trackSelect_, &pointer_, &pencil_, &scissors_, &snap_, &delete_, &noteMenu_, &undo_, &redo_, &zoomOut_, &zoom_, &zoomIn_ }) toolbar_.addAndMakeVisible(item);
        trackSelect_.setComponentID("studio.trackSelect"); snap_.setComponentID("studio.snap");
        pointer_.setComponentID("studio.pointer"); pencil_.setComponentID("studio.pencil"); scissors_.setComponentID("studio.scissors"); delete_.setComponentID("studio.delete");
        trackSelect_.onChange = [this](int index) { if (juce::isPositiveAndBelow(index, static_cast<int>(tracks_.size()))) selectTrack(tracks_[static_cast<std::size_t>(index)].id); };
        pointer_.onClick = [this] { setTool(PitchTool::pointer); }; pencil_.onClick = [this] { setTool(PitchTool::pencil); }; scissors_.onClick = [this] { setTool(PitchTool::scissors); };
        setTool(PitchTool::pointer);
        snap_.setItems({ "Snap off", "1/4", "1/8", "1/16", "1/32" }); snap_.setSelectedIndex(3);
        snap_.onChange = [this](int index) { constexpr double steps[] {0.0, 1.0, 0.5, 0.25, 0.125}; snapBeats_ = steps[std::clamp(index, 0, 4)]; pitch_.setSnapBeats(snapBeats_); };
        delete_.onClick = [this] { deleteSelection(); };
        noteMenu_.onClick = [this] { showNoteMenu(); };
        undo_.onClick = [this] { send(command("pitch-history", { { "track", selectedTrack_ }, { "action", "undo" } })); };
        redo_.onClick = [this] { send(command("pitch-history", { { "track", selectedTrack_ }, { "action", "redo" } })); };
        zoom_.setRange(5.0, 160.0); zoom_.setValue(view_.pixelsPerBeat); zoom_.setComponentID("studio.zoom");
        zoom_.onValueChange = [this](double value) { gesture(0.0, value / view_.pixelsPerBeat, static_cast<float>(ruler_.getWidth()) * 0.5f); };
        zoomOut_.onClick = [this] { gesture(0, 0.8, static_cast<float>(ruler_.getWidth()) * 0.5f); }; zoomIn_.onClick = [this] { gesture(0, 1.25, static_cast<float>(ruler_.getWidth()) * 0.5f); };
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_, &lowerRuler_, &pitch_ })
        {
            surface->onSeek = [this](double beat) { seek(beat); };
            const auto lower = surface == &lowerRuler_ || surface == &pitch_;
            surface->onViewportGesture = [this, lower](double scroll, double factor, float anchor) { gesture(scroll, factor, anchor - (lower ? static_cast<float>(kHeaderWidth - kKeyboardWidth) : 0.0f)); };
        }
        pitch_.onNoteSelected = [this](int id) { selectNote(id); };
        pitch_.onNotesSelected = [this](const std::vector<int>& ids)
        {
            selectedClip_.clear();
            for (auto& row : rows_) row->updateSelection();
            selectedNotes_.clear();
            for (auto id : ids) if (const auto* note = findNote(id)) selectedNotes_.insert(note->id);
            const auto* primary = findNote(pitch_.selectedNote());
            selectedNote_ = primary != nullptr ? primary->id : juce::String();
            updateEnabled();
        };
        pitch_.onEditRequested = [this](PitchTool tool, int id, double beat, int midi) { editNote(tool, id, beat, midi); };
        pitch_.onNotesMoved = [this](const std::vector<PitchEditor::NoteMovement>& movements)
        {
            juce::Array<juce::var> changes;
            for (const auto& movement : movements)
            {
                const auto* note = findNote(movement.id); if (note == nullptr) continue;
                const auto pitch = std::clamp(note->pitch + static_cast<double>(movement.midiNote - juce::roundToInt(note->pitch)), 0.0, 127.0);
                const auto startDelta = tempoMap_.secondsAt(movement.startBeat) - duration_ * note->x / 100.0;
                const auto endDelta = tempoMap_.secondsAt(movement.startBeat + movement.lengthBeats) - duration_ * (note->x + note->width) / 100.0;
                changes.add(object({ { "clipId", note->id }, { "startDeltaSeconds", startDelta }, { "endDeltaSeconds", endDelta }, { "pitch", pitch } }));
            }
            if (!changes.isEmpty()) send(command("move-note-group", { { "track", selectedTrack_ }, { "notes", juce::var(changes) }, { "snapToChord", snapToChord_ } }));
        };
        horizontal_.setRange(0.0, 1.0); horizontal_.onValueChange = [this](double first) { view_.firstBeat = first; refreshViewport(); };
        status_.setText("Open a project or import audio to begin.", juce::dontSendNotification); status_.setColour(juce::Label::textColourId, colour(tokens::muted)); status_.setFont(font(tokens::fontSmall));
        setWantsKeyboardFocus(true);
        pitch_.setNoteRange(46, 75); keyboard_.setNoteRange(46, 75);
        loopRange_ = object({ { "start", 0.0 }, { "end", 25.0 } });
        updateEnabled();
    }
    ~Content() override { scroll_.setViewedComponent(nullptr, false); }

    void send(const juce::var& payload)
    {
        // Backend commands may synchronously replace every arrangement row.
        // Deliver after the originating control has finished its mouse/key event.
        const juce::Component::SafePointer<NativeStudioView> safeOwner(&owner_);
        juce::MessageManager::callAsync([safeOwner, payload]
        {
            if (safeOwner == nullptr) return;
            const auto callback = safeOwner->content_->handler_;
            if (callback) callback(payload);
        });
    }
    double percentToBeat(double percent) const { return tempoMap_.beatAt(duration_ * percent / 100.0); }
    double beatToPercent(double beat) const { return duration_ > 0.0 ? std::clamp(tempoMap_.secondsAt(beat) * 100.0 / duration_, 0.0, 100.0) : 0.0; }
    double snapBeat(double beat) const { return std::max(0.0, snapBeats_ > 0.0 ? std::round(beat / snapBeats_) * snapBeats_ : beat); }
    void seek(double beat)
    {
        if (duration_ <= 0.0) return;
        updatePosition(beatToPercent(beat));
        send(command("set-playhead", { { "position", beatToPercent(beat) } }));
    }
    void gesture(double scroll, double factor, float anchor)
    {
        if (factor != 1.0) view_.zoomAt(factor, anchor);
        if (scroll != 0.0) view_.scroll(scroll);
        refreshViewport();
    }
    void refreshViewport()
    {
        lowerView_ = view_;
        lowerView_.firstBeat = view_.firstBeat - static_cast<double>(kHeaderWidth - kKeyboardWidth) / view_.pixelsPerBeat;
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_, &lowerRuler_, &pitch_ }) surface->viewportChanged();
        for (auto& row : rows_) row->refresh();
        arrangementPlayhead_.viewportChanged(); lowerPlayhead_.viewportChanged(); musicalEvents_.repaint();
        cycleOverlay_.setRange(percentToBeat(number(loopRange_["start"])), percentToBeat(number(loopRange_["end"])), loopActive_);
        zoom_.setValue(view_.pixelsPerBeat);
        horizontal_.setRange(0.0, std::max(1.0, tempoMap_.beatAt(duration_))); horizontal_.setValue(view_.firstBeat);
    }
    void updatePosition(double percent)
    {
        playheadPercent_ = std::clamp(percent, 0.0, 100.0); playheadBeat_ = percentToBeat(playheadPercent_);
        const auto seconds = duration_ * playheadPercent_ / 100.0;
        transport_.setPosition(playheadBeat_, seconds);
        transport_.setTempo(tempoMap_.tempoAt(seconds));
        const auto position = view_.positionAt(playheadBeat_);
        transport_.setSignature(position.numerator, position.denominator);
        arrangementPlayhead_.setBeat(playheadBeat_); lowerPlayhead_.setBeat(playheadBeat_);
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_, &lowerRuler_, &pitch_ }) surface->setCurrentBeat(playheadBeat_);
        for (auto& row : rows_) row->syncPosition();
    }
    const Track* findTrack(const juce::String& id) const
    {
        const auto found = std::find_if(tracks_.begin(), tracks_.end(), [&](const auto& track) { return track.id == id; });
        return found == tracks_.end() ? nullptr : &*found;
    }
    const Note* findNote(int id) const
    {
        const auto found = std::find_if(notes_.begin(), notes_.end(), [id](const auto& note) { return note.localId == id; });
        return found == notes_.end() ? nullptr : &*found;
    }
    void changeLoop(int boundary)
    {
        if (duration_ <= 0.0) return;
        auto start = std::clamp(number(loopRange_["start"]), 0.0, 99.99);
        auto end = std::clamp(number(loopRange_["end"], 100.0), start + 0.01, 100.0);
        if (boundary == 0) { start = std::min(99.99, playheadPercent_); end = std::max(end, start + 0.01); }
        else if (boundary == 1) { end = std::max(0.01, playheadPercent_); start = std::min(start, end - 0.01); }
        else { start = 0.0; end = 100.0; }
        send(command("set-loop", { { "active", true }, { "range", object({ { "start", start }, { "end", end } }) } }));
    }
    void setTrackState(const juce::String& id, bool muted, bool solo)
    { send(command("set-track-state", { { "track", id }, { "state", object({ { "mute", muted }, { "solo", solo } }) } })); }
    void selectTrack(const juce::String& id, bool notify = true)
    {
        if (findTrack(id) == nullptr) return;
        if (selectedTrack_ != id) { selectedTrack_ = id; selectedClip_.clear(); selectedNote_.clear(); selectedNotes_.clear(); rebuildPitch(); }
        for (std::size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].id == id) trackSelect_.setSelectedIndex(static_cast<int>(i));
        for (auto& row : rows_) row->updateSelection();
        updateEnabled();
        if (notify) send(command("select-tracks", { { "tracks", juce::var(juce::Array<juce::var>{ juce::var(id) }) } }));
    }
    void selectClip(const juce::String& track, const juce::String& clip)
    {
        selectTrack(track);
        selectedNotes_.clear(); selectedNote_.clear(); pitch_.setSelectedNotes({});
        selectedClip_ = clip;
        for (auto& row : rows_) row->updateSelection();
        updateEnabled();
    }
    void selectNote(int id)
    {
        selectedClip_.clear();
        for (auto& row : rows_) row->updateSelection();
        const auto* note = findNote(id);
        selectedNote_ = note != nullptr ? note->id : juce::String();
        selectedNotes_.clear(); if (note != nullptr) selectedNotes_.insert(note->id);
        updateEnabled();
    }
    void setTool(PitchTool tool)
    {
        pitch_.setTool(tool); pointer_.setSelected(tool == PitchTool::pointer); pencil_.setSelected(tool == PitchTool::pencil); scissors_.setSelected(tool == PitchTool::scissors);
    }
    void editNote(PitchTool tool, int id, double beat, int midi)
    {
        if (!pitchTrackAvailable()) return;
        if (tool == PitchTool::pencil)
        {
            const auto start = beatToPercent(beat), end = beatToPercent(beat + std::max(0.125, snapBeats_));
            send(command("add-clip", { { "track", selectedTrack_ }, { "snapToChord", snapToChord_ },
                { "clip", object({ { "id", juce::Uuid().toString() }, { "x", start }, { "width", std::max(0.001, end - start) }, { "pitch", midi } }) } }));
        }
        else if (tool == PitchTool::scissors)
        {
            const auto* note = findNote(id);
            if (note == nullptr) return;
            send(command("split-clip", { { "track", selectedTrack_ }, { "clipId", note->id }, { "x", beatToPercent(beat) }, { "rightClipId", juce::Uuid().toString() }, { "snapToChord", snapToChord_ } }));
        }
    }
    bool pitchTrackAvailable() const
    {
        if (selectedTrack_ == "Voice Main") return true;
        for (const auto& content : array(project_["backingTrackContents"])) if (content["id"].toString() == selectedTrack_) return true;
        return false;
    }
    void deleteSelection()
    {
        if (!selectedNotes_.empty())
        {
            juce::Array<juce::var> ids; for (const auto& id : selectedNotes_) ids.add(id);
            send(command("delete-clips", { { "track", selectedTrack_ }, { "clipIds", juce::var(ids) } }));
        }
        else if (selectedClip_.isNotEmpty()) send(command("remove-arrangement-clip", { { "track", selectedTrack_ }, { "clipId", selectedClip_ } }));
    }
    void shiftNotes(double semitones)
    {
        juce::Array<juce::var> changes;
        for (const auto& note : notes_) if (selectedNotes_.contains(note.id)) changes.add(object({ { "clipId", note.id }, { "pitch", std::clamp(note.pitch + semitones, 0.0, 127.0) } }));
        if (!changes.isEmpty()) send(command("move-clips", { { "track", selectedTrack_ }, { "clips", juce::var(changes) }, { "snapToChord", snapToChord_ } }));
    }
    void selectAllNotes()
    {
        std::vector<int> ids; selectedNotes_.clear(); selectedClip_.clear();
        for (auto& row : rows_) row->updateSelection();
        for (const auto& note : notes_) { selectedNotes_.insert(note.id); ids.push_back(note.localId); }
        selectedNote_ = notes_.empty() ? juce::String() : notes_.back().id;
        pitch_.setSelectedNotes(std::move(ids)); updateEnabled();
    }
    void showNoteMenu()
    {
        const auto* selected = findNote(pitch_.selectedNote());
        const auto id = selected != nullptr ? selected->id : juce::String();
        std::vector<Menu::Item> items;
        items.push_back({ "Snap pitch edits to chord", [this] { snapToChord_ = !snapToChord_; }, true, snapToChord_ });
        items.push_back({ "Select all notes", [this] { selectAllNotes(); }, !notes_.empty() });
        items.push_back({ "Move selected up one semitone", [this] { shiftNotes(1.0); }, !selectedNotes_.empty() });
        items.push_back({ "Move selected down one semitone", [this] { shiftNotes(-1.0); }, !selectedNotes_.empty() });
        items.push_back({ "Delete selected notes", [this] { deleteSelection(); }, !selectedNotes_.empty() });
        if (selected != nullptr)
        {
            const auto index = static_cast<std::size_t>(selected - notes_.data());
            const auto nextId = index + 1 < notes_.size() ? notes_[index + 1].id : juce::String();
            items.push_back({ "Join with next note", [this, id, nextId] { send(command("join-clips", { { "track", selectedTrack_ }, { "clipIds", juce::var(juce::Array<juce::var>{ juce::var(id), juce::var(nextId) }) }, { "snapToChord", snapToChord_ } })); }, nextId.isNotEmpty() });
            for (auto gain : { -6.0, -3.0, 0.0, 3.0, 6.0 })
                items.push_back({ "Note gain " + juce::String(gain, 0) + " dB", [this, id, gain] { send(command("set-clip-gain", { { "track", selectedTrack_ }, { "clipId", id }, { "gainDb", gain } })); } });
            for (auto scale : { 0.0, 0.5, 1.0, 1.5 })
                items.push_back({ "Vibrato " + juce::String(scale * 100.0, 0) + "%", [this, id, scale] { send(command("set-clip-vibrato", { { "track", selectedTrack_ }, { "clipId", id }, { "scale", scale } })); } });
        }
        menu_.setItems(std::move(items)); menu_.showFor(noteMenu_, 260);
    }
    void showTrackMenu(const juce::String& id, juce::Component& anchor)
    {
        const auto* track = findTrack(id); if (track == nullptr) return;
        const auto audioMuted = track->audioMuted, notesMuted = track->notesMuted;
        std::vector<Menu::Item> items {
            { "Import audio…", [this, id] { send(command("import-track-audio", { { "track", id } })); } },
            { "Audio layer", [this, id, audioMuted, notesMuted] { send(command("set-track-layer-state", { { "track", id }, { "state", object({ { "audioMuted", !audioMuted }, { "notesMuted", notesMuted } }) } })); }, true, !audioMuted },
            { "Note layer", [this, id, audioMuted, notesMuted] { send(command("set-track-layer-state", { { "track", id }, { "state", object({ { "audioMuted", audioMuted }, { "notesMuted", !notesMuted } }) } })); }, id != "Instrumental", !notesMuted },
            { "Analyze selected track", [this, id] { selectTrack(id); send(command("project-action", { { "action", "analyze" } })); }, id != "Instrumental" },
            { "Add backing / voice settings…", [this, id] { selectTrack(id); const auto callback = owner_.onShowSettings; if (callback) callback(); }, id != "Instrumental" }
        };
        if (track->parent.isNotEmpty())
        {
            items.push_back({ "Regenerate backing", [this, id] { send(command("regenerate-backing-track", { { "track", id }, { "generator", "local" } })); } });
            items.push_back({ "Render backing audio", [this, id] { send(command("render-backing-track", { { "track", id } })); } });
            items.push_back({ "Remove backing track", [this, id] { send(command("remove-backing-track", { { "track", id } })); } });
        }
        else if (id != "Instrumental") items.push_back({ "Remove track", [this, id] { send(command("remove-track", { { "track", id } })); } });
        menu_.setItems(std::move(items)); menu_.showFor(anchor, 260);
    }
    void showClipMenu(const juce::String& track, const juce::String& clip, juce::Component& anchor)
    {
        menu_.setItems({ { "Select track", [this, track] { selectTrack(track); } },
            { "Remove audio clip", [this, track, clip] { send(command("remove-arrangement-clip", { { "track", track }, { "clipId", clip } })); } } });
        menu_.showFor(anchor, 230);
    }
    juce::String trackAt(juce::Point<int> position) const
    {
        const auto point = arrangement_.getLocalPoint(this, position);
        for (const auto& row : rows_) if (row->isVisible() && row->getBounds().contains(point)) return row->id();
        return {};
    }
    void importFiles(const juce::StringArray& paths, juce::Point<int> position)
    {
        juce::Array<juce::var> files;
        for (const auto& path : paths) if (juce::File::isAbsolutePath(path)) files.add(path);
        if (files.isEmpty()) return;
        auto track = trackAt(position); if (track.isEmpty()) track = selectedTrack_;
        const auto local = arrangement_.getLocalPoint(this, position);
        const auto seconds = tempoMap_.secondsAt(snapBeat(view_.beatAtX(static_cast<float>(std::max(0, local.x - kHeaderWidth)))));
        send(command("import-dropped-audio", { { "track", track }, { "start", seconds }, { "files", juce::var(files) } }));
    }
    void rebuildRows()
    {
        juce::String focusId;
        if (auto* focused = juce::Component::getCurrentlyFocusedComponent(); focused != nullptr && arrangement_.isParentOf(focused)) focusId = focused->getComponentID();
        menu_.dismiss();
        rows_.clear(); groups_.clear();
        for (std::size_t index = 0; index < tracks_.size(); ++index)
        {
            auto row = std::make_unique<Row>(*this, tracks_[index], static_cast<int>(index));
            arrangement_.addAndMakeVisible(*row); rows_.push_back(std::move(row));
            const auto& track = tracks_[index];
            if (std::any_of(tracks_.begin(), tracks_.end(), [&](const auto& child) { return child.parent == track.id; }))
            {
                auto group = std::make_unique<TrackGroupHeader>();
                group->setTitle(track.name + " · Backing Vocals"); group->setExpanded(!track.collapsed);
                group->onExpanded = [this, id = track.id](bool expanded) { send(command("set-track-collapsed", { { "track", id }, { "collapsed", !expanded } })); };
                group->onAdd = [this, id = track.id] { selectTrack(id); const auto callback = owner_.onShowSettings; if (callback) callback(); };
                arrangement_.addAndMakeVisible(*group); groups_.emplace(track.id, std::move(group));
            }
        }
        for (auto& row : rows_) row->updateSelection();
        resized();
        if (focusId.isNotEmpty())
        {
            const auto restore = [&](const auto& self, juce::Component& component) -> bool
            {
                if (component.getComponentID() == focusId && component.isShowing() && component.isEnabled() && component.getWantsKeyboardFocus()) { component.grabKeyboardFocus(); return true; }
                for (auto* child : component.getChildren()) if (self(self, *child)) return true;
                return false;
            };
            restore(restore, arrangement_);
        }
    }
    void rebuildPitch()
    {
        notes_.clear();
        juce::var noteValues;
        if (selectedTrack_ == "Voice Main") noteValues = project_["clips"];
        else for (const auto& track : array(project_["backingTrackContents"])) if (track["id"].toString() == selectedTrack_) { noteValues = track["clips"]; break; }
        std::vector<NoteSegment> segments;
        int minimum = 127, maximum = 0;
        for (const auto& value : array(noteValues))
        {
            Note note;
            note.id = value["id"].toString(); if (note.id.isEmpty()) continue;
            const auto key = selectedTrack_ + "\n" + note.id;
            auto found = noteIds_.find(key);
            if (found == noteIds_.end()) found = noteIds_.emplace(key, nextNoteId_++).first;
            note.localId = found->second; note.x = number(value["x"]); note.width = number(value["width"]); note.pitch = std::clamp(number(value["pitch"], 60.0), 0.0, 127.0); note.source = value;
            if (note.width <= 0.0) continue;
            NoteSegment segment;
            segment.id = note.localId; segment.startBeat = percentToBeat(note.x); segment.lengthBeats = percentToBeat(note.x + note.width) - segment.startBeat;
            segment.midiNote = juce::roundToInt(note.pitch); segment.colour = trackColour(selectedTrack_, selectedTrack_ != "Voice Main");
            minimum = std::min(minimum, segment.midiNote); maximum = std::max(maximum, segment.midiNote);
            // Wire curves have absolute, irregular x positions. Resample in time
            // here because PitchEditor deliberately accepts uniform offsets.
            std::vector<std::pair<double, double>> curve;
            for (const auto& point : array(value["pitchCurve"])) curve.emplace_back(number(point["x"]), number(point["pitch"], note.pitch));
            std::sort(curve.begin(), curve.end());
            const auto count = std::clamp(static_cast<int>(curve.size()) * 2, 2, 256);
            segment.pitchOffsets.reserve(static_cast<std::size_t>(count));
            std::size_t cursor = 0;
            for (int i = 0; i < count; ++i)
            {
                const auto beat = segment.startBeat + segment.lengthBeats * static_cast<double>(i) / static_cast<double>(count - 1);
                const auto x = beatToPercent(beat);
                double pitchValue = note.pitch;
                if (!curve.empty())
                {
                    while (cursor + 1 < curve.size() && curve[cursor + 1].first < x) ++cursor;
                    if (x <= curve.front().first) pitchValue = curve.front().second;
                    else if (cursor + 1 < curve.size())
                    {
                        const auto& a = curve[cursor]; const auto& b = curve[cursor + 1];
                        const auto fraction = std::clamp((x - a.first) / std::max(0.000001, b.first - a.first), 0.0, 1.0);
                        pitchValue = a.second + (b.second - a.second) * fraction;
                    }
                    else pitchValue = curve.back().second;
                }
                segment.pitchOffsets.push_back(static_cast<float>(pitchValue - static_cast<double>(segment.midiNote)));
            }
            notes_.push_back(std::move(note)); segments.push_back(std::move(segment));
        }
        std::sort(notes_.begin(), notes_.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
        if (segments.empty()) { minimum = 48; maximum = 72; }
        const auto low = std::max(0, minimum - 5), high = std::min(127, std::max(maximum + 5, low + 23));
        pitch_.setNoteRange(low, high); keyboard_.setNoteRange(low, high); pitch_.setNotes(std::move(segments));
        std::vector<int> selectedLocals;
        std::set<juce::String> surviving;
        for (const auto& note : notes_) if (selectedNotes_.contains(note.id)) { selectedLocals.push_back(note.localId); surviving.insert(note.id); }
        selectedNotes_ = std::move(surviving);
        if (!selectedNotes_.contains(selectedNote_)) selectedNote_ = selectedNotes_.empty() ? juce::String() : *selectedNotes_.begin();
        pitch_.setSelectedNotes(std::move(selectedLocals));
        updateEnabled();
    }
    void updateEnabled()
    {
        const auto editable = pitchTrackAvailable() && duration_ > 0.0;
        pencil_.setEnabled(editable); scissors_.setEnabled(editable); pitch_.setEnabled(duration_ > 0.0);
        if (!editable) setTool(PitchTool::pointer);
        delete_.setEnabled(!selectedNotes_.empty() || selectedClip_.isNotEmpty()); noteMenu_.setEnabled(editable);
        bool canUndo = false, canRedo = false;
        for (const auto& item : array(project_["pitchEditorHistory"])) if (item["track"].toString() == selectedTrack_) { canUndo = static_cast<bool>(item["canUndo"]); canRedo = static_cast<bool>(item["canRedo"]); }
        undo_.setEnabled(canUndo); redo_.setEnabled(canRedo);
    }
    void dispatch(const juce::var& event)
    {
        const auto type = event["type"].toString();
        if (type == "project-state")
        {
            project_ = event["project"]; tempoMap_.set(project_);
            duration_ = std::max(0.0, number(project_["timelineDurationSeconds"]));
            arrangement_.tempo = number(project_["tempo"], 120.0); arrangement_.meter = project_["meter"].toString();
            auto title = project_["title"].toString(); if (title.isEmpty()) title = project_["name"].toString();
            transport_.setProjectTitle(title.isNotEmpty() ? title : "Obsidian"); transport_.setTempo(arrangement_.tempo);
            view_.signatures.clear();
            for (const auto& item : array(project_["timeSignatures"]))
            {
                const auto numerator = static_cast<int>(number(item["numerator"], 4)), denominator = static_cast<int>(number(item["denominator"], 4));
                if (numerator >= 1 && numerator <= 32 && denominator >= 1 && denominator <= 64 && (denominator & (denominator - 1)) == 0)
                    view_.signatures.push_back({ tempoMap_.beatAt(number(item["start"])), numerator, denominator });
            }
            std::sort(view_.signatures.begin(), view_.signatures.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
            transport_.setTimeSignatures(view_.signatures);
            if (!view_.signatures.empty()) { transport_.setSignature(view_.signatures.front().numerator, view_.signatures.front().denominator); signature_.setSignature(view_.signatures.front().numerator, view_.signatures.front().denominator); }
            tempo_.setTempo(arrangement_.tempo);
            std::vector<double> tempoPoints;
            for (const auto& item : array(project_["tempoSegments"])) tempoPoints.push_back(tempoMap_.beatAt(number(item["start"])));
            tempo_.setPointBeats(std::move(tempoPoints));
            musicalEvents_.tempos.clear(); musicalEvents_.signatures.clear();
            for (const auto& item : array(project_["tempoSegments"])) musicalEvents_.tempos.emplace_back(tempoMap_.beatAt(number(item["start"])), juce::String(number(item["bpm"]), 1));
            for (const auto& signature : view_.signatures) musicalEvents_.signatures.emplace_back(signature.beat, juce::String(signature.numerator) + "/" + juce::String(signature.denominator));
            std::vector<ChordBlock> chords;
            double start = 0.0;
            for (const auto& item : array(project_["chords"]))
            {
                const auto width = std::max(0.0, number(item["width"]));
                chords.push_back({ percentToBeat(start), percentToBeat(start + width) - percentToBeat(start), item["label"].toString() }); start += width;
            }
            chords_.setChords(std::move(chords));
            tracks_.clear(); juce::StringArray names;
            for (const auto& item : array(project_["tracks"]))
            {
                Track track; track.id = item["id"].toString(); if (track.id.isEmpty()) continue;
                track.name = item["name"].toString(); if (track.name.isEmpty()) track.name = track.id;
                track.parent = item["parentId"].toString(); track.collapsed = static_cast<bool>(item["collapsed"]);
                track.muted = static_cast<bool>(item["mute"]); track.solo = static_cast<bool>(item["solo"]);
                track.audioMuted = static_cast<bool>(item["audioMuted"]); track.notesMuted = static_cast<bool>(item["notesMuted"]); track.source = item;
                for (const auto& entry : array(project_["initialTrackState"])) if (entry["track"].toString() == track.id) { track.muted = static_cast<bool>(entry["state"]["mute"]); track.solo = static_cast<bool>(entry["state"]["solo"]); }
                for (const auto& entry : array(project_["initialTrackLayerState"])) if (entry["track"].toString() == track.id) { track.audioMuted = static_cast<bool>(entry["state"]["audioMuted"]); track.notesMuted = static_cast<bool>(entry["state"]["notesMuted"]); }
                names.add(track.name); tracks_.push_back(std::move(track));
            }
            const auto selectCreatedBacking = pendingTrack_.isNotEmpty() && findTrack(pendingTrack_) != nullptr;
            if (selectCreatedBacking)
            { selectedTrack_ = pendingTrack_; pendingTrack_.clear(); selectedNote_.clear(); selectedNotes_.clear(); selectedClip_.clear(); }
            if (findTrack(selectedTrack_) == nullptr)
            {
                selectedTrack_ = findTrack("Voice Main") != nullptr ? "Voice Main" : tracks_.empty() ? juce::String() : tracks_.front().id;
                selectedNote_.clear(); selectedNotes_.clear(); selectedClip_.clear();
            }
            trackSelect_.setItems(names);
            for (std::size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].id == selectedTrack_) trackSelect_.setSelectedIndex(static_cast<int>(i));
            instrumentalVolume_ = std::clamp(number(project_["initialInstrumentalVolume"], 100.0), 0.0, 100.0);
            transport_.setMasterVolume(std::clamp(number(project_["initialVolume"], 100.0), 0.0, 100.0) / 100.0);
            loopActive_ = static_cast<bool>(project_["initialLoopActive"]); transport_.setLooping(loopActive_); loopRange_ = project_["initialCycleRange"];
            if (!loopRange_.isObject()) loopRange_ = object({ { "start", 0.0 }, { "end", 25.0 } });
            rebuildRows(); rebuildPitch();
            if (!fitted_ && duration_ > 0.0 && std::any_of(tracks_.begin(), tracks_.end(), [](const auto& track) { return !array(track.source["clips"]).isEmpty(); }))
            { view_.firstBeat = 0.0; view_.pixelsPerBeat = std::clamp(static_cast<double>(std::max(300, ruler_.getWidth())) / std::max(1.0, tempoMap_.beatAt(duration_)), 5.0, 80.0); fitted_ = true; }
            refreshViewport(); updatePosition(number(project_["initialPlayhead"])); arrangement_.repaint();
            if (selectCreatedBacking) selectTrack(selectedTrack_);
        }
        else if (type == "transport-state") { playing_ = static_cast<bool>(event["playing"]); transport_.setPlaying(playing_); updatePosition(number(event["playhead"])); }
        else if (type == "volume-state") transport_.setMasterVolume(std::clamp(number(event["value"]), 0.0, 100.0) / 100.0);
        else if (type == "instrumental-volume-state")
        {
            instrumentalVolume_ = std::clamp(number(event["value"]), 0.0, 100.0);
            for (auto& row : rows_) if (row->id() == "Instrumental") row->header().setGain(20.0 * std::log10(std::max(0.0001, instrumentalVolume_ / 100.0)));
        }
        else if (type == "output-meter-state") transport_.setMasterLevels(static_cast<float>(number(event["left"])), static_cast<float>(number(event["right"])));
        else if (type == "loop-state") { loopActive_ = static_cast<bool>(event["active"]); transport_.setLooping(loopActive_); loopRange_ = event["range"]; cycleOverlay_.setRange(percentToBeat(number(loopRange_["start"])), percentToBeat(number(loopRange_["end"])), loopActive_); }
        else if (type == "track-selected") selectTrack(event["track"].toString(), false);
        else if (type == "backing-track-created")
        {
            const auto id = event["track"]["id"].toString();
            if (id.isNotEmpty() && activatedBackingTracks_.insert(id).second)
            {
                if (findTrack(id) != nullptr) selectTrack(id);
                else pendingTrack_ = id;
            }
        }
        else if (type == "track-state" || type == "track-layer-state")
        {
            const auto id = event["track"].toString();
            for (auto& track : tracks_) if (track.id == id)
            {
                if (type == "track-state") { track.muted = static_cast<bool>(event["state"]["mute"]); track.solo = static_cast<bool>(event["state"]["solo"]); }
                else { track.audioMuted = static_cast<bool>(event["state"]["audioMuted"]); track.notesMuted = static_cast<bool>(event["state"]["notesMuted"]); }
                for (auto& row : rows_) if (row->id() == id) row->updateState(track);
            }
        }
        else if (type == "clip-pitch-state")
        {
            const auto track = event["track"].toString(), id = event["clipId"].toString();
            juce::var values;
            if (track == "Voice Main") values = project_["clips"];
            else for (const auto& content : array(project_["backingTrackContents"])) if (content["id"].toString() == track) values = content["clips"];
            for (const auto& value : array(values)) if (value["id"].toString() == id)
                if (auto* data = value.getDynamicObject()) data->setProperty("pitch", event["pitch"]);
            if (track == selectedTrack_) rebuildPitch();
        }
        else if (type == "status-state" || type == "export-complete") status_.setText(event["message"].toString(), juce::dontSendNotification);
    }
    NativeStudioView::Snapshot state() const
    {
        NativeStudioView::Snapshot result;
        result.trackCount = static_cast<int>(tracks_.size()); result.noteCount = static_cast<int>(notes_.size());
        for (const auto& track : tracks_) result.clipCount += array(track.source["clips"]).size();
        result.selectedTrack = selectedTrack_; result.selectedNoteId = selectedNote_; result.playheadSeconds = duration_ * playheadPercent_ / 100.0; result.pixelsPerBeat = view_.pixelsPerBeat;
        return result;
    }
    void paint(juce::Graphics& g) override { g.fillAll(colour(tokens::background)); }
    void resized() override
    {
        auto bounds = getLocalBounds();
        transport_.setBounds(bounds.removeFromTop(kTransportHeight));
        status_.setBounds(bounds.removeFromBottom(kStatusHeight));
        horizontal_.setBounds(bounds.removeFromBottom(16).withTrimmedLeft(kHeaderWidth).reduced(8, 1));
        const auto pitchHeight = std::clamp(bounds.getHeight() * 37 / 100, 150, 360);
        auto pitchArea = bounds.removeFromBottom(std::min(pitchHeight, std::max(0, bounds.getHeight() - kToolbarHeight - 120)));
        auto pitchRuler = pitchArea.removeFromTop(kRulerHeight);
        lowerRuler_.setBounds(pitchRuler.withTrimmedLeft(kKeyboardWidth));
        keyboard_.setBounds(pitchArea.removeFromLeft(kKeyboardWidth).reduced(2, 0)); pitch_.setBounds(pitchArea);
        lowerPlayhead_.setBounds(lowerRuler_.getX(), lowerRuler_.getY(), lowerRuler_.getWidth(), lowerRuler_.getHeight() + pitch_.getHeight());
        toolbar_.setBounds(bounds.removeFromBottom(kToolbarHeight));
        auto tools = toolbar_.getLocalBounds().reduced(10, 6);
        trackSelect_.setBounds(tools.removeFromLeft(std::min(180, tools.getWidth() / 5))); tools.removeFromLeft(10);
        for (auto* button : { &pointer_, &pencil_, &scissors_ }) { button->setBounds(tools.removeFromLeft(32)); tools.removeFromLeft(4); }
        tools.removeFromLeft(8); snap_.setBounds(tools.removeFromLeft(94)); tools.removeFromLeft(8);
        for (auto* button : { &delete_, &noteMenu_, &undo_, &redo_ }) { button->setBounds(tools.removeFromLeft(30)); tools.removeFromLeft(4); }
        zoomIn_.setBounds(tools.removeFromRight(30)); zoom_.setBounds(tools.removeFromRight(std::min(130, tools.getWidth()))); zoomOut_.setBounds(tools.removeFromRight(30));
        scroll_.setBounds(bounds);
        const auto globalHeight = arrangement_.globalsExpanded ? 132 : 36;
        auto y = globalHeight + 40;
        std::set<juce::String> laidOut;
        const auto place = [&](const auto& self, const juce::String& id, bool visible) -> void
        {
            if (!laidOut.insert(id).second) return;
            auto found = std::find_if(rows_.begin(), rows_.end(), [&](const auto& row) { return row->id() == id; });
            if (found == rows_.end()) return;
            (*found)->setVisible(visible);
            if (visible) { (*found)->setBounds(0, y, scroll_.getWidth(), kRowHeight); y += kRowHeight; }
            const auto* track = findTrack(id);
            const auto group = groups_.find(id);
            if (group != groups_.end())
            {
                group->second->setVisible(visible);
                if (visible) { group->second->setBounds(16, y, std::min(scroll_.getWidth() - 24, 420), 38); y += 40; }
            }
            const auto childrenVisible = visible && track != nullptr && !track->collapsed;
            for (const auto& child : tracks_) if (child.parent == id) self(self, child.id, childrenVisible);
        };
        for (const auto& track : tracks_) if (track.parent.isEmpty() || findTrack(track.parent) == nullptr) place(place, track.id, true);
        for (const auto& track : tracks_) if (!laidOut.contains(track.id)) place(place, track.id, true);
        arrangement_.setSize(scroll_.getWidth(), std::max(scroll_.getHeight(), y + 8));
        global_.setBounds(12, 4, kHeaderWidth - 14, 30);
        const auto timeWidth = std::max(0, arrangement_.getWidth() - kHeaderWidth);
        ruler_.setBounds(kHeaderWidth, 4, timeWidth, 30);
        for (auto* lane : std::initializer_list<juce::Component*> { &tempo_, &signature_, &chords_ }) lane->setVisible(arrangement_.globalsExpanded);
        tempo_.setBounds(kHeaderWidth, 36, timeWidth, 32); signature_.setBounds(kHeaderWidth, 68, timeWidth, 32); chords_.setBounds(kHeaderWidth, 100, timeWidth, 32);
        musicalEvents_.setBounds(kHeaderWidth, 36, timeWidth, 64); musicalEvents_.setVisible(arrangement_.globalsExpanded); musicalEvents_.toFront(false);
        cycleOverlay_.setBounds(ruler_.getBounds()); cycleOverlay_.toFront(false);
        addTrack_.setBounds(14, globalHeight + 6, 150, 28);
        arrangementPlayhead_.setBounds(kHeaderWidth, 26, timeWidth, arrangement_.getHeight() - 26); arrangementPlayhead_.toFront(false); lowerPlayhead_.toFront(false);
        refreshViewport();
    }
    void arrangementWheel(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel, float anchor)
    {
        const auto delta = wheel.deltaX != 0.0f ? wheel.deltaX : wheel.deltaY;
        if (event.mods.isCommandDown() || event.mods.isCtrlDown()) gesture(0.0, std::exp(static_cast<double>(delta) * 1.7), anchor);
        else if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY)) gesture(-static_cast<double>(delta) * 160.0 / view_.pixelsPerBeat, 1.0, anchor);
        else scroll_.setViewPosition(0, scroll_.getViewPositionY() - juce::roundToInt(wheel.deltaY * 220.0f));
    }
    bool arrangementKey(const juce::KeyPress& key)
    {
        if (selectedClip_.isEmpty()) return false;
        if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
        if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey)
        {
            const auto* track = findTrack(selectedTrack_); if (track == nullptr) return false;
            for (const auto& clip : array(track->source["clips"])) if (clip["id"].toString() == selectedClip_)
            {
                const auto step = key.getModifiers().isShiftDown() ? 1.0 : 0.1;
                const auto start = std::max(0.0, number(clip["start"]) + (key.getKeyCode() == juce::KeyPress::leftKey ? -step : step));
                send(command("move-arrangement-clip", { { "track", selectedTrack_ }, { "clipId", selectedClip_ }, { "targetTrack", selectedTrack_ }, { "start", start } }));
                return true;
            }
        }
        return false;
    }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (arrangementKey(key)) return true;
        if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
        if (key.getKeyCode() == juce::KeyPress::spaceKey) { send(command("transport", { { "action", playing_ ? "pause" : "play" } })); return true; }
        if ((key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) && juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) == 'z')
        { send(command("project-action", { { "action", key.getModifiers().isShiftDown() ? "redo" : "undo" } })); return true; }
        if ((key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) && juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) == 'a')
        { selectAllNotes(); return true; }
        return false;
    }
private:
    NativeStudioView& owner_; CommandHandler handler_;
    juce::var project_, loopRange_;
    TempoMap tempoMap_;
    TimeViewport view_, lowerView_;
    TransportBar transport_;
    ScrollViewport scroll_;
    ArrangementPanel arrangement_;
    TimelineRuler ruler_;
    TempoLane tempo_;
    SignatureLane signature_;
    ChordLane chords_;
    Playhead arrangementPlayhead_;
    MusicalEventsOverlay musicalEvents_;
    CycleOverlay cycleOverlay_;
    Button global_ { "Global Tracks", Icon::chevronDown }, addTrack_ { "Add Track", Icon::plus };
    std::vector<Track> tracks_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::map<juce::String, std::unique_ptr<TrackGroupHeader>> groups_;
    Panel toolbar_;
    Select trackSelect_, snap_;
    IconButton pointer_ { Icon::pointer, "Select notes" }, pencil_ { Icon::pencil, "Draw notes" }, scissors_ { Icon::scissors, "Split notes" };
    IconButton delete_ { Icon::close, "Delete selection" }, noteMenu_ { Icon::more, "Note actions" }, undo_ { Icon::rewind, "Undo pitch edit" }, redo_ { Icon::loop, "Redo pitch edit" };
    IconButton zoomOut_ { Icon::minus, "Zoom out" }, zoomIn_ { Icon::plus, "Zoom in" };
    Slider zoom_, horizontal_;
    TimelineRuler lowerRuler_;
    PianoKeyboard keyboard_;
    PitchEditor pitch_;
    Playhead lowerPlayhead_;
    juce::Label status_;
    Menu menu_;
    std::vector<Note> notes_;
    std::map<juce::String, int> noteIds_;
    std::set<juce::String> selectedNotes_, activatedBackingTracks_;
    int nextNoteId_ = 1;
    juce::String selectedTrack_, selectedClip_, selectedNote_, pendingTrack_;
    double duration_ = 0.0, playheadBeat_ = 0.0, playheadPercent_ = 0.0, instrumentalVolume_ = 100.0, snapBeats_ = 0.25;
    bool fitted_ = false, snapToChord_ = false, playing_ = false, loopActive_ = false;
};

NativeStudioView::NativeStudioView(CommandHandler handler) : content_(std::make_unique<Content>(*this, std::move(handler)))
{
    setTitle("Obsidian vocal editor"); addAndMakeVisible(*content_); setWantsKeyboardFocus(true);
}
NativeStudioView::~NativeStudioView() = default;
void NativeStudioView::dispatch(const juce::var& event) { jassert(juce::MessageManager::getInstance()->isThisTheMessageThread()); content_->dispatch(event); }
NativeStudioView::Snapshot NativeStudioView::state() const { return content_->state(); }
void NativeStudioView::resized() { content_->setBounds(getLocalBounds()); }
bool NativeStudioView::keyPressed(const juce::KeyPress& key) { return content_->keyPressed(key); }
bool NativeStudioView::isInterestedInFileDrag(const juce::StringArray& files)
{
    static const juce::StringArray extensions { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".m4a", ".aac" };
    return std::any_of(files.begin(), files.end(), [&](const auto& path) { return extensions.contains(juce::File(path).getFileExtension().toLowerCase()); });
}
void NativeStudioView::filesDropped(const juce::StringArray& files, int x, int y) { content_->importFiles(files, { x, y }); }
}
