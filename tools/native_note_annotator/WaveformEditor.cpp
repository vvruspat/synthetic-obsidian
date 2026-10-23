#include "WaveformEditor.h"

#include <algorithm>
#include <cmath>

namespace native_annotation
{
namespace UIConstants
{
constexpr float kMargin = 22.0f;
constexpr float kHandleWidth = 7.0f;
constexpr float kTimelineHeight = 34.0f;
constexpr float kFooterHeight = 46.0f;
constexpr int kPeakSamples = 128;
}

WaveformEditor::WaveformEditor(vocal_annotation::AnnotationDocument& document, const NoteAnalysis& analysis)
    : document_(document), analysis_(analysis)
{
    setWantsKeyboardFocus(true);
}

void WaveformEditor::setAudio(const juce::AudioBuffer<float>& audio)
{
    peaks_.clear();
    for (int offset = 0; offset < audio.getNumSamples(); offset += UIConstants::kPeakSamples)
    {
        Peak peak;
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int i = offset; i < juce::jmin(audio.getNumSamples(), offset + UIConstants::kPeakSamples); ++i)
            {
                const auto value = audio.getSample(channel, i);
                peak.low = juce::jmin(peak.low, value);
                peak.high = juce::jmax(peak.high, value);
            }
        peaks_.push_back(peak);
    }
    clearSelection();
    fit();
}

void WaveformEditor::clearSelection()
{
    selected_.clear();
    dragging_.reset();
    repaint();
}

void WaveformEditor::fit()
{
    start_ = 0.0;
    end_ = juce::jmax(0.1, document_.duration);
    repaint();
}

void WaveformEditor::zoom(double factor, double pivot)
{
    if (document_.duration <= 0.0)
        return;
    if (pivot < 0.0)
        pivot = (start_ + end_) * 0.5;
    const auto width = end_ - start_;
    const auto newWidth = juce::jlimit(juce::jmin(0.15, document_.duration), document_.duration, width / factor);
    const auto ratio = juce::jlimit(0.0, 1.0, (pivot - start_) / width);
    start_ = juce::jlimit(0.0, document_.duration - newWidth, pivot - ratio * newWidth);
    end_ = start_ + newWidth;
    repaint();
}

void WaveformEditor::pan(double delta)
{
    const auto width = end_ - start_;
    start_ = juce::jlimit(0.0, juce::jmax(0.0, document_.duration - width), start_ + delta);
    end_ = start_ + width;
    repaint();
}

void WaveformEditor::setPlayhead(double time)
{
    if (std::abs(playhead_ - time) > 0.001)
    {
        playhead_ = time;
        repaint();
    }
}

juce::Rectangle<float> WaveformEditor::waveBounds() const
{
    return getLocalBounds().toFloat().reduced(UIConstants::kMargin)
        .withTrimmedTop(UIConstants::kTimelineHeight).withTrimmedBottom(UIConstants::kFooterHeight);
}

float WaveformEditor::xAt(double time) const
{
    const auto bounds = waveBounds();
    return bounds.getX() + static_cast<float>((time - start_) / (end_ - start_)) * bounds.getWidth();
}

double WaveformEditor::timeAt(float x) const
{
    const auto bounds = waveBounds();
    return juce::jlimit(0.0, document_.duration, start_ + (x - bounds.getX()) / bounds.getWidth() * (end_ - start_));
}

std::optional<size_t> WaveformEditor::noteAt(double time) const
{
    for (size_t i = 0; i < document_.notes.size(); ++i)
        if (time >= document_.notes[i].start && time <= document_.notes[i].end)
            return i;
    return std::nullopt;
}

void WaveformEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff111820));
    if (document_.duration <= 0.0)
    {
        g.setColour(juce::Colour(0xffbac8d5));
        g.setFont(juce::FontOptions(22.0f));
        g.drawText("Open or drop a vocal WAV, AIFF or FLAC", getLocalBounds(), juce::Justification::centred);
        return;
    }
    const auto bounds = waveBounds();
    g.setColour(juce::Colour(0xff1b2632));
    g.fillRoundedRectangle(bounds, 8.0f);
    g.setFont(juce::FontOptions(12.0f));
    const auto rawStep = (end_ - start_) / juce::jmax(1.0f, bounds.getWidth() / 100.0f);
    const auto base = std::pow(10.0, std::floor(std::log10(rawStep)));
    const auto scaled = rawStep / base;
    const auto step = base * (scaled <= 1.0 ? 1.0 : scaled <= 2.0 ? 2.0 : scaled <= 5.0 ? 5.0 : 10.0);
    for (auto time = std::ceil(start_ / step) * step; time <= end_; time += step)
    {
        const auto x = xAt(time);
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawVerticalLine(static_cast<int>(x), bounds.getY(), bounds.getBottom());
        g.setColour(juce::Colour(0xffa8b7c7));
        g.drawText(juce::String(time, step < 1.0 ? 2 : 1) + "s", static_cast<int>(x), 24, 75, 22, juce::Justification::left);
    }
    {
        const juce::Graphics::ScopedSaveState scope(g);
        g.reduceClipRegion(bounds.toNearestInt());
        for (size_t i = 0; i < document_.notes.size(); ++i)
        {
            const auto& note = document_.notes[i];
            if (note.end < start_ || note.start > end_)
                continue;
            const auto colour = juce::Colour::fromHSV(static_cast<float>(note.pitch % 12) / 12.0f, 0.48f, 0.92f, 1.0f);
            const auto left = xAt(note.start);
            const auto right = xAt(note.end);
            const juce::Rectangle<float> region(left, bounds.getY(), right - left, bounds.getHeight());
            g.setColour(colour.withAlpha(note.id == selected_ ? 0.27f : 0.13f));
            g.fillRect(region);
            g.setColour(colour.withAlpha(0.85f));
            g.fillRect(left, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect(right - 2.0f, bounds.getY(), 2.0f, bounds.getHeight());
            for (const auto edge : { left, right - UIConstants::kHandleWidth })
                g.fillRoundedRectangle(edge, bounds.getCentreY() - 22.0f, UIConstants::kHandleWidth, 44.0f, 3.0f);
            if (right - left > 28.0f)
            {
                g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
                auto label = juce::MidiMessage::getMidiNoteName(note.pitch, true, true, 4);
                if (note.flags.contains("pitch_uncertain"))
                    label += " ?";
                g.drawText(label, static_cast<int>(left + 7), static_cast<int>(bounds.getY() + 9),
                           static_cast<int>(right - left - 14), 23, juce::Justification::left);
            }
        }
        g.setColour(juce::Colour(0xffd8e5ec));
        const auto centre = bounds.getCentreY();
        const auto scale = bounds.getHeight() * 0.36f;
        for (int x = static_cast<int>(bounds.getX()); x < static_cast<int>(bounds.getRight()); ++x)
        {
            const auto begin = static_cast<size_t>(timeAt(static_cast<float>(x)) * document_.sampleRate / UIConstants::kPeakSamples);
            const auto end = static_cast<size_t>(timeAt(static_cast<float>(x + 1)) * document_.sampleRate / UIConstants::kPeakSamples);
            Peak peak;
            for (size_t index = begin; index <= end && index < peaks_.size(); ++index)
            {
                peak.low = juce::jmin(peak.low, peaks_[index].low);
                peak.high = juce::jmax(peak.high, peaks_[index].high);
            }
            g.drawVerticalLine(x, centre - peak.high * scale, centre - peak.low * scale + 1.0f);
        }
        g.setColour(juce::Colour(0xfff8d477));
        g.drawVerticalLine(static_cast<int>(xAt(playhead_)), bounds.getY(), bounds.getBottom());
    }
    g.setColour(juce::Colour(0xffa8b7c7));
    g.setFont(juce::FontOptions(13.0f));
    juce::String info = juce::String(static_cast<int>(document_.notes.size())) + " notes  |  "
                      + juce::String(document_.duration, 2) + " seconds";
    if (const auto index = document_.findNoteIndex(selected_))
    {
        const auto& note = document_.notes[static_cast<size_t>(*index)];
        info = juce::MidiMessage::getMidiNoteName(note.pitch, true, true, 4) + "  |  "
             + juce::String(note.start, 3) + " - " + juce::String(note.end, 3) + " s  |  Attack begins at left edge";
    }
    g.drawText(info, getLocalBounds().removeFromBottom(48).reduced(22, 0), juce::Justification::left);
}

