#include "Demo.h"
#include "editor_ui/Chrome.h"
#include "editor_ui/PitchEditor.h"
#include "uikit/Layout.h"
#include <cmath>
#include <vector>

namespace obsidian::demo
{
namespace
{
using namespace obsidian::ui;
using namespace obsidian::editor;
constexpr int kKeyboardWidth = 84;
constexpr int kTransportHeight = 60;
constexpr int kToolbarHeight = 46;
constexpr int kRulerHeight = 22;

std::vector<float> makePeaks(bool instrumental, int seed, double start, double length, bool chorus)
{
    std::vector<float> peaks(4800);
    juce::Random random(seed);
    for (std::size_t i = 0; i < peaks.size(); ++i)
    {
        const auto beat = start + length * static_cast<double>(i) / static_cast<double>(peaks.size());
        double envelope = 0.0;
        if (instrumental)
            envelope = 0.35 + 0.16 * std::abs(std::sin(beat * 3.7)) + 0.15 * std::pow(std::abs(std::sin(beat * 0.4)), 14.0);
        else
        {
            const auto inPhrase = chorus ? ((beat > 222 && beat < 236) || (beat > 253 && beat < 276))
                                         : ((beat > 205 && beat < 224) || (beat > 235 && beat < 251));
            if (inPhrase)
            {
                const auto syllable = std::abs(std::sin(beat * 2.1)) * std::abs(std::sin(beat * 0.47));
                envelope = 0.13 + 0.60 * syllable;
            }
        }
        const auto oscillation = 0.30 + 0.70 * std::abs(std::sin(beat * (chorus ? 19.0 : 27.0)));
        peaks[i] = static_cast<float>(envelope * oscillation) * (0.60f + random.nextFloat() * 0.40f);
    }
    return peaks;
}
std::vector<NoteSegment> makeNotes()
{
    std::vector<NoteSegment> result;
    // Synthetic phrase timings are expressed in beats, independent of window size.
    constexpr double starts[] {183,185,186.7,190.5,191.6,194.3,196,199.3,200.1,201.9,203.1,204.7,208.1,209.1,213.7,216,217.6,219.1,221.4,224.3,225.5,228.7,229.9,232.7,235.1,238,239,245.7,248.2,249.1,251.3,252.8,254.3,257.6,259.2,261.1,266.1,269.4,271.1,274.5};
    constexpr int melody[] {63,55,61,66,61,69,66,65,61,56,67,65,63,60,69,66,56,59,60,69,63,59,66,62,59,63,66,61,56,63,69,58,61,55,58,60,66,63,65,64};
    for (int i = 0; i < 40; ++i)
    {
        NoteSegment note;
        note.id = i + 1;
        note.startBeat = starts[i];
        note.lengthBeats = i == 26 ? 5.3 : i == 39 ? 2.0 : std::max(0.3, (starts[i + 1] - starts[i]) - (i % 6 == 0 ? 0.25 : 0.06));
        note.midiNote = melody[i];
        note.ghost = (i == 1 || i == 14 || i == 15 || i == 16 || i == 17 || i == 21 || i == 22 || i == 28 || i == 36 || i == 37 || i == 38);
        note.colour = colour(tokens::olive);
        const auto samples = std::max(8, static_cast<int>(note.lengthBeats * 15));
        for (int n = 0; n < samples; ++n)
            note.pitchOffsets.push_back(static_cast<float>(0.20 * std::sin(n * 0.62 + i) + 0.10 * std::sin(n * 1.53) + 0.15 * std::sin(n * 0.17 + i)));
        result.push_back(std::move(note));
    }
    return result;
}
class ArrangementContent final : public juce::Component
{
public:
    int headerWidth = 290;
    bool globalsExpanded = true;
    void paint(juce::Graphics& g) override
    {
        g.fillAll(colour(tokens::background));
        g.setFont(font());
        if (globalsExpanded)
        {
            const juce::String labels[] { "Tempo", "Signature", "Chords" };
            const juce::String values[] { "94", "4/4", "" };
            const Icon icons[] { Icon::clock, Icon::signature, Icon::chords };
            for (int i = 0; i < 3; ++i)
            {
                auto row = juce::Rectangle<int>(12, 38 + 36 * i, headerWidth - 14, 36);
                g.setColour(colour(tokens::surface)); g.fillRect(row);
                g.setColour(colour(tokens::border)); g.drawRect(row, 1);
                drawIcon(g, icons[i], row.withX(42).withWidth(19).toFloat().withSizeKeepingCentre(19, 19), colour(tokens::text));
                g.setColour(colour(tokens::text));
                g.drawText(labels[i], row.withTrimmedLeft(66).withTrimmedRight(45), juce::Justification::centredLeft);
                g.setColour(colour(tokens::muted));
                g.drawText(values[i], row.removeFromRight(38).reduced(4, 0), juce::Justification::centredRight);
            }
        }
    }
};
class TrackRow final : public juce::Component
{
public:
    TrackRow(const TimeViewport& view, int trackId, bool isBacking)
        : id(trackId), backing(isBacking), grid(view), clip(view)
    {
        addAndMakeVisible(grid); addAndMakeVisible(clip); addAndMakeVisible(header);
        grid.setInterceptsMouseClicks(false, false);
        setComponentID("track." + juce::String(id));
    }
    void resized() override
    {
        header.setBounds(8, 0, headerWidth - 9, getHeight() - 2);
        const auto timeline = getLocalBounds().withTrimmedLeft(headerWidth).withTrimmedBottom(2);
        grid.setBounds(timeline); clip.setBounds(timeline);
    }
    int id, headerWidth = 290;
    bool backing, muted = false, solo = false;
    double gain = 0.0;
    juce::String name;
    TrackHeader header;
    TimelineGrid grid;
    AudioClip clip;
};
class ScrollViewport final : public juce::Viewport
{
public:
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (!e.mods.isCtrlDown() && !e.mods.isCommandDown())
            setViewPosition(getViewPositionX(), getViewPositionY() - juce::roundToInt(wheel.deltaY * 220.0f));
    }
};
}

