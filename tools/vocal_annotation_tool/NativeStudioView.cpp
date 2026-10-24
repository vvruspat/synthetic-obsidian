#include "NativeStudioView.h"
#include "StudioShortcuts.h"
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
constexpr int kCycleHeight = 24, kRulerHeight = 24, kStatusHeight = 24, kRowHeight = 72;
constexpr int kPitchMinimum = 0, kPitchMaximum = 127;
constexpr double kPitchRowHeight = 18.0;
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
        const auto count = static_cast<std::size_t>(std::clamp(number(waveform["pointCount"]), 0.0, static_cast<double>(decoded.getDataSize() / 4)));
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
AudioClipData prepareClipWaveform(const juce::var& clip, const TempoMap& tempoMap, juce::Colour tint)
{
    const auto start = std::max(0.0, number(clip["start"]));
    const auto duration = std::max(0.0, number(clip["duration"]));
    const auto offset = std::max(0.0, number(clip["offset"]));
    const auto sourceDuration = number(clip["waveformDurationSeconds"], number(clip["sourceDurationSeconds"], offset + duration));
    AudioClipData data;
    data.name = clip["name"].toString(); data.colour = tint;
    data.startBeat = tempoMap.beatAt(start);
    data.lengthBeats = tempoMap.beatAt(start + duration) - data.startBeat;
    const auto sourcePeaks = decodePeaks(clip["waveform"]);
    if (sourcePeaks.empty() || sourceDuration <= 0.0 || data.lengthBeats <= 0.0) return data;
    // Source peaks are uniform in seconds. Each output beat bin covers its
    // complete source interval, preserving transients across tempo changes.
    const auto count = sourcePeaks.size();
    const auto targetCount = std::clamp(static_cast<std::size_t>(std::ceil(static_cast<double>(count) * std::min(1.0, duration / sourceDuration))), std::size_t{2}, std::max(std::size_t{2}, count));
    data.peaks.reserve(targetCount);
    for (std::size_t i = 0; i < targetCount; ++i)
    {
        const auto fromBeat = data.startBeat + data.lengthBeats * static_cast<double>(i) / static_cast<double>(targetCount);
        const auto toBeat = data.startBeat + data.lengthBeats * static_cast<double>(i + 1) / static_cast<double>(targetCount);
        const auto sourceFrom = offset + tempoMap.secondsAt(fromBeat) - start;
        const auto sourceTo = offset + tempoMap.secondsAt(toBeat) - start;
        if (sourceFrom >= sourceDuration || sourceTo <= 0.0) { data.peaks.push_back(0.0f); continue; }
        const auto first = std::min(count - 1, static_cast<std::size_t>(std::floor(std::clamp(sourceFrom / sourceDuration, 0.0, 1.0) * static_cast<double>(count))));
        const auto last = std::clamp(static_cast<std::size_t>(std::ceil(std::clamp(sourceTo / sourceDuration, 0.0, 1.0) * static_cast<double>(count))), first + 1, count);
        float peak = 0.0f;
        for (auto sourceIndex = first; sourceIndex < last; ++sourceIndex) peak = std::max(peak, sourcePeaks[sourceIndex]);
        data.peaks.push_back(peak);
    }
    return data;
}
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
            auto bounds = juce::Rectangle<int>(8, 36 + kCycleHeight + i * 32, kHeaderWidth - 10, 32);
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
    explicit CycleOverlay(const TimeViewport& view) : view_(view)
    {
        setWantsKeyboardFocus(true); setTitle("Cycle range"); setComponentID("studio.cycle");
        setDescription("Drag outside the cycle to create; drag inside to move; drag edges to resize. Click the cycle or press Enter to toggle. Escape cancels a drag.");
    }
    std::function<double(double)> positionForBeat, beatForPosition;
    std::function<void(double, double, bool)> onCommit;
    std::function<void(const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;
    void setRange(double first, double last, bool active, double duration, bool authoritative = true)
    {
        minimum_ = duration > 0.0 ? std::min(100.0, 100.0 / duration) : 100.0;
        setEnabled(duration > 0.0);
        if (mode_ != Mode::none)
        {
            if (authoritative) cancel();
            else { repaint(); return; }
        }
        first_ = std::clamp(first, 0.0, 100.0); last_ = std::clamp(last, first_, 100.0); active_ = active; repaint();
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(colour(tokens::surface));
        g.setColour(colour(tokens::border)); g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
        const auto firstX = xAt(first_), lastX = xAt(last_);
        if (last_ > first_)
        {
            const auto bounds = juce::Rectangle<float>(firstX, 3.0f, std::max(1.0f, lastX - firstX), static_cast<float>(getHeight() - 6));
            g.setColour(colour(active_ ? tokens::olive : tokens::muted).withAlpha(active_ ? 0.3f : 0.12f)); g.fillRoundedRectangle(bounds, 3.0f);
            g.setColour(colour(active_ ? tokens::olive : tokens::faint)); g.drawRoundedRectangle(bounds, 3.0f, 1.0f);
            g.fillRect(firstX, 6.0f, 3.0f, static_cast<float>(std::max(1, getHeight() - 12)));
            g.fillRect(lastX - 3.0f, 6.0f, 3.0f, static_cast<float>(std::max(1, getHeight() - 12)));
            g.setFont(font(tokens::fontSmall)); g.drawText("Cycle", bounds.reduced(8.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        if (lastX < 0.0f || firstX > static_cast<float>(getWidth()) || last_ <= first_)
        { g.setColour(colour(tokens::faint)); g.setFont(font(tokens::fontSmall)); g.drawText("Drag to create a cycle", getLocalBounds().reduced(8, 0), juce::Justification::centredLeft); }
        drawFocus(g, *this);
    }
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (!isEnabled() || !event.mods.isLeftButtonDown()) return;
        const juce::Component::SafePointer<CycleOverlay> safe(this);
        const auto x = event.position.x;
        grabKeyboardFocus(); if (safe == nullptr) return;
        initialFirst_ = first_; initialLast_ = last_; initialActive_ = active_;
        anchor_ = positionAt(x); moved_ = false;
        const auto firstX = xAt(first_), lastX = xAt(last_);
        if (last_ > first_ && std::abs(x - firstX) <= 6.0f) mode_ = Mode::start;
        else if (last_ > first_ && std::abs(x - lastX) <= 6.0f) mode_ = Mode::end;
        else if (last_ > first_ && x > firstX && x < lastX) mode_ = Mode::move;
        else mode_ = Mode::create;
        if (mode_ == Mode::create)
        { first_ = std::min(anchor_, 100.0 - minimum_); last_ = first_ + minimum_; active_ = true; }
        else if (mode_ == Mode::start || mode_ == Mode::end) active_ = true;
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (mode_ == Mode::none) return;
        const auto position = positionAt(event.position.x);
        moved_ = moved_ || event.getDistanceFromDragStart() >= 3 || std::abs(position - anchor_) >= 0.1;
        if (mode_ == Mode::create)
        {
            first_ = std::min(anchor_, position); last_ = std::max(anchor_, position);
            if (last_ - first_ < minimum_)
            { first_ = position < anchor_ ? std::max(0.0, anchor_ - minimum_) : std::min(anchor_, 100.0 - minimum_); last_ = first_ + minimum_; }
        }
        else if (mode_ == Mode::move)
        {
            const auto width = initialLast_ - initialFirst_;
            first_ = std::clamp(initialFirst_ + position - anchor_, 0.0, std::max(0.0, 100.0 - width)); last_ = first_ + width;
        }
        else if (mode_ == Mode::start) first_ = std::clamp(position, 0.0, std::max(0.0, last_ - minimum_));
        else if (mode_ == Mode::end) last_ = std::clamp(position, std::min(100.0, first_ + minimum_), 100.0);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (mode_ == Mode::none) return;
        if (mode_ == Mode::move && !moved_) active_ = !initialActive_;
        mode_ = Mode::none; repaint(); commit();
    }
    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto code = key.getKeyCode();
        if (code == juce::KeyPress::escapeKey) { cancel(); return true; }
        if (code == juce::KeyPress::returnKey || code == juce::KeyPress::spaceKey) { active_ = !active_; repaint(); commit(); return true; }
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        {
            const auto direction = code == juce::KeyPress::leftKey ? -1.0 : 1.0;
            const auto width = last_ - first_;
            const auto delta = std::max(0.01, minimum_ * 0.25) * direction;
            first_ = std::clamp(first_ + delta, 0.0, std::max(0.0, 100.0 - width)); last_ = first_ + width;
            repaint(); commit(); return true;
        }
        return false;
    }
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override { const auto callback = onWheel; if (callback) callback(event, wheel); }
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { cancel(); repaint(); }
    void visibilityChanged() override { if (!isShowing()) cancel(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    { return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group); }
private:
    enum class Mode { none, create, move, start, end };
    float xAt(double position) const { return view_.xForBeat(beatForPosition ? beatForPosition(position) : position); }
    double positionAt(float x) const
    {
        const auto beat = view_.beatAtX(x);
        const auto convert = [&](double value) { return std::clamp(positionForBeat ? positionForBeat(value) : value, 0.0, 100.0); };
        const auto requested = convert(beat);
        auto nearest = requested <= 50.0 ? 0.0 : 100.0;
        view_.forEachGridLine(std::max(0.0, beat - 4.0), beat + 4.0, 1.0, [&](double gridBeat, bool)
        { const auto candidate = convert(gridBeat); if (std::abs(candidate - requested) < std::abs(nearest - requested)) nearest = candidate; });
        return nearest;
    }
    void cancel()
    {
        if (mode_ == Mode::none) return;
        first_ = initialFirst_; last_ = initialLast_; active_ = initialActive_; mode_ = Mode::none; repaint();
    }
    void commit()
    {
        const auto callback = onCommit; const auto first = first_, last = last_; const auto active = active_;
        if (callback) callback(first, last, active);
    }
    const TimeViewport& view_;
    Mode mode_ = Mode::none;
    double first_ = 0.0, last_ = 0.0, minimum_ = 1.0, anchor_ = 0.0, initialFirst_ = 0.0, initialLast_ = 0.0;
    bool active_ = false, initialActive_ = false, moved_ = false;
};
class PianoViewport final : public juce::Viewport
{
public:
    std::function<void(const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;
    std::function<void(const juce::MouseEvent&, float)> onMagnify;
    std::function<void(const juce::Rectangle<int>&)> onVisibleAreaChange;
    void mouseMagnify(const juce::MouseEvent& event, float factor) override { const auto callback = onMagnify; if (callback) callback(event, factor); }
    void visibleAreaChanged(const juce::Rectangle<int>& area) override { const auto callback = onVisibleAreaChange; if (callback) callback(area); }
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    { const auto callback = onWheel; if (callback) callback(event, wheel); }
};
class ScrollViewport final : public juce::Viewport
{
public:
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    { setViewPosition(0, getViewPositionY() - juce::roundToInt(wheel.deltaY * 220.0f)); }
};
class BackingPicker final : public Popover
{
public:
    BackingPicker()
    {
        setTitle("Add backing track"); setComponentID("studio.backing.picker");
        addAndMakeVisible(local_); addAndMakeVisible(luna_); addAndMakeVisible(terra_); addAndMakeVisible(scroll_);
        local_.setComponentID("studio.backing.generator.local"); luna_.setComponentID("studio.backing.generator.luna"); terra_.setComponentID("studio.backing.generator.terra");
        local_.onClick = [this] { setGenerator("local"); }; luna_.onClick = [this] { setGenerator("luna"); }; terra_.onClick = [this] { setGenerator("terra"); };
        setGenerator("local"); scroll_.setViewedComponent(&list_, false); scroll_.setScrollBarsShown(false, false);
    }
    ~BackingPicker() override { scroll_.setViewedComponent(nullptr, false); }
    std::function<void(const juce::String&, const juce::String&, const juce::String&)> onCreate;
    const juce::String& generator() const noexcept { return generator_; }
    void show(const juce::String& parentId, const juce::String& name, const juce::StringArray& types, juce::Component& anchor)
    {
        parentId_ = parentId; parentName_ = name; buttons_.clear();
        for (int index = 0; index < types.size(); ++index)
        {
            auto button = std::make_unique<Button>(types[index], Icon::backing); button->setVariant(Button::Variant::subtle); button->setTextJustification(juce::Justification::centredLeft);
            button->setComponentID("studio.backing.type." + juce::String(index));
            button->onClick = [this, type = types[index]]
            {
                const auto callback = onCreate; const auto parent = parentId_, generator = generator_;
                dismiss(); if (callback) callback(parent, type, generator);
            };
            list_.addAndMakeVisible(*button); buttons_.push_back(std::move(button));
        }
        Popover::showFor(anchor, 286, std::min(448, 90 + static_cast<int>(buttons_.size()) * 30));
        resized();
    }
    void paint(juce::Graphics& g) override
    {
        Popover::paint(g); g.setColour(colour(tokens::text)); g.setFont(font());
        g.drawText("Add backing to " + parentName_, juce::Rectangle<int>(12, 6, getWidth() - 24, 24), juce::Justification::centredLeft, true);
    }
    void resized() override
    {
        const auto width = (getWidth() - 32) / 3;
        local_.setBounds(8, 34, width, 28); luna_.setBounds(16 + width, 34, width, 28); terra_.setBounds(24 + width * 2, 34, width, 28);
        scroll_.setBounds(6, 70, getWidth() - 12, std::max(0, getHeight() - 76));
        list_.setSize(scroll_.getWidth(), static_cast<int>(buttons_.size()) * 30);
        for (std::size_t i = 0; i < buttons_.size(); ++i) buttons_[i]->setBounds(0, static_cast<int>(i) * 30, list_.getWidth(), 30);
    }
private:
    void setGenerator(juce::String value)
    { generator_ = std::move(value); local_.setSelected(generator_ == "local"); luna_.setSelected(generator_ == "luna"); terra_.setSelected(generator_ == "terra"); }
    Button local_ { "Local" }, luna_ { "Luna" }, terra_ { "Terra" };
    ScrollViewport scroll_; juce::Component list_;
    std::vector<std::unique_ptr<Button>> buttons_;
    juce::String parentId_, parentName_, generator_;
};
}

class NativeStudioView::Content final : public juce::Component, private juce::KeyListener, private juce::ComponentListener, private juce::ScrollBar::Listener
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
        juce::String id, track;
        double x = 0.0, width = 0.0, pitch = 60.0;
        juce::var source;
    };
    class Row;
    class ClipItem final : public juce::Component, private juce::KeyListener
    {
    public:
        ClipItem(Content& content, const juce::String& track, const juce::var& clip)
            : content_(content), track_(track), id_(clip["id"].toString()), start_(std::max(0.0, number(clip["start"]))),
              duration_(std::max(0.0, number(clip["duration"]))), audio_(localView_)
        {
            setComponentID(id_);
            addAndMakeVisible(audio_);
            audio_.setComponentID("studio.clip." + id_);
            auto data = content_.preparedClip(track, clip);
            localView_.firstBeat = data.startBeat; localView_.pixelsPerBeat = content_.view_.pixelsPerBeat;
            audio_.setData(std::move(data));
            audio_.onSelect = [this] { content_.selectClip(track_, id_); };
            audio_.onSeek = [this](double beat) { content_.seek(beat); };
            audio_.onMagnifyGesture = [this](const juce::MouseEvent& event, float factor) { content_.magnify(event, factor); };
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
        void mouseMagnify(const juce::MouseEvent& event, float factor) override
        { if (event.eventComponent == this) content_.magnify(event, factor); }
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
                track.id == "Instrumental" ? Icon::waveform : Icon::none);
            header_.setMuted(track.muted); header_.setSolo(track.solo);
            header_.getMuteButton().setComponentID("studio.mute." + track.id);
            header_.getSoloButton().setComponentID("studio.solo." + track.id);
            header_.getGainSlider().setComponentID("studio.gain." + track.id);
            header_.getGainSlider().setEnabled(track.id == "Instrumental"); header_.setGainVisible(track.id == "Instrumental");
            if (track.id == "Instrumental") header_.getGainSlider().setRange(-60.0, 0.0, 0.1);
            header_.getGainSlider().setDescription(track.id == "Instrumental" ? "Instrumental gain" : "This track has no independent gain control.");
            header_.setGain(track.id == "Instrumental" ? 20.0 * std::log10(std::max(0.0001, content.instrumentalVolume_ / 100.0)) : 0.0);
            header_.onMute = [this](bool value) { track_.muted = value; content_.setTrackState(track_.id, track_.muted, track_.solo); };
            header_.onSolo = [this](bool value) { track_.solo = value; content_.setTrackState(track_.id, track_.muted, track_.solo); };
            header_.onGain = [this](double db) { content_.send(command("set-instrumental-volume", { { "value", std::clamp(std::pow(10.0, db / 20.0) * 100.0, 0.0, 100.0) } })); };
            header_.onMenu = [this] { content_.showTrackMenu(track_.id, header_); };
            header_.addMouseListener(this, true);
            for (auto* component : std::initializer_list<juce::Component*> { &audioLayer_, &noteLayer_, &profile_ }) addAndMakeVisible(component);
            body_.addAndMakeVisible(render_); body_.addAndMakeVisible(regenerate_);
            audioLayer_.setComponentID("studio.layer.audio." + track.id); noteLayer_.setComponentID("studio.layer.notes." + track.id);
            profile_.setComponentID("studio.profile." + track.id);
            audioLayer_.setText({}); noteLayer_.setText({});
            audioLayer_.setIcon(track.parent.isEmpty() ? Icon::microphone : Icon::backing); noteLayer_.setIcon(Icon::notes);
            audioLayer_.setVariant(Button::Variant::subtle); noteLayer_.setVariant(Button::Variant::subtle);
            audioLayer_.setAccent(trackColour(track.id, track.parent.isNotEmpty())); noteLayer_.setAccent(trackColour(track.id, track.parent.isNotEmpty()));
            audioLayer_.setTitle("Audio layer for " + track.name); noteLayer_.setTitle("Note layer for " + track.name); profile_.setTitle("Voice profile for " + track.name);
            render_.setComponentID("studio.render." + track.id); regenerate_.setComponentID("studio.regenerate." + track.id);
            audioLayer_.onToggle = [this](bool enabled) { track_.audioMuted = !enabled; sendLayers(); };
            noteLayer_.onToggle = [this](bool enabled) { track_.notesMuted = !enabled; sendLayers(); };
            profile_.onChange = [this](int profileIndex) { if (juce::isPositiveAndBelow(profileIndex, static_cast<int>(profileIds_.size()))) content_.send(command("set-backing-voice-profile", { { "track", track_.id }, { "profileId", profileIds_[static_cast<std::size_t>(profileIndex)] } })); };
            render_.onClick = [this] { content_.send(command("render-backing-track", { { "track", track_.id } })); };
            regenerate_.onClick = [this] { content_.send(command("regenerate-backing-track", { { "track", track_.id }, { "generator", content_.backingPicker_.generator() } })); };
            updateControls();
            grid_.onSeek = [this](double beat) { content_.seek(beat); };
            grid_.onMagnifyGesture = [this](const juce::MouseEvent& event, float factor) { content_.magnify(event, factor); };
            grid_.addMouseListener(this, false);
            for (const auto& value : array(track.source["clips"]))
            {
                auto clip = std::make_unique<ClipItem>(content, track.id, value);
                body_.addAndMakeVisible(*clip); clips_.push_back(std::move(clip));
            }
        }
        ~Row() override { header_.removeMouseListener(this); grid_.removeMouseListener(this); }
        void resized() override
        {
            header_.setBounds(8, 0, kHeaderWidth - 10, getHeight() - 2);
            audioLayer_.setBounds(37, 10, 22, 22); noteLayer_.setBounds(37, 36, 22, 22);
            profile_.setBounds(126, 33, 130, 24);
            body_.setBounds(getLocalBounds().withTrimmedLeft(kHeaderWidth)); grid_.setBounds(body_.getLocalBounds());
            render_.setBounds(std::max(0, body_.getWidth() - 190), 34, 80, 28); regenerate_.setBounds(std::max(0, body_.getWidth() - 106), 34, 100, 28);
            refresh(); render_.toFront(false); regenerate_.toFront(false);
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
            track_ = track; header_.setMuted(track.muted); header_.setSolo(track.solo); updateControls(); repaint();
        }
        void updateControls()
        {
            const auto backing = track_.parent.isNotEmpty(), instrumental = track_.id == "Instrumental";
            const auto content = content_.backingContent(track_.id);
            const auto hasNotes = !array(content_.noteValues(track_.id)).isEmpty();
            audioLayer_.setVisible(!instrumental); noteLayer_.setVisible(!instrumental); profile_.setVisible(backing);
            audioLayer_.setToggleState(!track_.audioMuted); noteLayer_.setToggleState(!track_.notesMuted);
            audioLayer_.setEnabled(!array(track_.source["clips"]).isEmpty()); noteLayer_.setEnabled(hasNotes);
            audioLayer_.setIconColour(track_.audioMuted ? colour(tokens::faint) : trackColour(track_.id, backing));
            noteLayer_.setIconColour(track_.notesMuted ? colour(tokens::faint) : trackColour(track_.id, backing));
            content_.populateProfile(profile_, track_.id, profileIds_); profile_.setEnabled(!content_.busy_ && hasNotes && !profileIds_.empty());
            const auto generating = content_.generationTrack_ == track_.id, rendering = content_.renderTrack_ == track_.id;
            render_.setText(rendering ? "Rendering..." : "Render"); regenerate_.setText(generating ? "Generating..." : "Regenerate");
            render_.setVisible(backing && (!static_cast<bool>(content["hasAudio"]) || generating || rendering)); regenerate_.setVisible(render_.isVisible());
            render_.setEnabled(hasNotes && !content_.busy_); regenerate_.setEnabled(hasNotes && !content_.busy_);
            render_.toFront(false); regenerate_.toFront(false);
        }
        void updateSelection()
        {
            for (auto& clip : clips_) clip->setSelected(content_.isTrackSelected(track_.id) && content_.selectedClip_ == clip->getComponentID());
            repaint();
        }
        void paintOverChildren(juce::Graphics& g) override
        {
            if (content_.isTrackSelected(track_.id))
            { g.setColour(trackColour(track_.id, track_.parent.isNotEmpty()).withAlpha(0.2f)); g.drawRect(8, 0, kHeaderWidth - 10, getHeight() - 2, 1); }
        }
        juce::String id() const { return track_.id; }
        TrackHeader& header() { return header_; }
    private:
        void sendLayers() { content_.send(command("set-track-layer-state", { { "track", track_.id }, { "state", object({ { "audioMuted", track_.audioMuted }, { "notesMuted", track_.notesMuted } }) } })); }
        void mouseDown(const juce::MouseEvent& event) override
        {
            for (auto* target = event.eventComponent; target != nullptr && target != &header_ && target != this; target = target->getParentComponent())
                if (dynamic_cast<Button*>(target) != nullptr || dynamic_cast<Slider*>(target) != nullptr || dynamic_cast<Select*>(target) != nullptr) return;
            if (event.eventComponent == this || event.eventComponent == &grid_ || header_.isParentOf(event.eventComponent) || event.eventComponent == &header_) content_.selectTrack(track_.id, true, event.mods.isShiftDown());
        }
        void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
        { if (event.eventComponent == &grid_) content_.arrangementWheel(event, wheel, event.position.x); }
        void mouseMagnify(const juce::MouseEvent& event, float factor) override
        { if (event.eventComponent == this) content_.magnify(event, factor); }
        Content& content_; Track track_; TrackHeader header_; juce::Component body_; TimelineGrid grid_;
        ToggleButton audioLayer_, noteLayer_;
        Tooltip audioLayerTip_ { audioLayer_, "Toggle audio layer" }, noteLayerTip_ { noteLayer_, "Toggle note layer" };
        Select profile_;
        Button render_ { "Render" }, regenerate_ { "Regenerate" };
        std::vector<juce::String> profileIds_;
        std::vector<std::unique_ptr<ClipItem>> clips_;
    };

    Content(NativeStudioView& owner, CommandHandler handler, std::shared_ptr<StudioShortcuts> shortcuts) : owner_(owner), handler_(std::move(handler)), shortcuts_(shortcuts ? std::move(shortcuts) : std::make_shared<StudioShortcuts>()),
        ruler_(view_), tempo_(view_), signature_(view_), chords_(view_), arrangementPlayhead_(view_), musicalEvents_(view_), cycleOverlay_(view_),
        lowerRuler_(lowerView_), pitch_(lowerView_), lowerPlayhead_(lowerView_)
    {
        view_.firstBeat = 0.0; view_.pixelsPerBeat = 40.0; lowerView_ = view_;
        for (auto* child : std::initializer_list<juce::Component*> { &transport_, &scroll_, &toolbar_, &lowerRuler_, &pianoScroll_, &lowerPlayhead_, &horizontal_, &status_, &exportToast_ }) addAndMakeVisible(child);
        scroll_.setViewedComponent(&arrangement_, false); scroll_.setScrollBarsShown(false, false);
        pianoScroll_.setViewedComponent(&pianoCanvas_, false); pianoScroll_.setScrollBarsShown(false, false);
        pianoCanvas_.addAndMakeVisible(keyboard_); pianoCanvas_.addAndMakeVisible(pitch_);
        pianoScroll_.setComponentID("studio.pitchViewport");
        scroll_.setComponentID("studio.arrangementViewport");
        pianoScroll_.onWheel = [this](const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) { pitchWheel(event, wheel); };
        pianoScroll_.onMagnify = [this](const juce::MouseEvent& event, float factor) { magnify(event, factor); };
        pianoScroll_.onVisibleAreaChange = [this](const juce::Rectangle<int>& area) { pitch_.setWaveformViewport(static_cast<float>(area.getY()), static_cast<float>(area.getHeight())); };
        keyboard_.setComponentID("studio.keyboard");
        for (auto* child : std::initializer_list<juce::Component*> { &ruler_, &tempo_, &signature_, &chords_, &global_, &addTrack_, &arrangementPlayhead_, &musicalEvents_, &cycleOverlay_ }) arrangement_.addAndMakeVisible(child);
        toolbar_.setComponentID("studio.pitchToolbar");
        transport_.setComponentID("studio.transport"); ruler_.setComponentID("studio.ruler"); lowerRuler_.setComponentID("studio.pitchRuler"); pitch_.setComponentID("studio.pitch");
        transport_.setProjectTitle("Obsidian"); transport_.setRecordingAvailable(false);
        transport_.setProjectActions({ "New project", "Open project...", "Save project", "Import vocal audio...", "Import instrumental...", "Load annotation JSON...", "Load lyrics...", "Analyze selected track", "Analyze AI parts", "Generate backing", "Render backing audio", "Undo", "Redo", "Export all tracks...", "Export MIDI...", "Validate", "Audio and voice settings...", "Set loop start at playhead", "Set loop end at playhead", "Loop entire project", "File Import...", "Voice Lab...", "Note actions..." });
        transport_.onProjectAction = [this](int index)
        {
            static const char* actions[] { "new-project", "open-project", "save-project", "open-audio", "open-instrumental", "load-json", "load-lyrics", "analyze", "ai-parts", "generate-backing", "render-backing", "undo", "redo", "export-all-tracks", "export-midi", "validate" };
            if (index == 16) { const auto callback = owner_.onShowSettings; if (callback) callback(); }
            else if (index == 21) { const auto callback = owner_.onShowVoiceLab ? owner_.onShowVoiceLab : owner_.onShowSettings; if (callback) callback(); }
            else if (index == 22) showNoteMenu();
            else if (index == 20) send(command("import-track-audio", { { "track", selectedRoot() } }));
            else if (index == 9) showBackingPicker(selectedTrack_, transport_);
            else if (index >= 17 && index <= 19) changeLoop(index - 17);
            else if (juce::isPositiveAndBelow(index, static_cast<int>(std::size(actions)))) send(command("project-action", { { "action", actions[index] } }));
        };
        transport_.onPlay = [this](bool play) { send(command("transport", { { "action", play ? "play" : "pause" } })); };
        transport_.onStop = [this] { send(command("transport", { { "action", "stop" } })); };
        transport_.onRewind = [this] { send(command("transport", { { "action", "return-to-start" } })); };
        transport_.onMasterVolume = [this](double value) { send(command("set-volume", { { "value", value * 100.0 } })); };
        cycleOverlay_.positionForBeat = [this](double beat) { return beatToPercent(beat); };
        cycleOverlay_.beatForPosition = [this](double position) { return percentToBeat(position); };
        cycleOverlay_.onCommit = [this](double first, double last, bool active)
        {
            loopActive_ = active; loopRange_ = object({ { "start", first }, { "end", last } }); transport_.setLooping(active);
            send(command("set-loop", { { "active", active }, { "range", loopRange_ } }));
        };
        cycleOverlay_.onWheel = [this](const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) { arrangementWheel(event, wheel, event.position.x); };
        transport_.onLoop = [this](bool active) { send(command("set-loop", { { "active", active }, { "range", loopRange_ } })); };
        global_.setVariant(Button::Variant::subtle); global_.setTextJustification(juce::Justification::centredLeft);
        global_.onClick = [this] { arrangement_.globalsExpanded = !arrangement_.globalsExpanded; global_.setIcon(arrangement_.globalsExpanded ? Icon::chevronDown : Icon::chevronRight); resized(); };
        addTrack_.setComponentID("studio.addTrack"); addTrack_.onClick = [this] { send(command("add-track")); };
        for (auto* item : std::initializer_list<juce::Component*> { &trackSelect_, &primaryTool_, &secondaryTool_, &snap_, &chordSnap_, &undo_, &redo_, &zoomOut_, &zoom_, &zoomIn_, &pitchZoom_, &pitchCollapse_ }) toolbar_.addAndMakeVisible(item);
        trackSelect_.setComponentID("studio.trackSelect"); snap_.setComponentID("studio.snap");
        primaryTool_.setComponentID("studio.tool.primary"); secondaryTool_.setComponentID("studio.tool.secondary");
        primaryTool_.setTitle("Click tool"); secondaryTool_.setTitle(juce::SystemStats::getOperatingSystemType() & juce::SystemStats::MacOSX ? "Command-click tool" : "Control-click tool");
        primaryTool_.setDescription("Primary piano roll tool. Keys 1 through 9 select a tool.");
        secondaryTool_.setDescription("Secondary tool for Command-click on macOS, Control-click on Windows or Linux, and right-click.");
        const juce::StringArray toolNames { "Pointer", "Pencil", "Eraser", "Scissors", "Join", "Flex", "Vibrato", "Gain", "Zoom" };
        primaryTool_.setItems(toolNames); secondaryTool_.setItems(toolNames);
        const std::vector<Icon> toolIcons { Icon::pointer, Icon::pencil, Icon::eraser, Icon::scissors, Icon::join, Icon::flex, Icon::vibrato, Icon::gain, Icon::search };
        primaryTool_.setItemIcons(toolIcons, true); secondaryTool_.setItemIcons(toolIcons, true);
        primaryTool_.setSelectedIndex(0); secondaryTool_.setSelectedIndex(3);
        primaryTool_.onChange = [this](int) { applyTools(); }; secondaryTool_.onChange = [this](int) { applyTools(); };
        applyTools();
        pitchZoom_.setComponentID("studio.pitchZoom"); pitchZoom_.setTitle("Vertical Piano Roll zoom");
        pitchZoom_.setRange(1.0, 4.0, 0.05); pitchZoom_.setValue(verticalZoom_);
        pitchZoom_.valueToText = [](double value) { return juce::String(value, 2) + juce::String::fromUTF8("×"); };
        pitchZoom_.onValueChange = [this](double value) { setVerticalZoom(value); };
        pitchCollapse_.setComponentID("studio.pitchCollapse"); pitchCollapse_.setTitle("Collapse Piano Roll");
        pitchCollapse_.onClick = [this] { pianoCollapsed_ = !pianoCollapsed_; pitchCollapse_.setIcon(pianoCollapsed_ ? Icon::chevronRight : Icon::chevronDown); pitchCollapse_.setTitle(pianoCollapsed_ ? "Expand Piano Roll" : "Collapse Piano Roll"); pitchCollapse_.setSelected(pianoCollapsed_); resized(); };
        trackSelect_.onChange = [this](int index) { if (juce::isPositiveAndBelow(index, static_cast<int>(tracks_.size()))) selectTrack(tracks_[static_cast<std::size_t>(index)].id); };
        snap_.setItems({ "Snap off", "1/4", "1/8", "1/16", "1/32" }); snap_.setSelectedIndex(3);
        snap_.onChange = [this](int index) { constexpr double steps[] {0.0, 1.0, 0.5, 0.25, 0.125}; snapBeats_ = steps[std::clamp(index, 0, 4)]; pitch_.setSnapBeats(snapBeats_); };
        chordSnap_.setComponentID("studio.chordSnap"); chordSnap_.setTitle("Snap notes to chord tones"); chordSnap_.setIcon(Icon::notes);
        chordSnap_.onToggle = [this](bool enabled) { snapToChord_ = enabled; };
        undo_.setComponentID("studio.undo"); redo_.setComponentID("studio.redo");
        undo_.onClick = [this] { send(command("pitch-history", { { "track", selectedTrack_ }, { "action", "undo" } })); };
        redo_.onClick = [this] { send(command("pitch-history", { { "track", selectedTrack_ }, { "action", "redo" } })); };
        zoom_.setRange(5.0, 160.0); zoom_.setValue(view_.pixelsPerBeat); zoom_.setComponentID("studio.zoom");
        zoom_.onValueChange = [this](double value) { gesture(0.0, value / view_.pixelsPerBeat, static_cast<float>(ruler_.getWidth()) * 0.5f); };
        zoomOut_.onClick = [this] { gesture(0, 0.8, static_cast<float>(ruler_.getWidth()) * 0.5f); }; zoomIn_.onClick = [this] { gesture(0, 1.25, static_cast<float>(ruler_.getWidth()) * 0.5f); };
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_, &lowerRuler_, &pitch_ })
        {
            surface->onSeek = [this](double beat) { seek(beat); };
            surface->onMagnifyGesture = [this](const juce::MouseEvent& event, float factor) { magnify(event, factor); };
            const auto lower = surface == &lowerRuler_ || surface == &pitch_;
            surface->onWheelGesture = [this, lower](const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
            {
                if (lower) pitchWheel(event, wheel);
                else arrangementWheel(event, wheel, getLocalPoint(event.eventComponent, event.position).x - static_cast<float>(kHeaderWidth));
            };
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
        pitch_.onNoteCreated = [this](double start, double length, double pitch) { createNote(start, length, pitch); };
        pitch_.pitchTransform = [this](double requested, double beat)
        {
            if (!snapToChord_ || !pitchTrackAvailable()) return requested;
            const auto seconds = tempoMap_.secondsAt(beat);
            for (const auto& chord : array(project_["chords"]))
            {
                const auto start = number(chord["startSeconds"], -1.0), end = number(chord["endSeconds"], -1.0);
                if (start < 0.0 || seconds < start || seconds >= end || array(chord["pitchClasses"]).isEmpty()) continue;
                auto result = requested, distance = 128.0;
                for (int midi = 0; midi <= 127; ++midi)
                    if (std::any_of(array(chord["pitchClasses"]).begin(), array(chord["pitchClasses"]).end(), [midi](const auto& pitchClass) { return static_cast<int>(number(pitchClass, -1.0)) == midi % 12; }))
                    {
                        const auto difference = std::abs(static_cast<double>(midi) - requested);
                        if (difference < distance) { result = static_cast<double>(midi); distance = difference; }
                    }
                return result;
            }
            return requested;
        };
        pitch_.defaultNoteLengthBeats = [this](double start) { return tempoMap_.beatAt(tempoMap_.secondsAt(start) + 0.5) - start; };
        pitch_.onNoteExpressionEdited = [this](PitchTool tool, int id, double value)
        {
            if (!pitchTrackAvailable()) return;
            const auto* note = findNote(id); if (note == nullptr) return;
            if (tool == PitchTool::vibrato) send(command("set-clip-vibrato", { { "track", selectedTrack_ }, { "clipId", note->id }, { "scale", value } }));
            else if (tool == PitchTool::gain) send(command("set-clip-gain", { { "track", selectedTrack_ }, { "clipId", note->id }, { "gainDb", value } }));
        };
        pitch_.onPitchPreview = [this](double pitch, bool restart) { send(command("preview-pitch-tone", { { "pitch", pitch }, { "restart", restart } })); };
        pitch_.onPitchPreviewEnd = [this] { send(command("stop-pitch-tone")); };
        pitch_.onWheelGesture = [this](const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) { pitchWheel(event, wheel); };
        pitch_.onNotesMoved = [this](const std::vector<PitchEditor::NoteMovement>& movements)
        {
            if (!pitchTrackAvailable()) return;
            juce::Array<juce::var> changes;
            for (const auto& movement : movements)
            {
                const auto* note = findNote(movement.id); if (note == nullptr) continue;
                const auto pitch = std::clamp(std::isfinite(movement.pitch) ? movement.pitch : note->pitch + static_cast<double>(movement.midiNote - juce::roundToInt(note->pitch)), 0.0, 127.0);
                const auto startDelta = tempoMap_.secondsAt(movement.startBeat) - duration_ * note->x / 100.0;
                const auto endDelta = tempoMap_.secondsAt(movement.startBeat + movement.lengthBeats) - duration_ * (note->x + note->width) / 100.0;
                changes.add(object({ { "clipId", note->id }, { "startDeltaSeconds", startDelta }, { "endDeltaSeconds", endDelta }, { "pitch", pitch } }));
            }
            if (!changes.isEmpty()) send(command("move-note-group", { { "track", selectedTrack_ }, { "notes", juce::var(changes) }, { "snapToChord", snapToChord_ } }));
        };
        horizontal_.setComponentID("studio.horizontalScroll"); horizontal_.addListener(this); horizontal_.setAutoHide(false); horizontal_.setSingleStepSize(1.0);
        horizontal_.setColour(juce::ScrollBar::backgroundColourId, colour(tokens::background)); horizontal_.setColour(juce::ScrollBar::thumbColourId, colour(tokens::muted).withAlpha(0.65f));
        exportToast_.addAndMakeVisible(exportLabel_); exportToast_.addAndMakeVisible(exportOpen_); exportToast_.addAndMakeVisible(exportDismiss_); exportToast_.setVisible(false);
        exportLabel_.setColour(juce::Label::textColourId, colour(tokens::text)); exportLabel_.setFont(font(tokens::fontSmall));
        exportOpen_.setComponentID("studio.export.open"); exportDismiss_.setComponentID("studio.export.dismiss");
        exportOpen_.onClick = [this] { const auto path = exportDirectory_; if (juce::File::isAbsolutePath(path)) juce::File(path).revealToUser(); };
        exportDismiss_.onClick = [this] { exportToast_.setVisible(false); };
        status_.setText("Open a project or import audio to begin.", juce::dontSendNotification); status_.setColour(juce::Label::textColourId, colour(tokens::muted)); status_.setFont(font(tokens::fontSmall));
        setWantsKeyboardFocus(true);
        pitch_.setNoteRange(kPitchMinimum, kPitchMaximum); keyboard_.setNoteRange(kPitchMinimum, kPitchMaximum);
        loopRange_ = object({ { "start", 0.0 }, { "end", 25.0 } });
        backingPicker_.onCreate = [this](const juce::String& parent, const juce::String& type, const juce::String& generator)
        { send(command("add-backing-track", { { "track", type }, { "generator", generator }, { "parentId", parent } })); };
        updateEnabled();
        observeShortcutRoot(); installShortcuts(owner_);
    }
    ~Content() override
    {
        if (shortcutRoot_ != nullptr) shortcutRoot_->removeComponentListener(this);
        for (auto target : shortcutTargets_) if (target != nullptr) target->removeKeyListener(this);
        horizontal_.removeListener(this);
        pianoScroll_.onVisibleAreaChange = {}; pianoScroll_.onMagnify = {}; pianoScroll_.onWheel = {};
        scroll_.setViewedComponent(nullptr, false); pianoScroll_.setViewedComponent(nullptr, false);
    }

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
        if (!std::isfinite(scroll) || !std::isfinite(factor) || factor <= 0.0 || !std::isfinite(anchor)) return;
        if (factor != 1.0) view_.zoomAt(factor, anchor);
        if (scroll != 0.0) view_.scroll(scroll);
        refreshViewport();
    }
    void refreshViewport()
    {
        const auto laidOut = ruler_.getWidth() > 0;
        const auto visibleBeats = laidOut ? static_cast<double>(ruler_.getWidth()) / view_.pixelsPerBeat : 1.0;
        const auto totalBeats = std::max(1.0, tempoMap_.beatAt(duration_));
        if (laidOut) view_.firstBeat = std::clamp(view_.firstBeat, 0.0, std::max(0.0, totalBeats - visibleBeats));
        horizontal_.setRangeLimits(0.0, laidOut ? std::max(totalBeats, visibleBeats) : std::max(totalBeats, view_.firstBeat + visibleBeats), juce::dontSendNotification);
        horizontal_.setCurrentRange(view_.firstBeat, visibleBeats, juce::dontSendNotification);
        lowerView_ = view_;
        lowerView_.firstBeat = view_.firstBeat - static_cast<double>(kHeaderWidth - kKeyboardWidth) / view_.pixelsPerBeat;
        for (auto* surface : std::initializer_list<EditorSurface*> { &ruler_, &tempo_, &signature_, &chords_, &lowerRuler_, &pitch_ }) surface->viewportChanged();
        for (auto& row : rows_) row->refresh();
        arrangementPlayhead_.viewportChanged(); lowerPlayhead_.viewportChanged(); musicalEvents_.repaint();
        cycleOverlay_.setRange(number(loopRange_["start"]), number(loopRange_["end"]), loopActive_, duration_, false);
        zoom_.setValue(view_.pixelsPerBeat);

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
    juce::var backingContent(const juce::String& id) const
    {
        for (const auto& item : array(project_["backingTrackContents"])) if (item["id"].toString() == id) return item;
        return {};
    }
    juce::var noteValues(const juce::String& id) const { return id == "Voice Main" ? project_["clips"] : backingContent(id)["clips"]; }
    void populateProfile(Select& selector, const juce::String& trackId, std::vector<juce::String>& ids)
    {
        juce::StringArray names; ids.clear();
        for (const auto& profile : array(project_["voiceProfiles"])) { names.add(profile["name"].toString()); ids.push_back(profile["id"].toString()); }
        selector.setItems(names); const auto selected = backingContent(trackId)["voiceProfileId"].toString();
        const auto found = std::find(ids.begin(), ids.end(), selected); selector.setSelectedIndex(found == ids.end() ? -1 : static_cast<int>(std::distance(ids.begin(), found)));
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
    bool isTrackSelected(const juce::String& id) const
    { return std::find(selectedTracks_.begin(), selectedTracks_.end(), id) != selectedTracks_.end(); }
    void selectTrack(const juce::String& id, bool notify = true, bool additive = false)
    {
        if (findTrack(id) == nullptr) return;
        const auto previous = selectedTracks_;
        if (!additive) selectedTracks_ = { id };
        else if (const auto found = std::find(selectedTracks_.begin(), selectedTracks_.end(), id); found != selectedTracks_.end()) selectedTracks_.erase(found);
        else selectedTracks_.push_back(id);
        selectedTrack_ = selectedTracks_.size() == 1 ? selectedTracks_.front() : juce::String();
        if (selectedTracks_ != previous) { selectedClip_.clear(); selectedNote_.clear(); selectedNotes_.clear(); rebuildPitch(); rebuildPitchWaveforms(); }
        trackSelect_.setSelectedIndex(-1);
        for (std::size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].id == selectedTrack_) trackSelect_.setSelectedIndex(static_cast<int>(i));
        for (auto& row : rows_) row->updateSelection();
        updateEnabled();
        if (notify)
        {
            juce::Array<juce::var> values; for (const auto& track : selectedTracks_) values.add(track);
            send(command("select-tracks", { { "tracks", juce::var(values) } }));
        }
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
    void applyTools()
    {
        constexpr PitchTool tools[] { PitchTool::pointer, PitchTool::pencil, PitchTool::eraser, PitchTool::scissors, PitchTool::join, PitchTool::flex, PitchTool::vibrato, PitchTool::gain, PitchTool::zoom };
        pitch_.setTool(tools[std::clamp(primaryTool_.getSelectedIndex(), 0, 8)]);
        pitch_.setSecondaryTool(tools[std::clamp(secondaryTool_.getSelectedIndex(), 0, 8)]);
        primaryToolTip_.setText("Left click: " + primaryTool_.getSelectedText());
        secondaryToolTip_.setText((juce::SystemStats::getOperatingSystemType() & juce::SystemStats::MacOSX ? juce::String("Command-click: ") : juce::String("Control-click: ")) + secondaryTool_.getSelectedText());
    }
    void createNote(double beat, double length, double pitch)
    {
        if (!pitchTrackAvailable() || duration_ <= 0.0) return;
        const auto start = beatToPercent(beat), end = beatToPercent(beat + length);
        send(command("add-clip", { { "track", selectedTrack_ }, { "snapToChord", snapToChord_ },
            { "clip", object({ { "id", juce::Uuid().toString() }, { "x", start }, { "width", std::max(0.001, end - start) }, { "pitch", std::clamp(pitch, 0.0, 127.0) } }) } }));
    }
    void editNote(PitchTool tool, int id, double beat, int midi)
    {
        if (!pitchTrackAvailable()) return;
        if (tool == PitchTool::pencil)
        {
            const auto length = tempoMap_.beatAt(tempoMap_.secondsAt(beat) + 0.5) - beat;
            createNote(beat, length, static_cast<double>(midi));
            return;
        }
        const auto* note = findNote(id);
        if (note == nullptr) return;
        if (tool == PitchTool::scissors)
            send(command("split-clip", { { "track", selectedTrack_ }, { "clipId", note->id }, { "x", beatToPercent(beat) }, { "rightClipId", juce::Uuid().toString() }, { "snapToChord", snapToChord_ } }));
        else if (tool == PitchTool::eraser)
            send(command("delete-clips", { { "track", selectedTrack_ }, { "clipIds", juce::var(juce::Array<juce::var>{ juce::var(note->id) }) } }));
        else if (tool == PitchTool::join)
        {
            juce::Array<juce::var> ids;
            if (selectedNotes_.contains(note->id) && selectedNotes_.size() > 1)
                for (const auto& selected : selectedNotes_) ids.add(selected);
            else if (notes_.size() > 1)
            {
                const auto index = static_cast<std::size_t>(note - notes_.data());
                if (index + 1 < notes_.size()) { ids.add(note->id); ids.add(notes_[index + 1].id); }
                else { ids.add(notes_[index - 1].id); ids.add(note->id); }
            }
            if (ids.size() > 1) send(command("join-clips", { { "track", selectedTrack_ }, { "clipIds", juce::var(ids) }, { "snapToChord", snapToChord_ } }));
        }
    }
    bool pitchTrackAvailable() const
    {
        if (selectedTracks_.size() != 1) return false;
        if (selectedTrack_ == "Voice Main") return true;
        for (const auto& content : array(project_["backingTrackContents"])) if (content["id"].toString() == selectedTrack_) return true;
        return false;
    }
    void deleteSelection()
    {
        if (!selectedNotes_.empty())
        {
            if (!pitchTrackAvailable()) return;
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
        if (!pitchTrackAvailable() || duration_ <= 0.0) { status_.setText("Select one vocal track to edit notes.", juce::dontSendNotification); return; }
        const auto* selected = findNote(pitch_.selectedNote());
        const auto id = selected != nullptr ? selected->id : juce::String();
        std::vector<Menu::Item> items;
        items.push_back({ "Snap pitch edits to chord", [this] { snapToChord_ = !snapToChord_; chordSnap_.setToggleState(snapToChord_); }, true, snapToChord_ });
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
        menu_.setItems(std::move(items)); menu_.showFor(toolbar_, 260); installShortcuts(menu_);
    }
    void showBackingPicker(juce::String parentId, juce::Component& anchor)
    {
        if (busy_) return;
        if (parentId.isEmpty() || parentId == "Instrumental") { status_.setText("Select one vocal track before adding backing.", juce::dontSendNotification); return; }
        const auto* track = findTrack(parentId); if (track == nullptr) return;
        juce::StringArray types; for (const auto& value : array(project_["backingTrackOptions"])) types.add(value.toString());
        if (types.isEmpty()) { status_.setText("No backing types are available for this project.", juce::dontSendNotification); return; }
        backingPicker_.show(parentId, track->name, types, anchor); installShortcuts(backingPicker_);
    }
    void showTrackMenu(const juce::String& id, juce::Component& anchor)
    {
        const auto* track = findTrack(id); if (track == nullptr) return;
        const auto audioMuted = track->audioMuted, notesMuted = track->notesMuted;
        const auto hasNotes = !array(noteValues(id)).isEmpty(), hasAudio = !array(track->source["clips"]).isEmpty();
        std::vector<Menu::Item> items {
            { "Import audio...", [this, id] { send(command("import-track-audio", { { "track", id } })); }, !busy_ && track->parent.isEmpty() },
            { "Audio layer", [this, id, audioMuted, notesMuted] { send(command("set-track-layer-state", { { "track", id }, { "state", object({ { "audioMuted", !audioMuted }, { "notesMuted", notesMuted } }) } })); }, hasAudio, !audioMuted },
            { "Note layer", [this, id, audioMuted, notesMuted] { send(command("set-track-layer-state", { { "track", id }, { "state", object({ { "audioMuted", audioMuted }, { "notesMuted", !notesMuted } }) } })); }, id != "Instrumental" && hasNotes, !notesMuted },
            { "Analyze selected track", [this, id] { selectTrack(id); send(command("project-action", { { "action", "analyze" } })); }, id != "Instrumental" && !busy_ },
            { "Add backing...", [this, id, anchorPointer = juce::Component::SafePointer<juce::Component>(&anchor)] { selectTrack(id); if (anchorPointer != nullptr) showBackingPicker(id, *anchorPointer); }, id != "Instrumental" && !busy_ && (hasNotes || hasAudio) }
        };
        if (track->parent.isNotEmpty())
        {
            items.push_back({ "Regenerate backing", [this, id] { send(command("regenerate-backing-track", { { "track", id }, { "generator", backingPicker_.generator() } })); }, !busy_ && hasNotes });
            items.push_back({ "Render backing audio", [this, id] { send(command("render-backing-track", { { "track", id } })); }, !busy_ && hasNotes });
            items.push_back({ "Remove backing track", [this, id] { send(command("remove-backing-track", { { "track", id } })); }, !busy_ });
        }
        else if (id != "Instrumental") items.push_back({ "Remove track", [this, id] { send(command("remove-track", { { "track", id } })); }, !busy_ });
        menu_.setItems(std::move(items)); menu_.showFor(anchor, 260); installShortcuts(menu_);
    }
    void showClipMenu(const juce::String& track, const juce::String& clip, juce::Component& anchor)
    {
        menu_.setItems({ { "Select track", [this, track] { selectTrack(track); } },
            { "Remove audio clip", [this, track, clip] { send(command("remove-arrangement-clip", { { "track", track }, { "clipId", clip } })); }, !busy_ } });
        menu_.showFor(anchor, 230); installShortcuts(menu_);
    }
    juce::String trackAt(juce::Point<int> position) const
    {
        const auto point = arrangement_.getLocalPoint(this, position);
        for (const auto& row : rows_) if (row->isVisible() && row->getBounds().contains(point)) return row->id();
        return {};
    }
    juce::String selectedRoot() const
    {
        if (selectedTracks_.size() != 1) return {};
        auto id = selectedTracks_.front(); std::set<juce::String> visited;
        while (visited.insert(id).second)
        {
            const auto* track = findTrack(id); if (track == nullptr) return {};
            if (track->parent.isEmpty()) return id;
            id = track->parent;
        }
        return {};
    }
    void importFiles(const juce::StringArray& paths, juce::Point<int> position)
    {
        if (busy_) { status_.setText("Wait for the current operation before importing audio.", juce::dontSendNotification); return; }
        juce::Array<juce::var> files;
        for (const auto& path : paths) if (juce::File::isAbsolutePath(path)) files.add(path);
        if (files.isEmpty()) return;
        auto track = trackAt(position);
        const auto local = arrangement_.getLocalPoint(this, position);
        const auto actualLane = scroll_.getBounds().contains(position) && local.x >= kHeaderWidth && track.isNotEmpty();
        const auto seconds = actualLane ? tempoMap_.secondsAt(snapBeat(view_.beatAtX(static_cast<float>(local.x - kHeaderWidth)))) : duration_ * playheadPercent_ / 100.0;
        if (!actualLane) track = selectedRoot();
        send(command("import-dropped-audio", { { "track", track }, { "start", seconds }, { "files", juce::var(files) } }));
    }
    void rebuildRows()
    {
        juce::String focusId;
        if (auto* focused = juce::Component::getCurrentlyFocusedComponent(); focused != nullptr && arrangement_.isParentOf(focused)) focusId = focused->getComponentID();
        menu_.dismiss(); backingPicker_.dismiss();
        rows_.clear(); groups_.clear();
        for (std::size_t index = 0; index < tracks_.size(); ++index)
        {
            auto row = std::make_unique<Row>(*this, tracks_[index], static_cast<int>(index));
            arrangement_.addAndMakeVisible(*row); rows_.push_back(std::move(row));
            const auto& track = tracks_[index];
            if ((track.parent.isEmpty() && track.id != "Instrumental") || std::any_of(tracks_.begin(), tracks_.end(), [&](const auto& child) { return child.parent == track.id; }))
            {
                auto group = std::make_unique<TrackGroupHeader>();
                group->setComponentID("studio.group." + track.id); group->setAddCompact(true);
                group->setTitle("Backing Vocals"); static_cast<juce::Component&>(*group).setTitle("Backing vocals for " + track.name); group->setDescription("Backing vocals for " + track.name); group->setExpanded(!track.collapsed);
                for (auto* child : group->getChildren()) if (child->getComponentID() == "group.toggle") child->setTitle("Backing vocals for " + track.name);
                for (auto* child : group->getChildren()) if (child->getComponentID() == "group.add") { child->setComponentID("studio.addBacking." + track.id); child->setTitle("Add backing to " + track.name); }
                group->onExpanded = [this, id = track.id](bool expanded) { send(command("set-track-collapsed", { { "track", id }, { "collapsed", !expanded } })); };
                group->onAdd = [this, id = track.id] { selectTrack(id); if (const auto found = groups_.find(id); found != groups_.end()) showBackingPicker(id, *found->second); };
                arrangement_.addAndMakeVisible(*group); groups_.emplace(track.id, std::move(group));
            }
        }
        for (auto& row : rows_) row->updateSelection();
        installShortcuts(owner_);
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
    const AudioClipData& preparedClip(const juce::String& trackId, const juce::var& clip)
    {
        const auto key = trackId + "\n" + clip["id"].toString();
        const auto existing = preparedClips_.find(key);
        if (existing != preparedClips_.end()) return existing->second;
        const auto* track = findTrack(trackId);
        return preparedClips_.emplace(key, prepareClipWaveform(clip, tempoMap_, trackColour(trackId, track != nullptr && track->parent.isNotEmpty()))).first->second;
    }
    void rebuildPitchWaveforms()
    {
        std::vector<PitchWaveformRegion> regions;
        if (selectedTracks_.size() == 1)
        {
            const auto& id = selectedTracks_.front(); const auto* track = findTrack(id);
            const auto* source = track != nullptr ? track->source.getDynamicObject() : nullptr;
            if (source != nullptr && source->hasProperty("clips"))
            {
                // An empty arrangement is authoritative: never substitute a
                // parent track's audio or an obsolete legacy waveform.
                for (const auto& clip : array(track->source["clips"]))
                {
                    const auto& data = preparedClip(id, clip);
                    if (!data.peaks.empty() && data.lengthBeats > 0.0) regions.push_back({ data.startBeat, data.lengthBeats, data.peaks, data.colour });
                }
            }
            else
            {
                const auto content = backingContent(id);
                const auto waveform = id == "Instrumental" ? project_["instrumentalWaveform"] : id == "Voice Main" ? project_["vocalWaveform"] : content["waveform"];
                const auto duration = id == "Instrumental" ? number(project_["instrumentalDurationSeconds"]) : id == "Voice Main" ? number(project_["vocalDurationSeconds"]) : number(content["audioDurationSeconds"]);
                const auto clip = object({ { "id", "legacy-waveform" }, { "start", 0.0 }, { "duration", duration }, { "offset", 0.0 }, { "waveformDurationSeconds", duration }, { "waveform", waveform } });
                const auto& data = preparedClip(id, clip);
                if (!data.peaks.empty() && data.lengthBeats > 0.0) regions.push_back({ data.startBeat, data.lengthBeats, data.peaks, data.colour });
            }
        }
        pitch_.setWaveformRegions(std::move(regions));
        pitch_.setWaveformViewport(static_cast<float>(pianoScroll_.getViewPositionY()), static_cast<float>(pianoScroll_.getHeight()));
    }
    void rebuildPitch()
    {
        notes_.clear();
        std::vector<std::pair<juce::String, juce::var>> noteGroups;
        for (const auto& trackId : selectedTracks_)
        {
            if (trackId == "Voice Main") noteGroups.emplace_back(trackId, project_["clips"]);
            else for (const auto& track : array(project_["backingTrackContents"])) if (track["id"].toString() == trackId) { noteGroups.emplace_back(trackId, track["clips"]); break; }
        }
        std::vector<NoteSegment> segments;

        for (const auto& [trackId, values] : noteGroups)
        for (const auto& value : array(values))
        {
            Note note; note.track = trackId;
            note.id = value["id"].toString(); if (note.id.isEmpty()) continue;
            const auto key = trackId + "\n" + note.id;
            auto found = noteIds_.find(key);
            if (found == noteIds_.end()) found = noteIds_.emplace(key, nextNoteId_++).first;
            note.localId = found->second; note.x = number(value["x"]); note.width = number(value["width"]); note.pitch = std::clamp(number(value["pitch"], 60.0), 0.0, 127.0); note.source = value;
            if (note.width <= 0.0) continue;
            NoteSegment segment;
            segment.id = note.localId; segment.startBeat = percentToBeat(note.x); segment.lengthBeats = percentToBeat(note.x + note.width) - segment.startBeat;
            segment.midiNote = juce::roundToInt(note.pitch); segment.pitch = note.pitch; segment.gainDb = number(value["gainDb"]); segment.colour = trackColour(trackId, trackId != "Voice Main"); segment.ghost = selectedTracks_.size() > 1;
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
                segment.pitchOffsets.push_back(static_cast<float>(pitchValue - note.pitch));
            }
            notes_.push_back(std::move(note)); segments.push_back(std::move(segment));
        }
        std::sort(notes_.begin(), notes_.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
        pitch_.setNotes(std::move(segments));
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
        for (auto& [id, group] : groups_)
            for (auto* child : group->getChildren()) if (child->getComponentID() == "studio.addBacking." + id) child->setEnabled(!busy_);
        primaryTool_.setEnabled(duration_ > 0.0); secondaryTool_.setEnabled(duration_ > 0.0); pitch_.setEnabled(duration_ > 0.0); pitch_.setReadOnly(!editable);
        chordSnap_.setEnabled(editable && !array(project_["chords"]).isEmpty());
        bool canUndo = false, canRedo = false;
        for (const auto& item : array(project_["pitchEditorHistory"])) if (item["track"].toString() == selectedTrack_) { canUndo = static_cast<bool>(item["canUndo"]); canRedo = static_cast<bool>(item["canRedo"]); }
        undo_.setEnabled(canUndo); redo_.setEnabled(canRedo);
    }
    void showExport(const juce::var& event)
    {
        exportDirectory_ = event["directory"].toString();
        status_.setText(event["message"].toString(), juce::dontSendNotification);
        exportLabel_.setText(event["message"].toString() + (exportDirectory_.isNotEmpty() ? "  " + exportDirectory_ : juce::String()), juce::dontSendNotification);
        exportOpen_.setEnabled(juce::File::isAbsolutePath(exportDirectory_) && juce::File(exportDirectory_).isDirectory());
        exportToast_.setVisible(true); exportToast_.toFront(false); resized();
    }
    void dispatch(const juce::var& event)
    {
        const auto type = event["type"].toString();
        if (type == "project-state")
        {
            project_ = event["project"]; tempoMap_.set(project_); preparedClips_.clear();
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
            { selectedTracks_ = { pendingTrack_ }; pendingTrack_.clear(); selectedNote_.clear(); selectedNotes_.clear(); selectedClip_.clear(); }
            selectedTracks_.erase(std::remove_if(selectedTracks_.begin(), selectedTracks_.end(), [this](const auto& id) { return findTrack(id) == nullptr; }), selectedTracks_.end());
            if (!selectionInitialised_ && selectedTracks_.empty())
            {
                const auto fallback = findTrack("Voice Main") != nullptr ? juce::String("Voice Main") : tracks_.empty() ? juce::String() : tracks_.front().id;
                if (fallback.isNotEmpty()) selectedTracks_.push_back(fallback);
                selectedNote_.clear(); selectedNotes_.clear(); selectedClip_.clear();
            }
            selectionInitialised_ = true;
            selectedTrack_ = selectedTracks_.size() == 1 ? selectedTracks_.front() : juce::String();
            trackSelect_.setItems(names); trackSelect_.setSelectedIndex(-1);
            for (std::size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i].id == selectedTrack_) trackSelect_.setSelectedIndex(static_cast<int>(i));
            instrumentalVolume_ = std::clamp(number(project_["initialInstrumentalVolume"], 100.0), 0.0, 100.0);
            transport_.setMasterVolume(std::clamp(number(project_["initialVolume"], 100.0), 0.0, 100.0) / 100.0);
            loopActive_ = static_cast<bool>(project_["initialLoopActive"]); transport_.setLooping(loopActive_); loopRange_ = project_["initialCycleRange"];
            if (!loopRange_.isObject()) loopRange_ = object({ { "start", 0.0 }, { "end", 25.0 } });
            cycleOverlay_.setRange(number(loopRange_["start"]), number(loopRange_["end"]), loopActive_, duration_);
            rebuildRows(); rebuildPitch(); rebuildPitchWaveforms();
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
        else if (type == "loop-state") { loopActive_ = static_cast<bool>(event["active"]); transport_.setLooping(loopActive_); loopRange_ = event["range"]; cycleOverlay_.setRange(number(loopRange_["start"]), number(loopRange_["end"]), loopActive_, duration_); }
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
        else if (type == "status-state")
        {
            busy_ = static_cast<bool>(event["vocalAnalysisRunning"]) || static_cast<bool>(event["instrumentalAnalysisRunning"]) || static_cast<bool>(event["backingGenerationRunning"]) || static_cast<bool>(event["backingAudioRenderRunning"]);
            generationTrack_ = static_cast<bool>(event["backingGenerationRunning"]) ? event["backingGenerationTrack"].toString() : juce::String();
            renderTrack_ = static_cast<bool>(event["backingAudioRenderRunning"]) ? event["backingAudioRenderTrack"].toString() : juce::String();
            status_.setText(event["message"].toString(), juce::dontSendNotification);
            for (auto& row : rows_) row->updateControls();
            updateEnabled();
        }
        else if (type == "voice-training-state")
        {
            const auto state = event["state"]; auto progress = std::clamp(number(state["progress"]), 0.0, 1.0);
            auto message = state["message"].toString();
            for (const auto& job : array(state["jobs"])) if (static_cast<bool>(job["canCancel"])) { progress = std::clamp(number(job["progress"]), 0.0, 1.0); message = job["presetName"].toString() + ": " + job["message"].toString(); break; }
            const auto phase = state["phase"].toString();
            const auto active = phase == "preparing" || phase == "cancelling" || std::any_of(array(state["jobs"]).begin(), array(state["jobs"]).end(), [](const auto& job) { return static_cast<bool>(job["canCancel"]); });
            if (active) status_.setText("Voice " + juce::String(juce::roundToInt(progress * 100.0)) + "%: " + message, juce::dontSendNotification);
        }
        else if (type == "export-complete") showExport(event);
    }
    NativeStudioView::Snapshot state() const
    {
        NativeStudioView::Snapshot result;
        result.trackCount = static_cast<int>(tracks_.size()); result.noteCount = static_cast<int>(notes_.size());
        for (const auto& track : tracks_) result.clipCount += array(track.source["clips"]).size();
        result.selectedTrackCount = static_cast<int>(selectedTracks_.size()); result.pitchReadOnly = pitch_.isReadOnly();
        result.loopActive = loopActive_; result.loopStart = number(loopRange_["start"]); result.loopEnd = number(loopRange_["end"]);
        result.verticalZoom = verticalZoom_; result.pianoCollapsed = pianoCollapsed_;
        result.lowestMidi = kPitchMinimum; result.highestMidi = kPitchMaximum; result.pianoScrollY = pianoScroll_.getViewPositionY();
        result.selectedTrack = selectedTrack_; result.selectedNoteId = selectedNote_; result.playheadSeconds = duration_ * playheadPercent_ / 100.0; result.pixelsPerBeat = view_.pixelsPerBeat;
        return result;
    }
    void paint(juce::Graphics& g) override { g.fillAll(colour(tokens::background)); }
    void layoutPianoCanvas()
    {
        const auto canvasHeight = juce::roundToInt(kPitchRowHeight * static_cast<double>(kPitchMaximum - kPitchMinimum + 1) * verticalZoom_);
        pianoCanvas_.setSize(pianoScroll_.getWidth(), canvasHeight);
        keyboard_.setBounds(2, 0, kKeyboardWidth - 4, canvasHeight);
        pitch_.setBounds(kKeyboardWidth, 0, std::max(0, pianoCanvas_.getWidth() - kKeyboardWidth), canvasHeight);
        pitch_.setWaveformViewport(static_cast<float>(pianoScroll_.getViewPositionY()), static_cast<float>(pianoScroll_.getHeight()));
        if (!pianoViewportInitialised_ && pianoScroll_.getHeight() > 0)
        {
            const auto centreC4 = (static_cast<double>(kPitchMaximum - 60) + 0.5) * kPitchRowHeight * verticalZoom_;
            pianoScroll_.setViewPosition(0, juce::roundToInt(centreC4) - pianoScroll_.getHeight() / 2);
            pianoViewportInitialised_ = true;
        }
    }
    void setVerticalZoom(double zoom, float requestedAnchor = -1.0f)
    {
        if (!std::isfinite(zoom)) return;
        if (!std::isfinite(requestedAnchor)) requestedAnchor = -1.0f;
        zoom = std::clamp(zoom, 1.0, 4.0);
        if (std::abs(zoom - verticalZoom_) < 0.000001) return;
        auto anchor = requestedAnchor;
        if (anchor < 0.0f)
        {
            anchor = static_cast<float>(pianoScroll_.getHeight()) * 0.5f;
            const auto selected = pitch_.noteBounds(pitch_.selectedNote());
            const auto selectedY = selected.getCentreY() - static_cast<float>(pianoScroll_.getViewPositionY());
            if (!selected.isEmpty() && selectedY >= 0.0f && selectedY <= static_cast<float>(pianoScroll_.getHeight())) anchor = selectedY;
        }
        anchor = std::clamp(anchor, 0.0f, static_cast<float>(std::max(0, pianoScroll_.getHeight())));
        const auto unscaledY = (static_cast<double>(pianoScroll_.getViewPositionY()) + static_cast<double>(anchor)) / verticalZoom_;
        verticalZoom_ = zoom;
        layoutPianoCanvas();
        pianoScroll_.setViewPosition(0, juce::roundToInt(unscaledY * verticalZoom_ - static_cast<double>(anchor)));
        pitchZoom_.setValue(verticalZoom_);
    }
    static double dominantWheelDelta(const juce::MouseWheelDetails& wheel)
    { return std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? static_cast<double>(wheel.deltaX) : static_cast<double>(wheel.deltaY); }
    static double wheelZoomFactor(double delta) { return std::exp(std::clamp(delta * 1.7, -1.4, 1.4)); }
    void magnify(const juce::MouseEvent& event, float requestedFactor)
    {
        if (!std::isfinite(requestedFactor) || requestedFactor <= 0.0f || !std::isfinite(event.position.x) || !std::isfinite(event.position.y)) return;
        const auto factor = static_cast<double>(std::clamp(requestedFactor, 0.25f, 4.0f));
        const auto local = getLocalPoint(event.eventComponent, event.position);
        if (event.mods.isAltDown())
        {
            const auto inPiano = pianoScroll_.getBounds().contains(local.toInt());
            const auto anchorY = inPiano ? pianoScroll_.getLocalPoint(event.eventComponent, event.position).y : -1.0f;
            setVerticalZoom(verticalZoom_ * factor, anchorY);
        }
        else gesture(0.0, factor, local.x - static_cast<float>(kHeaderWidth));
    }
    void mouseMagnify(const juce::MouseEvent& event, float factor) override { magnify(event, factor); }
    void pitchWheel(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
    {
        if (!std::isfinite(wheel.deltaX) || !std::isfinite(wheel.deltaY) || !std::isfinite(event.position.x) || !std::isfinite(event.position.y)) return;
        const auto delta = dominantWheelDelta(wheel);
        if (delta == 0.0) return;
        const auto local = getLocalPoint(event.eventComponent, event.position);
        const auto anchorX = local.x - static_cast<float>(kHeaderWidth);
        if (event.mods.isAltDown())
        {
            const auto inPiano = pianoScroll_.getBounds().contains(local.toInt());
            const auto anchor = inPiano ? pianoScroll_.getLocalPoint(event.eventComponent, event.position).y : -1.0f;
            setVerticalZoom(verticalZoom_ * wheelZoomFactor(delta), anchor);
        }
        else if (event.mods.isCommandDown() || event.mods.isCtrlDown()) gesture(0.0, wheelZoomFactor(delta), anchorX);
        else if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY)) gesture(-delta * 160.0 / view_.pixelsPerBeat, 1.0, anchorX);
        else pianoScroll_.setViewPosition(0, pianoScroll_.getViewPositionY() - juce::roundToInt(wheel.deltaY * 220.0f));
    }
    void resized() override
    {
        auto bounds = getLocalBounds();
        transport_.setBounds(bounds.removeFromTop(kTransportHeight));
        status_.setBounds(bounds.removeFromBottom(kStatusHeight));
        horizontal_.setBounds(bounds.removeFromBottom(16).withTrimmedLeft(kKeyboardWidth).reduced(4, 1));
        constexpr int toolbarHeight = 42;
        auto pitchArea = juce::Rectangle<int>();
        if (!pianoCollapsed_)
        {
            const auto pitchHeight = std::clamp(bounds.getHeight() * 37 / 100, 150, 360);
            pitchArea = bounds.removeFromBottom(std::min(pitchHeight, std::max(0, bounds.getHeight() - toolbarHeight - 120)));
            auto pitchRuler = pitchArea.removeFromTop(kRulerHeight);
            lowerRuler_.setBounds(pitchRuler.withTrimmedLeft(kKeyboardWidth));
            pianoScroll_.setBounds(pitchArea); layoutPianoCanvas();
            lowerPlayhead_.setBounds(lowerRuler_.getX(), lowerRuler_.getY(), lowerRuler_.getWidth(), lowerRuler_.getHeight() + pianoScroll_.getHeight());
        }
        lowerRuler_.setVisible(!pianoCollapsed_); pianoScroll_.setVisible(!pianoCollapsed_); lowerPlayhead_.setVisible(!pianoCollapsed_);
        toolbar_.setBounds(bounds.removeFromBottom(toolbarHeight));
        auto tools = toolbar_.getLocalBounds().reduced(10, 6);
        const auto placeControl = [](juce::Component& component, juce::Rectangle<int>& row, int width, int gap = 4)
        { component.setBounds(row.removeFromLeft(std::min(width, row.getWidth()))); row.removeFromLeft(gap); };
        auto left = tools;
        placeControl(pitchZoom_, left, 96, 10);
        placeControl(undo_, left, 28, 4); placeControl(redo_, left, 28, 10);
        placeControl(trackSelect_, left, 110, 8); placeControl(snap_, left, 70, 8);
        auto right = tools;
        pitchCollapse_.setBounds(right.removeFromRight(28)); right.removeFromRight(10);
        zoomIn_.setBounds(right.removeFromRight(28)); right.removeFromRight(4);
        zoom_.setBounds(right.removeFromRight(104)); right.removeFromRight(4);
        zoomOut_.setBounds(right.removeFromRight(28));
        constexpr int toolGroupWidth = 230;
        const auto centredX = (toolbar_.getWidth() - toolGroupWidth) / 2;
        const auto groupX = std::max(left.getX() + 2, std::min(centredX, zoomOut_.getX() - toolGroupWidth - 8));
        auto centre = juce::Rectangle<int>(groupX, tools.getY(), toolGroupWidth, tools.getHeight());
        placeControl(primaryTool_, centre, 52, 6); placeControl(secondaryTool_, centre, 52, 8); placeControl(chordSnap_, centre, 112, 0);
        scroll_.setBounds(bounds);
        const auto globalHeight = (arrangement_.globalsExpanded ? 132 : 36) + kCycleHeight;
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
                if (visible) { group->second->setBounds(8, y, kHeaderWidth - 10, 38); y += 40; }
            }
            const auto childrenVisible = visible && track != nullptr && !track->collapsed;
            for (const auto& child : tracks_) if (child.parent == id) self(self, child.id, childrenVisible);
        };
        for (const auto& track : tracks_) if (track.parent.isEmpty() || findTrack(track.parent) == nullptr) place(place, track.id, true);
        for (const auto& track : tracks_) if (!laidOut.contains(track.id)) place(place, track.id, true);
        arrangement_.setSize(scroll_.getWidth(), std::max(scroll_.getHeight(), y + 8));
        global_.setBounds(12, 4, kHeaderWidth - 14, 30);
        const auto timeWidth = std::max(0, arrangement_.getWidth() - kHeaderWidth);
        ruler_.setBounds(kHeaderWidth, 4 + kCycleHeight, timeWidth, 30);
        for (auto* lane : std::initializer_list<juce::Component*> { &tempo_, &signature_, &chords_ }) lane->setVisible(arrangement_.globalsExpanded);
        tempo_.setBounds(kHeaderWidth, 36 + kCycleHeight, timeWidth, 32); signature_.setBounds(kHeaderWidth, 68 + kCycleHeight, timeWidth, 32); chords_.setBounds(kHeaderWidth, 100 + kCycleHeight, timeWidth, 32);
        musicalEvents_.setBounds(kHeaderWidth, 36 + kCycleHeight, timeWidth, 64); musicalEvents_.setVisible(arrangement_.globalsExpanded); musicalEvents_.toFront(false);
        cycleOverlay_.setBounds(kHeaderWidth, 4, timeWidth, kCycleHeight); cycleOverlay_.toFront(false);
        addTrack_.setBounds(14, globalHeight + 6, 150, 28);
        arrangementPlayhead_.setBounds(kHeaderWidth, 26 + kCycleHeight, timeWidth, arrangement_.getHeight() - 26 - kCycleHeight); arrangementPlayhead_.toFront(false); lowerPlayhead_.toFront(false);
        exportToast_.setBounds(12, std::max(0, getHeight() - 100), std::min(760, std::max(0, getWidth() - 24)), 64);
        auto toast = exportToast_.getLocalBounds().reduced(8); exportDismiss_.setBounds(toast.removeFromRight(72).withSizeKeepingCentre(72, 28)); toast.removeFromRight(6); exportOpen_.setBounds(toast.removeFromRight(100).withSizeKeepingCentre(100, 28)); toast.removeFromRight(8); exportLabel_.setBounds(toast);
        if (exportToast_.isVisible()) exportToast_.toFront(false);
        refreshViewport();
    }
    void arrangementWheel(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel, float anchor)
    {
        if (!std::isfinite(wheel.deltaX) || !std::isfinite(wheel.deltaY) || !std::isfinite(event.position.x) || !std::isfinite(event.position.y)) return;
        const auto delta = dominantWheelDelta(wheel);
        if (delta == 0.0) return;
        if (event.mods.isAltDown()) setVerticalZoom(verticalZoom_ * wheelZoomFactor(delta));
        else if (event.mods.isCommandDown() || event.mods.isCtrlDown()) gesture(0.0, wheelZoomFactor(delta), anchor);
        else if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY)) gesture(-delta * 160.0 / view_.pixelsPerBeat, 1.0, anchor);
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
    bool performShortcut(const juce::String& action)
    {
        if (action.startsWith("tool.left.") || action.startsWith("tool.right."))
        {
            const juce::StringArray tools { "pointer", "pencil", "eraser", "scissors", "join", "flex", "vibrato", "gain", "zoom" };
            const auto index = tools.indexOf(action.fromLastOccurrenceOf(".", false, false));
            if (index >= 0) { auto& selector = action.startsWith("tool.left.") ? primaryTool_ : secondaryTool_; selector.setSelectedIndex(index); applyTools(); }
            return true;
        }
        if (action == "app.settings") { const auto callback = owner_.onShowSettings; if (callback) callback(); return true; }
        if (action.startsWith("project."))
        {
            const auto suffix = action.substring(8);
            const auto value = suffix == "new" ? "new-project" : suffix == "open" ? "open-project" : suffix == "save" ? "save-project" : suffix == "export" ? "export-all-tracks" : suffix;
            send(command("project-action", { { "action", value } })); return true;
        }
        if (action == "transport.play-pause") { send(command("transport", { { "action", playing_ ? "pause" : "play" } })); return true; }
        if (action == "transport.return-to-start") { send(command("transport", { { "action", "return-to-start" } })); return true; }
        if (action == "transport.toggle-loop") { send(command("set-loop", { { "active", !loopActive_ }, { "range", loopRange_ } })); return true; }
        if (action == "editor.toggle-piano") { pitchCollapse_.triggerClick(); return true; }
        if (action == "editor.toggle-chord-snap") { if (chordSnap_.isEnabled()) chordSnap_.triggerClick(); return true; }
        if (action.startsWith("view."))
        {
            const auto factor = action.endsWith("zoom-out") ? 0.8 : 1.25;
            if (action.startsWith("view.vertical")) setVerticalZoom(verticalZoom_ * factor);
            else gesture(0.0, factor, static_cast<float>(ruler_.getWidth()) * 0.5f);
            return true;
        }
        return false;
    }
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (auto* focus = juce::Component::getCurrentlyFocusedComponent(); dynamic_cast<juce::TextEditor*>(focus) != nullptr) return false;
        const auto action = shortcuts_->actionFor(key);
        if (action.isNotEmpty()) return performShortcut(action);
        if (arrangementKey(key)) return true;
        if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
        if ((key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) && juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) == 'a')
        { selectAllNotes(); return true; }
        return false;
    }
