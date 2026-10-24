#include "Timeline.h"
#include "../uikit/Theme.h"
#include <cmath>
#include <limits>

namespace obsidian::editor
{
namespace
{
using namespace obsidian::ui;
constexpr float kLaneInset = 2.0f;
constexpr float kWaveformLabelHeight = 20.0f;

void laneBackground(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.fillAll(colour(tokens::surface));
    g.setColour(colour(tokens::border));
    g.drawHorizontalLine(area.getBottom() - 1, 0.0f, static_cast<float>(area.getWidth()));
}

void beatLines(juce::Path& minor, juce::Path& major, const TimeViewport& viewport,
               int width, int height)
{
    minor.clear();
    major.clear();
    if (width <= 0 || height <= 0) return;
    const double step = viewport.pixelsPerBeat >= 32.0 ? 0.25 : 1.0;
    const auto first = std::floor(viewport.firstBeat / step) * step;
    const auto last = viewport.beatAtX(static_cast<float>(width));
    for (auto beat = first; beat <= last + step; beat += step)
    {
        const auto x = std::floor(viewport.xForBeat(beat)) + 0.5f;
        auto& path = std::abs(std::remainder(beat, 4.0)) < 0.001 ? major : minor;
        path.startNewSubPath(x, 0.0f);
        path.lineTo(x, static_cast<float>(height));
    }
}
}

EditorSurface::EditorSurface(const TimeViewport& viewport) : viewport_(viewport), keyboardBeat_(viewport.firstBeat)
{
    setWantsKeyboardFocus(true);
    setRepaintsOnMouseActivity(true);
}
void EditorSurface::viewportChanged() { repaint(); }
void EditorSurface::setCurrentBeat(double beat)
{
    if (std::isfinite(beat)) keyboardBeat_ = std::max(0.0, beat);
}
void EditorSurface::seekAt(float x)
{
    setCurrentBeat(viewport_.beatAtX(x));
    const auto callback = onSeek;
    const auto beat = keyboardBeat_;
    if (callback) callback(beat);
}
void EditorSurface::mouseDown(const juce::MouseEvent& e)
{
    const auto position = e.position;
    const juce::Component::SafePointer<EditorSurface> safeThis(this);
    grabKeyboardFocus();
    if (safeThis != nullptr) safeThis->seekAt(position.x);
}
void EditorSurface::mouseDrag(const juce::MouseEvent& e) { seekAt(e.position.x); }
void EditorSurface::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto callback = onViewportGesture;
    if (!callback) return;
    const auto delta = wheel.deltaX != 0.0f ? wheel.deltaX : wheel.deltaY;
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        callback(0.0, std::exp(static_cast<double>(delta) * 1.7), e.position.x);
    else
        callback(-static_cast<double>(delta) * 160.0 / viewport_.pixelsPerBeat, 1.0, e.position.x);
}
bool EditorSurface::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
    {
        keyboardBeat_ = std::max(0.0, keyboardBeat_ + (key == juce::KeyPress::leftKey ? -1.0 : 1.0));
        const auto callback = onSeek;
        const auto beat = keyboardBeat_;
        if (callback) callback(beat);
        return true;
    }
    if (key == juce::KeyPress::homeKey) { seekAt(0.0f); return true; }
    return false;
}
void EditorSurface::focusGained(juce::Component::FocusChangeType) { repaint(); }
void EditorSurface::focusLost(juce::Component::FocusChangeType) { repaint(); }
std::unique_ptr<juce::AccessibilityHandler> EditorSurface::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group);
}