class Demo::Content final : public juce::Component, private juce::Timer
{
public:
    explicit Content(Demo& owner) : owner_(owner), ruler_(view_), tempo_(view_), signature_(view_), chords_(view_),
        arrangementPlayhead_(view_), lowerRuler_(lowerView_), pitch_(lowerView_), lowerPlayhead_(lowerView_)
    {
        setTitle("Obsidian native editor demonstration");
        for (auto* component : std::initializer_list<juce::Component*> { &transport_, &scroll_, &toolbar_, &lowerRuler_, &keyboard_, &pitch_, &lowerPlayhead_, &horizontalScroll_ })
            addAndMakeVisible(component);
        scroll_.setViewedComponent(&arrangement_, false);
        scroll_.setScrollBarsShown(false, false);
        for (auto* component : std::initializer_list<juce::Component*> { &ruler_, &tempo_, &signature_, &chords_, &global_, &addTrack_, &group_, &arrangementPlayhead_ })
            arrangement_.addAndMakeVisible(component);
        global_.setVariant(Button::Variant::subtle);
        global_.setTextJustification(juce::Justification::centredLeft);
        global_.onClick = [this]
        {
            arrangement_.globalsExpanded = !arrangement_.globalsExpanded;
            global_.setIcon(arrangement_.globalsExpanded ? Icon::chevronDown : Icon::chevronRight);
            resized();
        };
        addTrack_.onClick = [this] { addTrack(false); };
        group_.onExpanded = [this](bool expanded) { group_.setExpanded(expanded); resized(); };
        group_.onAdd = [this] { addTrack(true); };
        transport_.onPlay = [this](bool play) { setPlaying(play); };
        transport_.setProjectTitle("Echoes Vocal");
        transport_.setSignature(4, 4);
        transport_.setProjectActions({ "New demo project", "Open saved demo", "Save demo in memory", "Simulate export", "Component gallery", "Reset demo" });
        transport_.onStop = [this] { setPlaying(false); };
        transport_.onRewind = [this] { seek(view_.firstBeat); };
        transport_.onRecord = [this](bool enabled) { recording_ = enabled; transport_.setRecording(enabled); showMessage(enabled ? "Demo recording armed" : "Recording disarmed"); };
        transport_.onLoop = [this](bool enabled) { looping_ = enabled; transport_.setLooping(enabled); };
        transport_.onMasterVolume = [this](double value) { masterVolume_ = value; };
        transport_.onProjectAction = [this](int action)
        {
            if (action == 4) { setPlaying(false); if (owner_.onShowGallery) owner_.onShowGallery(); return; }
            if (action == 0 || action == 5) { reset(); return; }
            if (action == 2) { savedNotes_ = notes_; showMessage("Demo project saved in memory"); }
            if (action == 1) { notes_ = savedNotes_.empty() ? makeNotes() : savedNotes_; pitch_.setNotes(notes_); showMessage("Demo project opened"); }
            if (action == 3) { ++exports_; showMessage("Demo export " + juce::String(exports_) + " completed"); }
        };
        for (auto* component : std::initializer_list<juce::Component*> { &trackSelect_, &pointer_, &pencil_, &scissors_, &snap_, &zoomOut_, &zoom_, &zoomIn_ })
            toolbar_.addAndMakeVisible(component);
        pointer_.setComponentID("tool.pointer"); pencil_.setComponentID("tool.pencil"); scissors_.setComponentID("tool.scissors");
        snap_.setComponentID("snap"); trackSelect_.setComponentID("track.select"); zoom_.setComponentID("zoom");
        addTrack_.setComponentID("track.add"); global_.setComponentID("globals.toggle");
        pitch_.setComponentID("pitch"); ruler_.setComponentID("ruler"); lowerRuler_.setComponentID("pitch.ruler");
        trackSelect_.onChange = [this](int index) { selectTrack(index); };
        pointer_.onClick = [this] { setTool(PitchTool::pointer); };
        pencil_.onClick = [this] { setTool(PitchTool::pencil); };
        scissors_.onClick = [this] { setTool(PitchTool::scissors); };
        snap_.setItems({ "Off", "1/4", "1/8", "1/16", "1/32" });
        snap_.setSelectedIndex(3);
        snap_.onChange = [this](int index)
        {
            const double steps[] { 0.0, 1.0, 0.5, 0.25, 0.125 };
            snapBeats_ = steps[juce::jlimit(0, 4, index)]; pitch_.setSnapBeats(snapBeats_);
        };
        zoom_.setTitle("Timeline zoom"); zoom_.setRange(5, 160, 0.1);
        zoom_.onValueChange = [this](double value) { changeViewport(0, value / view_.pixelsPerBeat, timelineWidth() * 0.5f); };
        zoomOut_.onClick = [this] { changeViewport(0, 1.0 / 1.25, timelineWidth() * 0.5f); };
        zoomIn_.onClick = [this] { changeViewport(0, 1.25, timelineWidth() * 0.5f); };
        horizontalScroll_.setTitle("Horizontal timeline position"); horizontalScroll_.setRange(0, 512, 0.25);
        horizontalScroll_.onValueChange = [this](double beat) { view_.firstBeat = beat; refreshTime(); };
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_ }) wire(*surface, false);
        wire(lowerRuler_, true); wire(pitch_, true);
        pitch_.onNoteSelected = [this](int id) { selectedNote_ = id; pitch_.setSelectedNote(id); };
        pitch_.onEditRequested = [this](PitchTool tool, int id, double beat, int note) { editNote(tool, id, beat, note); };
        keyboard_.setNoteRange(46, 75); pitch_.setNoteRange(46, 75);
        setTool(PitchTool::pointer);
        reset();
    }
    ~Content() override { stopTimer(); scroll_.setViewedComponent(nullptr); }
    void reset()
    {
        setPlaying(false);
        trackMenu_.dismiss(); trackMenu_.setItems({});
        tracks_.clear(); selectedTrack_ = -1; nextTrackId_ = 1; selectedNote_ = 27;
        view_ = TimeViewport{}; view_.pixelsPerBeat = getWidth() > 0 ? timelineWidth() / 84.0 : 15.0;
        group_.setExpanded(true); arrangement_.globalsExpanded = true; global_.setIcon(Icon::chevronDown);
        appendTrack("Instrumental", "Instrumental_01", false, colour(tokens::blue), Icon::waveform, 192, 84, -8, true);
        appendTrack("Voice Main", "Verse_1", false, colour(tokens::olive), Icon::microphone, 196.5, 54.5, 0, false);
        appendTrack("Audio 3", "Chorus_1", false, colour(tokens::pink), Icon::waveform, 214, 62, -4.5, false);
        appendTrack("Unison Double", "Unison Double", true, colour(tokens::pink), Icon::backing, 196.5, 79.5, -6, false);
        appendTrack("Fifth Below", "Fifth Below", true, colour(tokens::pink), Icon::backing, 196.5, 79.5, -7.5, false);
        notes_ = makeNotes(); savedNotes_.clear(); pitch_.setNotes(notes_); pitch_.setSelectedNote(selectedNote_);
        std::vector<ChordBlock> chords;
        const juce::String labels[] { "Bbm", "Gb", "Ab", "Ebm", "Bbm", "Gb", "Ab", "Bb", "Gb", "Ab", "Ebm" };
        double beat = 192;
        for (int i = 0; i < 25; ++i)
        {
            const auto length = (i % 5 == 3 || i % 5 == 4) ? 6.0 : 3.5;
            chords.push_back({ beat, length, labels[i % 11] }); beat += length;
        }
        chords_.setChords(std::move(chords));
        tempo_.setTempo(94); tempo_.setPointBeats({ 200, 218, 238, 255, 266, 269 }); signature_.setSignature(4, 4); transport_.setTempo(94);
        transport_.setMasterVolume(0.76); masterVolume_ = 0.76; transport_.setMasterLevels(0.53f, 0.48f);
        recording_ = looping_ = false; transport_.setRecording(false); transport_.setLooping(false);
        selectTrack(1); seek(226.0); resized(); refreshTime();
    }
    Demo::State state() const
    {
        return { playhead_, view_.firstBeat, view_.pixelsPerBeat, static_cast<int>(tracks_.size()),
                 static_cast<int>(notes_.size()), selectedTrack_, selectedNote_, playing_, group_.isExpanded() };
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(colour(tokens::background));
        g.setColour(colour(tokens::muted)); g.setFont(font());
        const auto y = toolbar_.getY();
        g.drawText("Snap", 378, y + 10, 42, 26, juce::Justification::centredLeft);
    }
    void paintOverChildren(juce::Graphics& g) override
    {
        if (message_.isNotEmpty())
        {
            auto box = juce::Rectangle<int>(getWidth() / 2 - 190, 72, 380, 34);
            g.setColour(colour(tokens::raised)); g.fillRoundedRectangle(box.toFloat(), 4);
            g.setColour(colour(tokens::border)); g.drawRoundedRectangle(box.toFloat(), 4, 1);
            g.setColour(colour(tokens::text)); g.setFont(font()); g.drawText(message_, box.reduced(12, 0), juce::Justification::centred);
        }
        g.setColour(colour(tokens::muted)); g.setFont(font());
        g.drawText("Snap", 378, toolbar_.getY() + 10, 42, 26, juce::Justification::centredLeft);
    }
    void resized() override
    {
        headerWidth_ = juce::jlimit(270, 290, juce::roundToInt(getWidth() * 0.183));
        arrangement_.headerWidth = headerWidth_;
        transport_.setBounds(0, 0, getWidth(), kTransportHeight);
        const auto pitchHeight = juce::jlimit(170, 292, juce::roundToInt(getHeight() * 0.282));
        const auto toolbarY = getHeight() - pitchHeight - kToolbarHeight - 20;
        scroll_.setBounds(0, kTransportHeight + 10, getWidth() - 12, std::max(120, toolbarY - kTransportHeight - 12));
        const auto globals = arrangement_.globalsExpanded;
        global_.setBounds(12, 0, headerWidth_ - 14, 36);
        ruler_.setBounds(headerWidth_, 0, static_cast<int>(timelineWidth()), 38);
        tempo_.setBounds(headerWidth_, 38, static_cast<int>(timelineWidth()), 36);
        signature_.setBounds(headerWidth_, 74, static_cast<int>(timelineWidth()), 36);
        chords_.setBounds(headerWidth_, 110, static_cast<int>(timelineWidth()), 36);
        tempo_.setVisible(globals); signature_.setVisible(globals); chords_.setVisible(globals);
        int y = globals ? 152 : 42;
        addTrack_.setBounds(12, y, 148, 30); y += 36;
        for (auto& row : tracks_) if (!row->backing) { row->setVisible(true); row->headerWidth = headerWidth_; row->setBounds(0, y, scroll_.getWidth(), 72); y += 72; }
        group_.setBounds(8, y, 342, 42); y += 42;
        for (auto& row : tracks_) if (row->backing)
        {
            row->setVisible(group_.isExpanded());
            if (group_.isExpanded()) { row->headerWidth = headerWidth_; row->setBounds(0, y, scroll_.getWidth(), 64); y += 64; }
        }
        arrangement_.setSize(scroll_.getWidth(), std::max(y + 4, scroll_.getHeight()));
        arrangementPlayhead_.setBounds(headerWidth_, 24, static_cast<int>(timelineWidth()), arrangement_.getHeight() - 24);
        arrangementPlayhead_.toFront(false);
        toolbar_.setBounds(0, toolbarY, getWidth(), kToolbarHeight);
        trackSelect_.setBounds(22, 8, 180, 30);
        pointer_.setBounds(216, 8, 38, 30); pencil_.setBounds(266, 8, 38, 30); scissors_.setBounds(316, 8, 38, 30);
        snap_.setBounds(418, 8, 92, 30);
        zoomOut_.setBounds(getWidth() - 218, 8, 30, 30); zoom_.setBounds(getWidth() - 181, 8, 132, 30); zoomIn_.setBounds(getWidth() - 40, 8, 30, 30);
        const auto lowerY = toolbarY + kToolbarHeight;
        lowerRuler_.setBounds(12 + kKeyboardWidth, lowerY, getWidth() - kKeyboardWidth - 24, kRulerHeight);
        keyboard_.setBounds(12, lowerY + kRulerHeight, kKeyboardWidth, pitchHeight - kRulerHeight);
        pitch_.setBounds(lowerRuler_.getX(), lowerY + kRulerHeight, lowerRuler_.getWidth(), pitchHeight - kRulerHeight);
        lowerPlayhead_.setBounds(lowerRuler_.getX(), lowerY + 10, lowerRuler_.getWidth(), pitchHeight - 10);
        horizontalScroll_.setBounds(headerWidth_, getHeight() - 17, getWidth() - headerWidth_ - 18, 14);
        refreshTime();
    }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::spaceKey) { setPlaying(!playing_); return true; }
        if (key.getTextCharacter() == 'g' || key.getTextCharacter() == 'G') { if (owner_.onShowGallery) owner_.onShowGallery(); return true; }
        if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
        {
            const auto before = notes_.size();
            std::erase_if(notes_, [this](const auto& note) { return note.id == selectedNote_; });
            if (before != notes_.size()) { selectedNote_ = -1; pitch_.setNotes(notes_); }
            return true;
        }
        return false;
    }