private:
    void scrollBarMoved(juce::ScrollBar* scrollBar, double start) override
    { if (scrollBar == &horizontal_) { view_.firstBeat = start; refreshViewport(); } }
    void installShortcuts(juce::Component& component)
    {
        if (dynamic_cast<juce::TextEditor*>(&component) != nullptr) return;
        shortcutTargets_.erase(std::remove_if(shortcutTargets_.begin(), shortcutTargets_.end(), [](const auto& target) { return target == nullptr; }), shortcutTargets_.end());
        if (std::none_of(shortcutTargets_.begin(), shortcutTargets_.end(), [&component](const auto& target) { return target.getComponent() == &component; }))
        { component.addKeyListener(this); shortcutTargets_.emplace_back(&component); }
        for (auto* child : component.getChildren()) installShortcuts(*child);
    }
    void observeShortcutRoot()
    {
        auto* root = owner_.getTopLevelComponent();
        if (shortcutRoot_.getComponent() == root) return;
        if (shortcutRoot_ != nullptr) shortcutRoot_->removeComponentListener(this);
        shortcutRoot_ = root; if (shortcutRoot_ != nullptr) shortcutRoot_->addComponentListener(this);
    }
    void parentHierarchyChanged() override { observeShortcutRoot(); }
    void componentChildrenChanged(juce::Component& component) override
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        const auto ownsFocus = focused != nullptr && (focused == &owner_ || owner_.isParentOf(focused) || menu_.isParentOf(focused) || backingPicker_.isParentOf(focused));
        if (!ownsFocus) return;
        // Select opens its owned menu in the top-level window; register that
        // transient subtree without touching the separate settings editor.
        for (auto* child : component.getChildren()) if (dynamic_cast<Popover*>(child) != nullptr) installShortcuts(*child);
    }
    bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override
    {
        if (dynamic_cast<juce::TextEditor*>(origin) != nullptr) return false;
        const auto action = shortcuts_->actionFor(key);
        return action.isNotEmpty() && performShortcut(action);
    }
    NativeStudioView& owner_; CommandHandler handler_;
    std::shared_ptr<StudioShortcuts> shortcuts_;
    std::vector<juce::Component::SafePointer<juce::Component>> shortcutTargets_;
    juce::Component::SafePointer<juce::Component> shortcutRoot_;
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
    Select trackSelect_, snap_, primaryTool_, secondaryTool_;
    Tooltip primaryToolTip_ { primaryTool_, "Left click: Pointer" }, secondaryToolTip_ { secondaryTool_, "Command-click: Scissors" };
    ToggleButton chordSnap_ { "Chord Snap" };
    Slider pitchZoom_;
    Tooltip verticalZoomTip_ { pitchZoom_, "Vertical pitch zoom (Alt + wheel)" };
    IconButton pitchCollapse_ { Icon::chevronDown, "Collapse Piano Roll" };
    IconButton undo_ { Icon::undo, "Undo pitch edit" }, redo_ { Icon::redo, "Redo pitch edit" };
    IconButton zoomOut_ { Icon::minus, "Zoom out" }, zoomIn_ { Icon::plus, "Zoom in" };
    Slider zoom_;
    juce::ScrollBar horizontal_ { false };
    TimelineRuler lowerRuler_;
    PianoViewport pianoScroll_;
    juce::Component pianoCanvas_;
    PianoKeyboard keyboard_;
    PitchEditor pitch_;
    Playhead lowerPlayhead_;
    juce::Label status_;
    Panel exportToast_;
    juce::Label exportLabel_;
    Button exportOpen_ { "Show folder" }, exportDismiss_ { "Dismiss" };
    juce::String exportDirectory_;
    Menu menu_;
    BackingPicker backingPicker_;
    std::vector<Note> notes_;
    std::map<juce::String, AudioClipData> preparedClips_;
    std::map<juce::String, int> noteIds_;
    std::set<juce::String> selectedNotes_, activatedBackingTracks_;
    int nextNoteId_ = 1;
    juce::String selectedTrack_, selectedClip_, selectedNote_, pendingTrack_;
    std::vector<juce::String> selectedTracks_;
    double duration_ = 0.0, playheadBeat_ = 0.0, playheadPercent_ = 0.0, instrumentalVolume_ = 100.0, snapBeats_ = 0.25;
    double verticalZoom_ = 1.0;
    bool pianoCollapsed_ = false, pianoViewportInitialised_ = false, selectionInitialised_ = false;
    juce::String generationTrack_, renderTrack_;
    bool fitted_ = false, snapToChord_ = false, playing_ = false, loopActive_ = false, busy_ = false;
};

NativeStudioView::NativeStudioView(CommandHandler handler, std::shared_ptr<StudioShortcuts> shortcuts) : content_(std::make_unique<Content>(*this, std::move(handler), std::move(shortcuts)))
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