TimelineRuler::TimelineRuler(const TimeViewport& viewport) : EditorSurface(viewport) { setTitle("Timeline ruler. Click to seek; arrows move one beat."); }
void TimelineRuler::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    g.fillAll(colour(tokens::background));
    g.setFont(font(tokens::fontSmall));
    const auto end = viewport_.beatAtX(static_cast<float>(getWidth()));
    const auto step = viewport_.pixelsPerBeat >= 12.0 ? 0.5 : 1.0;
    for (auto beat = std::floor(viewport_.firstBeat / step) * step; beat <= end; beat += step)
    {
        const auto x = viewport_.xForBeat(beat);
        const auto bar = std::abs(std::remainder(beat, 4.0)) < 0.001;
        const auto label = bar && (viewport_.pixelsPerBeat >= 24.0 || std::abs(std::remainder(beat, 8.0)) < 0.001);
        g.setColour(colour(bar ? tokens::faint : tokens::border));
        g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(getHeight() - (bar ? 15 : 7)), static_cast<float>(getHeight()));
        if (label)
        {
            g.setColour(colour(tokens::muted));
            g.drawText(juce::String(static_cast<int>(std::floor(beat / 4.0)) + 1), juce::roundToInt(x) + 3, 1, 42, 15, juce::Justification::left);
        }
    }
    drawFocus(g, *this);
}
TimelineGrid::TimelineGrid(const TimeViewport& viewport) : EditorSurface(viewport) { setTitle("Timeline grid"); }
void TimelineGrid::setRowHeight(float height) { rowHeight_ = std::max(1.0f, height); viewportChanged(); }
void TimelineGrid::resized() { viewportChanged(); }
void TimelineGrid::viewportChanged()
{
    beatLines(minor_, major_, viewport_, getWidth(), getHeight());
    rows_.clear();
    for (auto y = rowHeight_; y < static_cast<float>(getHeight()); y += rowHeight_)
    { rows_.startNewSubPath(0.0f, y); rows_.lineTo(static_cast<float>(getWidth()), y); }
    repaint();
}
void TimelineGrid::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    g.fillAll(colour(tokens::background));
    g.setColour(colour(tokens::border).withAlpha(0.25f)); g.strokePath(minor_, juce::PathStrokeType(1.0f));
    g.setColour(colour(tokens::border).withAlpha(0.65f)); g.strokePath(major_, juce::PathStrokeType(1.0f));
    g.strokePath(rows_, juce::PathStrokeType(1.0f));
    drawFocus(g, *this);
}
Playhead::Playhead(const TimeViewport& viewport) : viewport_(viewport), beat_(viewport.firstBeat)
{
    setInterceptsMouseClicks(false, false);
    setAccessible(false);
}
void Playhead::setBeat(double beat)
{
    if (!std::isfinite(beat)) return;
    const auto old = juce::roundToInt(viewport_.xForBeat(beat_));
    beat_ = std::max(0.0, beat);
    repaint(old - 7, 0, 15, getHeight());
    repaint(juce::roundToInt(viewport_.xForBeat(beat_)) - 7, 0, 15, getHeight());
}
void Playhead::viewportChanged() { repaint(); }
void Playhead::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    const auto x = viewport_.xForBeat(beat_);
    if (x < -7.0f || x > static_cast<float>(getWidth()) + 7.0f) return;
    g.setColour(colour(tokens::text));
    g.drawLine(x, 6.0f, x, static_cast<float>(getHeight()), 1.3f);
    juce::Path marker;
    marker.addTriangle(x - 5.0f, 1.0f, x + 5.0f, 1.0f, x, 9.0f);
    g.fillPath(marker);
}
TempoLane::TempoLane(const TimeViewport& viewport) : EditorSurface(viewport) { setTitle("Tempo lane"); }
void TempoLane::setTempo(double tempo) { if (std::isfinite(tempo)) { tempo_ = std::clamp(tempo, 20.0, 400.0); setDescription(juce::String(tempo_, 1) + " BPM"); repaint(); } }
void TempoLane::setPointBeats(std::vector<double> points) { pointBeats_ = std::move(points); repaint(); }
void TempoLane::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    laneBackground(g, getLocalBounds());
    const auto y = static_cast<float>(getHeight()) * 0.5f;
    g.setColour(colour(tokens::olive));
    g.drawLine(0.0f, y, static_cast<float>(getWidth()), y, 1.1f);
    for (auto beat : pointBeats_)
        if (std::isfinite(beat) && viewport_.xForBeat(beat) >= 0.0f && viewport_.xForBeat(beat) <= static_cast<float>(getWidth()))
            g.fillEllipse(viewport_.xForBeat(beat) - 2.5f, y - 2.5f, 5.0f, 5.0f);
    drawFocus(g, *this);
}
SignatureLane::SignatureLane(const TimeViewport& viewport) : EditorSurface(viewport) { setTitle("Time signature lane"); }
void SignatureLane::setSignature(int numerator, int denominator)
{
    numerator_ = std::clamp(numerator, 1, 32);
    denominator_ = (denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16 || denominator == 32) ? denominator : 4;
    setDescription(juce::String(numerator_) + "/" + juce::String(denominator_)); repaint();
}
void SignatureLane::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    laneBackground(g, getLocalBounds());
    g.setColour(colour(tokens::muted).withAlpha(0.6f)); g.setFont(font(tokens::fontSmall));
    // A signature event is shown only at project start, like an editor event lane.
    const auto x = viewport_.xForBeat(0.0);
    if (x > -35.0f && x < static_cast<float>(getWidth()))
        g.drawText(juce::String(numerator_) + "/" + juce::String(denominator_), juce::roundToInt(x) + 4, 0, 36, getHeight(), juce::Justification::centredLeft);
    drawFocus(g, *this);
}
ChordLane::ChordLane(const TimeViewport& viewport) : EditorSurface(viewport) { setTitle("Chord sequence"); }
void ChordLane::setChords(std::vector<ChordBlock> chords) { chords_ = std::move(chords); repaint(); }
void ChordLane::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    laneBackground(g, getLocalBounds());
    g.setFont(font(tokens::fontSmall));
    for (const auto& chord : chords_)
    {
        if (!std::isfinite(chord.startBeat) || !std::isfinite(chord.lengthBeats) || chord.lengthBeats <= 0.0) continue;
        const auto x = viewport_.xForBeat(chord.startBeat);
        const auto width = static_cast<float>(chord.lengthBeats * viewport_.pixelsPerBeat);
        auto bounds = juce::Rectangle<float>(x + 1.0f, kLaneInset, std::max(1.0f, width - 2.0f), static_cast<float>(getHeight()) - 2.0f * kLaneInset);
        if (!bounds.intersects(g.getClipBounds().toFloat())) continue;
        g.setColour(colour(tokens::pink).withAlpha(0.16f)); g.fillRoundedRectangle(bounds, 3.0f);
        g.setColour(colour(tokens::pink).withAlpha(0.23f)); g.drawRoundedRectangle(bounds, 3.0f, 1.0f);
        g.setColour(colour(tokens::text)); g.drawText(chord.label, bounds.toNearestInt().reduced(3, 0), juce::Justification::centred, true);
    }
    drawFocus(g, *this);
}
AudioClip::AudioClip(const TimeViewport& viewport) : EditorSurface(viewport) { setTitle("Audio clip"); }
void AudioClip::setData(AudioClipData data)
{
    if (!std::isfinite(data.startBeat)) data.startBeat = 0.0;
    if (!std::isfinite(data.lengthBeats) || data.lengthBeats <= 0.0) data.lengthBeats = 0.0;
    data_ = std::move(data);
    setTitle(data_.name); setDescription("Audio clip. Enter selects.");
    rebuildWaveform(); repaint();
}
void AudioClip::setSelected(bool selected) { if (data_.selected != selected) { data_.selected = selected; repaint(); } }
juce::Rectangle<float> AudioClip::clipBounds() const noexcept
{
    return { viewport_.xForBeat(data_.startBeat), kLaneInset,
             static_cast<float>(data_.lengthBeats * viewport_.pixelsPerBeat), std::max(0.0f, static_cast<float>(getHeight()) - 2.0f * kLaneInset) };
}
void AudioClip::resized() { rebuildWaveform(); }
void AudioClip::viewportChanged() { rebuildWaveform(); repaint(); }
void AudioClip::rebuildWaveform()
{
    waveform_.clear(); ++geometryRevision_;
    const auto bounds = clipBounds();
    if (data_.peaks.empty() || bounds.isEmpty()) return;
    const auto visible = bounds.getIntersection(getLocalBounds().toFloat());
    if (visible.isEmpty()) return;
    const auto centre = bounds.getCentreY() + 3.0f;
    const auto amplitude = std::max(0.0f, bounds.getHeight() - 12.0f) * 0.45f;
    const auto count = data_.peaks.size();
    const auto samplesPerPixel = static_cast<double>(count) / bounds.getWidth();
    const auto left = std::max(0, static_cast<int>(std::floor(visible.getX())));
    const auto right = std::min(getWidth(), static_cast<int>(std::ceil(visible.getRight())));
    waveform_.startNewSubPath(static_cast<float>(left), centre);
    // Reduce precomputed peaks into one visible vertical envelope per logical pixel.
    std::vector<float> visiblePeaks;
    visiblePeaks.reserve(static_cast<std::size_t>(std::max(0, right - left)));
    for (auto x = left; x < right; ++x)
    {
        const auto from = std::clamp(static_cast<std::size_t>(std::max(0.0, (static_cast<double>(x) - bounds.getX()) * samplesPerPixel)), std::size_t{0}, count - 1);
        const auto to = std::clamp(static_cast<std::size_t>(std::max(0.0, (x + 1.0 - bounds.getX()) * samplesPerPixel)) + 1, from + 1, count);
        float peak = 0.0f;
        for (auto i = from; i < to; ++i) if (std::isfinite(data_.peaks[i])) peak = std::max(peak, std::clamp(std::abs(data_.peaks[i]), 0.0f, 1.0f));
        visiblePeaks.push_back(peak);
        waveform_.lineTo(static_cast<float>(x), centre - peak * amplitude);
    }
    for (auto x = right - 1; x >= left; --x)
        waveform_.lineTo(static_cast<float>(x), centre + visiblePeaks[static_cast<std::size_t>(x - left)] * amplitude);
    waveform_.closeSubPath();
}
void AudioClip::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    const auto bounds = clipBounds();
    if (!bounds.intersects(g.getClipBounds().toFloat()) || bounds.isEmpty()) return;
    g.setColour(data_.colour.withAlpha(data_.selected ? 0.3f : isMouseOver() ? 0.24f : 0.19f)); g.fillRoundedRectangle(bounds, tokens::radius);
    g.setColour(data_.colour.withAlpha(data_.selected ? 0.95f : 0.42f)); g.drawRoundedRectangle(bounds.reduced(0.5f), tokens::radius, data_.selected ? 1.5f : 1.0f);
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(bounds.toNearestInt());
    g.setColour(data_.colour.withAlpha(0.78f)); g.fillPath(waveform_);
    g.setColour(colour(tokens::text)); g.setFont(font(tokens::fontSmall));
    auto label = bounds.withHeight(kWaveformLabelHeight).getIntersection(getLocalBounds().toFloat()).reduced(7.0f, 0.0f);
    g.drawText(data_.name, label, juce::Justification::centredLeft, true);
    if (hasKeyboardFocus(true))
    {
        g.setColour(colour(tokens::focus));
        g.drawRoundedRectangle(bounds.reduced(1.5f), tokens::radius, 1.0f);
    }
}
void AudioClip::mouseDown(const juce::MouseEvent& e)
{
    const auto position = e.position;
    const juce::Component::SafePointer<AudioClip> safeThis(this);
    grabKeyboardFocus();
    if (safeThis == nullptr) return;
    if (clipBounds().contains(position))
    {
        const auto callback = onSelect;
        if (callback) callback();
    }
    else seekAt(position.x);
}
void AudioClip::mouseDrag(const juce::MouseEvent&) {}
bool AudioClip::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey)
    {
        const auto callback = onSelect;
        if (callback) callback();
        return true;
    }
    return EditorSurface::keyPressed(key);
}
}