void WaveformEditor::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();
    dragging_.reset();
    if (document_.duration <= 0.0 || ! waveBounds().withTrimmedTop(-UIConstants::kTimelineHeight).contains(event.position))
        return;
    const auto time = timeAt(event.position.x);
    if (event.mods.isCommandDown() && waveBounds().contains(event.position))
    {
        if (beginEdit) beginEdit();
        if (NoteEditing::insert(document_, analysis_, time))
        {
            if (const auto index = noteAt(time + 0.001)) selected_ = document_.notes[*index].id;
            if (changed) changed();
        }
        repaint();
        return;
    }
    if (! waveBounds().contains(event.position))
    {
        if (seek) seek(time);
        return;
    }
    // Edge hit testing is independent of interval containment, including shared edges.
    for (size_t i = 0; i < document_.notes.size(); ++i)
    {
        const auto& note = document_.notes[i];
        const auto leftDistance = std::abs(xAt(note.start) - event.position.x);
        const auto rightDistance = std::abs(xAt(note.end) - event.position.x);
        if (juce::jmin(leftDistance, rightDistance) <= UIConstants::kHandleWidth)
        {
            selected_ = note.id;
            dragging_ = i;
            startEdge_ = leftDistance < rightDistance;
            original_ = note;
            neighborOriginal_.reset();
            if ((startEdge_ && i > 0) || (! startEdge_ && i + 1 < document_.notes.size()))
            {
                neighborIndex_ = startEdge_ ? i - 1 : i + 1;
                neighborOriginal_ = document_.notes[neighborIndex_];
            }
            if (beginEdit) beginEdit();
            repaint();
            return;
        }
    }
    if (const auto index = noteAt(time))
    {
        const auto& note = document_.notes[*index];
        selected_ = note.id;
        if (audition) audition(note.start, note.end);
    }
    else
    {
        selected_.clear();
        if (seek) seek(time);
    }
    repaint();
}

void WaveformEditor::mouseDrag(const juce::MouseEvent& event)
{
    if (! dragging_) return;
    document_.notes[*dragging_] = original_;
    if (neighborOriginal_) document_.notes[neighborIndex_] = *neighborOriginal_;
    if (NoteEditing::resize(document_, *dragging_, startEdge_, timeAt(event.position.x)))
        if (changed) changed();
    repaint();
}

void WaveformEditor::mouseUp(const juce::MouseEvent&)
{
    dragging_.reset();
}

void WaveformEditor::mouseMove(const juce::MouseEvent& event)
{
    if (event.mods.isCommandDown())
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    else
    {
        bool edge = false;
        for (const auto& note : document_.notes)
            edge = edge || std::abs(xAt(note.start) - event.position.x) < UIConstants::kHandleWidth
                        || std::abs(xAt(note.end) - event.position.x) < UIConstants::kHandleWidth;
        setMouseCursor(edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
}

void WaveformEditor::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCommandDown())
        zoom(std::exp(wheel.deltaY * 2.0), timeAt(event.position.x));
    else
        pan(-(std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY) * (end_ - start_) * 0.35);
}

void WaveformEditor::mouseMagnify(const juce::MouseEvent& event, float factor)
{
    zoom(factor, timeAt(event.position.x));
}

bool WaveformEditor::keyPressed(const juce::KeyPress& key)
{
    if (shortcut && shortcut(key)) return true;
    const auto index = document_.findNoteIndex(selected_);
    if (! index) return false;
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        if (beginEdit) beginEdit();
        document_.notes.erase(document_.notes.begin() + *index);
        selected_.clear();
    }
    else if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
    {
        if (beginEdit) beginEdit();
        auto& note = document_.notes[static_cast<size_t>(*index)];
        const auto pitch = juce::jlimit(0, 127, note.pitch + (key == juce::KeyPress::upKey ? 1 : -1));
        const auto delta = pitch - note.pitch;
        note.pitch = pitch;
        note.pitchExact = juce::jlimit(0.0, 127.0, note.pitchExact + delta);
        for (auto& point : note.curve) point.midi = juce::jlimit(0.0, 127.0, point.midi + delta);
        note.flags.removeString("pitch_uncertain");
        note.flags.addIfNotAlreadyThere("manual_edit");
    }
    else if (key.getTextCharacter() == 'm' || key.getTextCharacter() == 'M')
    {
        if (static_cast<size_t>(*index + 1) >= document_.notes.size()) return true;
        if (beginEdit) beginEdit();
        auto& note = document_.notes[static_cast<size_t>(*index)];
        const auto& next = document_.notes[static_cast<size_t>(*index + 1)];
        note.end = next.end;
        note.voicedEnd = next.voicedEnd;
        note.curve.insert(note.curve.end(), next.curve.begin(), next.curve.end());
        NoteEditing::trim(note);
        document_.notes.erase(document_.notes.begin() + *index + 1);
    }
    else return false;
    if (changed) changed();
    repaint();
    return true;
}
}