private:
    float timelineWidth() const { return static_cast<float>(std::max(1, getWidth() - headerWidth_ - 12)); }
    void wire(EditorSurface& surface, bool lower)
    {
        surface.onSeek = [this](double beat) { seek(beat); };
        surface.onViewportGesture = [this, lower](double scroll, double zoom, float anchor)
        {
            if (lower) anchor -= static_cast<float>(headerWidth_ - lowerRuler_.getX());
            changeViewport(scroll, zoom, anchor);
        };
    }
    void appendTrack(const juce::String& name, const juce::String& clipName, bool backing,
                     juce::Colour accent, Icon icon, double start, double length, double gain, bool instrumental)
    {
        auto row = std::make_unique<TrackRow>(view_, nextTrackId_++, backing);
        auto* track = row.get();
        track->name = name; track->gain = gain;
        track->header.setTrack(track->id, name, accent, icon); track->header.setGain(gain);
        track->clip.setData({ start, length, clipName, accent, makePeaks(instrumental, instrumental ? 32 : 19, start, length, accent == colour(tokens::pink)), false });
        track->header.onMute = [track](bool value) { track->muted = value; track->header.setMuted(value); track->clip.setAlpha(value ? 0.45f : 1.0f); };
        track->header.onSolo = [track](bool value) { track->solo = value; track->header.setSolo(value); };
        track->header.onGain = [track](double value) { track->gain = value; track->header.setGain(value); };
        track->header.onMenu = [this, track]
        {
            trackMenu_.setItems({
                { "Select track", [this, track] { selectTrack(indexOf(track)); } },
                { track->muted ? "Unmute" : "Mute", [track] { track->header.getMuteButton().triggerClick(); } },
                { "Reset volume", [track] { track->gain = 0; track->header.setGain(0); } }
            });
            trackMenu_.showFor(track->header, 180);
        };
        track->clip.onSelect = [this, track] { selectTrack(indexOf(track)); };
        wire(track->clip, false);
        arrangement_.addAndMakeVisible(track);
        tracks_.push_back(std::move(row));
        juce::StringArray names; for (const auto& item : tracks_) names.add(item->name);
        trackSelect_.setItems(names);
    }
    int indexOf(const TrackRow* track) const
    {
        for (std::size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].get() == track) return static_cast<int>(i);
        return -1;
    }
    void addTrack(bool backing)
    {
        if (tracks_.size() >= 32) { showMessage("Demo supports up to 32 tracks"); return; }
        const auto name = backing ? "Backing " + juce::String(nextTrackId_) : "Audio " + juce::String(nextTrackId_);
        appendTrack(name, name, backing, colour(backing ? tokens::pink : tokens::blue), backing ? Icon::backing : Icon::waveform,
                    view_.firstBeat + 8, 32, -6, false);
        if (backing) group_.setExpanded(true);
        selectTrack(static_cast<int>(tracks_.size()) - 1); resized();
        auto& row = *tracks_.back(); scroll_.setViewPosition(0, std::max(0, row.getBottom() - scroll_.getHeight()));
    }
    void selectTrack(int index)
    {
        if (!juce::isPositiveAndBelow(index, static_cast<int>(tracks_.size()))) return;
        selectedTrack_ = index; trackSelect_.setSelectedIndex(index);
        for (std::size_t i = 0; i < tracks_.size(); ++i) tracks_[i]->clip.setSelected(static_cast<int>(i) == index);
    }
    void setTool(PitchTool tool)
    {
        pitch_.setTool(tool); pointer_.setSelected(tool == PitchTool::pointer);
        pencil_.setSelected(tool == PitchTool::pencil); scissors_.setSelected(tool == PitchTool::scissors);
    }
    void editNote(PitchTool tool, int id, double beat, int midi)
    {
        if (tool == PitchTool::pencil)
        {
            if (notes_.size() >= 4096) return;
            NoteSegment note; note.id = nextNoteId_++; note.startBeat = beat;
            note.lengthBeats = std::max(0.25, snapBeats_); note.midiNote = midi; note.pitchOffsets = { 0.0f, 0.0f };
            notes_.push_back(std::move(note)); selectedNote_ = notes_.back().id;
        }
        else if (tool == PitchTool::scissors)
        {
            const auto it = std::find_if(notes_.begin(), notes_.end(), [id](const auto& note) { return note.id == id; });
            if (it == notes_.end() || beat <= it->startBeat + 0.05 || beat >= it->startBeat + it->lengthBeats - 0.05) return;
            auto tail = *it; const auto fraction = (beat - it->startBeat) / it->lengthBeats;
            const auto split = juce::jlimit(std::size_t{1}, std::max(std::size_t{1}, it->pitchOffsets.size() - 1), static_cast<std::size_t>(fraction * static_cast<double>(it->pitchOffsets.size())));
            if (it->pitchOffsets.size() > 1)
            {
                tail.pitchOffsets.assign(it->pitchOffsets.begin() + static_cast<std::ptrdiff_t>(split - 1), it->pitchOffsets.end());
                it->pitchOffsets.resize(split + 1);
            }
            tail.id = nextNoteId_++; tail.startBeat = beat; tail.lengthBeats = it->startBeat + it->lengthBeats - beat;
            it->lengthBeats = beat - it->startBeat; selectedNote_ = tail.id; notes_.push_back(std::move(tail));
        }
        pitch_.setNotes(notes_); pitch_.setSelectedNote(selectedNote_);
    }
    void seek(double beat)
    {
        playhead_ = std::max(0.0, beat); arrangementPlayhead_.setBeat(playhead_); lowerPlayhead_.setBeat(playhead_);
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_, &lowerRuler_, &pitch_ }) surface->setCurrentBeat(playhead_);
        for (auto& row : tracks_) row->clip.setCurrentBeat(playhead_);
        transport_.setPosition(playhead_, playhead_ * 60.0 / 94.0);
    }
    void changeViewport(double scroll, double zoom, float anchor)
    {
        view_.scroll(scroll); view_.zoomAt(zoom, anchor); refreshTime();
    }
    void refreshTime()
    {
        lowerView_ = view_;
        lowerView_.firstBeat = view_.firstBeat - static_cast<double>(headerWidth_ - lowerRuler_.getX()) / view_.pixelsPerBeat;
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_, &lowerRuler_, &pitch_ }) surface->viewportChanged();
        for (auto& row : tracks_) { row->grid.viewportChanged(); row->clip.viewportChanged(); }
        arrangementPlayhead_.viewportChanged(); lowerPlayhead_.viewportChanged();
        zoom_.setValue(view_.pixelsPerBeat); horizontalScroll_.setValue(view_.firstBeat);
        arrangement_.repaint();
    }
    void setPlaying(bool playing)
    {
        playing_ = playing; transport_.setPlaying(playing);
        lastTick_ = juce::Time::getMillisecondCounterHiRes();
        if (playing_ || message_.isNotEmpty()) startTimerHz(60); else stopTimer();
        if (!playing_) transport_.setMasterLevels(0.0f, 0.0f);
    }
    void showMessage(juce::String message)
    {
        message_ = std::move(message); messageUntil_ = juce::Time::getMillisecondCounterHiRes() + 2200;
        lastTick_ = juce::Time::getMillisecondCounterHiRes(); startTimerHz(60); repaint();
    }
    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (playing_)
        {
            auto beat = playhead_ + std::min(0.1, (now - lastTick_) / 1000.0) * 94.0 / 60.0;
            if (looping_ && beat >= 276.0) beat = 192.0;
            seek(beat);
            const auto level = static_cast<float>(masterVolume_ * (0.48 + 0.12 * std::sin(now * 0.006)));
            transport_.setMasterLevels(level, level * 0.91f);
        }
        lastTick_ = now;
        if (message_.isNotEmpty() && now > messageUntil_) { message_.clear(); repaint(); if (!playing_) stopTimer(); }
    }
    Demo& owner_;
    TimeViewport view_, lowerView_;
    TransportBar transport_;
    ScrollViewport scroll_;
    ArrangementContent arrangement_;
    TimelineRuler ruler_;
    TempoLane tempo_;
    SignatureLane signature_;
    ChordLane chords_;
    Button global_ { "Global Tracks", Icon::chevronDown }, addTrack_ { "Add Track", Icon::plus };
    TrackGroupHeader group_;
    Playhead arrangementPlayhead_;
    std::vector<std::unique_ptr<TrackRow>> tracks_;
    Menu trackMenu_;
    Panel toolbar_;
    Select trackSelect_, snap_;
    IconButton pointer_ { Icon::pointer, "Select notes" }, pencil_ { Icon::pencil, "Draw notes" }, scissors_ { Icon::scissors, "Split notes" };
    IconButton zoomOut_ { Icon::minus, "Zoom out" }, zoomIn_ { Icon::plus, "Zoom in" };
    Slider zoom_, horizontalScroll_;
    TimelineRuler lowerRuler_;
    PianoKeyboard keyboard_;
    PitchEditor pitch_;
    Playhead lowerPlayhead_;
    Tooltip pointerTip_ { pointer_, "Select notes (Delete removes selected note)" }, pencilTip_ { pencil_, "Draw a note at the snapped position" }, scissorsTip_ { scissors_, "Split a note at the snapped position" };
    std::vector<NoteSegment> notes_, savedNotes_;
    int headerWidth_ = 290, nextTrackId_ = 1, nextNoteId_ = 1000, selectedTrack_ = -1, selectedNote_ = -1, exports_ = 0;
    double playhead_ = 226.0, masterVolume_ = 0.76, snapBeats_ = 0.25, lastTick_ = 0, messageUntil_ = 0;
    bool playing_ = false, recording_ = false, looping_ = false;
    juce::String message_;
};

Demo::Demo() : content_(std::make_unique<Content>(*this)) { addAndMakeVisible(*content_); setSize(1586, 992); }
Demo::~Demo() = default;
Demo::State Demo::state() const { return content_->state(); }
void Demo::reset() { content_->reset(); }
void Demo::resized() { content_->setBounds(getLocalBounds()); }
bool Demo::keyPressed(const juce::KeyPress& key) { return content_->keyPressed(key); }
}
