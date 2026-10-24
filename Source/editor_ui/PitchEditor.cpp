#include "PitchEditor.h"
#include "../uikit/Theme.h"
#include <cmath>

namespace obsidian::editor
{
namespace
{
bool isBlackKey(int midiNote)
{
    const auto pitchClass = midiNote % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
}
juce::Rectangle<float> noteRectangle(const NoteSegment& note, const TimeViewport& viewport, int highest, float row)
{
    // Keep the pitch centre fixed while making short rows legible and selectable.
    const auto height = std::min(std::clamp(row * 1.75f, 10.0f, 18.0f), std::max(1.0f, row * 2.0f));
    const auto centreY = (static_cast<float>(highest - note.midiNote) + 0.5f) * row;
    return { viewport.xForBeat(note.startBeat), centreY - height * 0.5f,
             std::max(0.0f, static_cast<float>(note.lengthBeats * viewport.pixelsPerBeat)), height };
}
void constrainRange(int& low, int& high)
{
    low = std::clamp(low, 0, 126);
    high = std::clamp(high, low + 1, 127);
}
}

PianoKeyboard::PianoKeyboard()
{
    setTitle("Piano keyboard");
    setDescription("Vertical MIDI note reference. Middle C is C4.");
    setInterceptsMouseClicks(false, false);
}
void PianoKeyboard::setNoteRange(int low, int high)
{
    constrainRange(low, high); lowest_ = low; highest_ = high; repaint();
}
void PianoKeyboard::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    g.fillAll(juce::Colour(0xffc7cbd0));
    const auto row = static_cast<float>(getHeight()) / static_cast<float>(highest_ - lowest_ + 1);
    const auto width = static_cast<float>(getWidth());
    g.setFont(font(10.0f));
    for (auto note = highest_; note >= lowest_; --note)
    {
        const auto y = static_cast<float>(highest_ - note) * row;
        const juce::Rectangle<float> bounds(0.0f, y, width, row);
        if (isBlackKey(note))
        {
            g.setColour(juce::Colour(0xff1e2529));
            g.fillRoundedRectangle(bounds.withWidth(width * 0.59f).reduced(0.0f, std::min(1.0f, row * 0.12f)), 1.2f);
        }
        else
        {
            g.setColour(juce::Colour(0xffa3a8b0));
            g.drawHorizontalLine(juce::roundToInt(y + row), 0.0f, width);
        }
        if (note % 12 == 0)
        {
            g.setColour(juce::Colour(0xff252b30));
            g.drawText("C" + juce::String(note / 12 - 1), bounds.reduced(4.0f, 0.0f), juce::Justification::centredRight);
        }
    }
    g.setColour(colour(tokens::border));
    g.drawVerticalLine(getWidth() - 1, 0.0f, static_cast<float>(getHeight()));
}

PitchEditor::PitchEditor(const TimeViewport& viewport) : EditorSurface(viewport)
{
    setTitle("Pitch editor");
    setDescription("Select notes with the pointer or use Up and Down. Escape clears selection. Wheel scrolls time; Control or Command with wheel zooms.");
}
void PitchEditor::setNotes(std::vector<NoteSegment> notes)
{
    drag_ = {}; dragNotes_.clear();
    notes_ = std::move(notes);
    if (std::none_of(notes_.begin(), notes_.end(), [this](const auto& note) { return note.id == selectedNote_; })) selectedNote_ = -1;
    std::erase_if(selectedNotes_, [this](int id) { return std::none_of(notes_.begin(), notes_.end(), [id](const auto& n) { return n.id == id; }); });
    rebuildGeometry(); repaint();
}
void PitchEditor::setNoteRange(int low, int high)
{
    constrainRange(low, high); lowest_ = low; highest_ = high; rebuildGeometry(); repaint();
}
void PitchEditor::setSelectedNotes(std::vector<int> ids)
{
    selectedNotes_ = std::move(ids);
    selectedNote_ = selectedNotes_.empty() ? -1 : selectedNotes_.back();
    setDescription(selectedNotes_.empty() ? "Pitch editor. No note selected." : "Pitch editor. " + juce::String(static_cast<int>(selectedNotes_.size())) + " note(s) selected.");
    repaint();
    if (auto* handler = getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}
void PitchEditor::setSelectedNote(int id)
{
    selectedNotes_ = id < 0 ? std::vector<int>{} : std::vector<int>{ id };
    if (selectedNote_ != id)
    {
        selectedNote_ = id;
        setDescription(id < 0 ? "Pitch editor. No note selected." : "Pitch editor. Note " + juce::String(id) + " selected.");
        repaint();
        if (auto* handler = getAccessibilityHandler()) handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    }
}
void PitchEditor::setTool(PitchTool tool)
{
    tool_ = tool;
    setMouseCursor(tool == PitchTool::pointer ? juce::MouseCursor::NormalCursor : juce::MouseCursor::CrosshairCursor);
}
void PitchEditor::setSnapBeats(double beats)
{
    if (std::isfinite(beats) && beats >= 0.0) snapBeats_ = beats;
}
void PitchEditor::resized() { rebuildGeometry(); }
void PitchEditor::viewportChanged() { rebuildGeometry(); repaint(); }
int PitchEditor::midiNoteAt(float y) const noexcept
{
    if (getHeight() <= 0) return lowest_;
    const auto row = static_cast<float>(getHeight()) / static_cast<float>(highest_ - lowest_ + 1);
    return std::clamp(highest_ - static_cast<int>(std::floor(y / row)), lowest_, highest_);
}
juce::Rectangle<float> PitchEditor::noteBounds(int id) const noexcept
{
    const auto it = std::find_if(notes_.begin(), notes_.end(), [id](const auto& note) { return note.id == id; });
    if (it == notes_.end() || !std::isfinite(it->startBeat) || !std::isfinite(it->lengthBeats)) return {};
    const auto row = static_cast<float>(getHeight()) / static_cast<float>(highest_ - lowest_ + 1);
    return noteRectangle(*it, viewport_, highest_, row);
}
int PitchEditor::hitTestNote(juce::Point<float> point) const noexcept
{
    for (auto it = visibleNotes_.rbegin(); it != visibleNotes_.rend(); ++it)
        if (it->bounds.contains(point)) return it->id;
    return -1;
}
void PitchEditor::rebuildGeometry()
{
    ++geometryRevision_;
    minorGrid_.clear(); majorGrid_.clear(); rowGrid_.clear(); visibleNotes_.clear();
    if (getWidth() <= 0 || getHeight() <= 0) return;
    const auto row = static_cast<float>(getHeight()) / static_cast<float>(highest_ - lowest_ + 1);
    for (auto i = 1; i <= highest_ - lowest_; ++i)
    { const auto y = static_cast<float>(i) * row; rowGrid_.startNewSubPath(0.0f, y); rowGrid_.lineTo(static_cast<float>(getWidth()), y); }
    const auto step = viewport_.pixelsPerBeat >= 32.0 ? 0.25 : 1.0;
    const auto end = viewport_.beatAtX(static_cast<float>(getWidth()));
    viewport_.forEachGridLine(viewport_.firstBeat, end, step, [&](double beat, bool bar)
    {
        auto& path = bar ? majorGrid_ : minorGrid_;
        const auto x = std::floor(viewport_.xForBeat(beat)) + 0.5f;
        path.startNewSubPath(x, 0.0f); path.lineTo(x, static_cast<float>(getHeight()));
    });
    visibleNotes_.reserve(notes_.size());
    const auto screen = getLocalBounds().toFloat();
    for (const auto& note : notes_)
    {
        if (!std::isfinite(note.startBeat) || !std::isfinite(note.lengthBeats) || note.lengthBeats <= 0.0 || note.midiNote < lowest_ || note.midiNote > highest_) continue;
        const auto bounds = noteRectangle(note, viewport_, highest_, row);
        if (!bounds.intersects(screen)) continue;
        CachedNote cached { note.id, bounds, {}, note.colour, note.ghost };
        if (!note.pitchOffsets.empty())
        {
            const auto count = note.pitchOffsets.size();
            const auto visible = bounds.getIntersection(screen);
            const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor((visible.getX() - bounds.getX()) / bounds.getWidth() * static_cast<float>(count - 1))));
            const auto last = std::min(count - 1, static_cast<std::size_t>(std::ceil((visible.getRight() - bounds.getX()) / bounds.getWidth() * static_cast<float>(count - 1))));
            const auto stride = std::max(std::size_t{1}, (last - first) / static_cast<std::size_t>(std::max(1.0f, visible.getWidth() * 2.0f)));
            bool started = false;
            for (auto i = first; i <= last; i += stride)
            {
                const auto x = bounds.getX() + (count == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(count - 1) * bounds.getWidth());
                const auto offset = std::isfinite(note.pitchOffsets[i]) ? std::clamp(note.pitchOffsets[i], -2.0f, 2.0f) : 0.0f;
                const auto y = bounds.getCentreY() - offset * row;
                if (started) cached.contour.lineTo(x, y); else { cached.contour.startNewSubPath(x, y); started = true; }
                if (last - i < stride) break;
            }
        }
        visibleNotes_.push_back(std::move(cached));
    }
}
void PitchEditor::paint(juce::Graphics& g)
{
    using namespace obsidian::ui;
    g.fillAll(colour(tokens::background));
    const auto row = static_cast<float>(getHeight()) / static_cast<float>(highest_ - lowest_ + 1);
    for (auto note = highest_; note >= lowest_; --note)
    {
        if (isBlackKey(note))
        {
            g.setColour(colour(tokens::surface).withAlpha(0.65f));
            g.fillRect(0.0f, static_cast<float>(highest_ - note) * row, static_cast<float>(getWidth()), row);
        }
    }
    g.setColour(colour(tokens::border).withAlpha(0.33f));
    g.strokePath(minorGrid_, juce::PathStrokeType(1.0f)); g.strokePath(rowGrid_, juce::PathStrokeType(0.6f));
    g.setColour(colour(tokens::faint).withAlpha(0.32f)); g.strokePath(majorGrid_, juce::PathStrokeType(1.0f));
    const auto clip = g.getClipBounds().toFloat();
    for (const auto& note : visibleNotes_)
    {
        if (!note.bounds.expanded(0.0f, row * 2.0f).intersects(clip)) continue;
        const auto colourValue = note.ghost ? colour(tokens::muted) : note.colour;
        const auto selected = std::find(selectedNotes_.begin(), selectedNotes_.end(), note.id) != selectedNotes_.end();
        g.setColour(colourValue.darker(0.3f).withAlpha(note.ghost ? 0.23f : selected ? 0.72f : 0.6f));
        g.fillRoundedRectangle(note.bounds, 1.5f);
        g.setColour(colourValue.withAlpha(note.ghost ? 0.4f : 0.75f));
        g.drawRoundedRectangle(note.bounds.reduced(0.25f), 1.5f, 0.6f);
        g.setColour(colourValue.brighter(0.25f).withAlpha(note.ghost ? 0.52f : 0.98f));
        g.strokePath(note.contour, juce::PathStrokeType(note.ghost ? 0.8f : 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (selected)
        {
            g.setColour(colour(tokens::text));
            g.drawRoundedRectangle(note.bounds.expanded(1.0f), 2.0f, 1.5f);
        }
    }
    drawFocus(g, *this);
}
void PitchEditor::selectFromUser(int id, bool extend)
{
    const auto callback = onNoteSelected;
    const auto multiCallback = onNotesSelected;
    const juce::Component::SafePointer<PitchEditor> safeThis(this);
    auto ids = extend ? selectedNotes_ : std::vector<int>{};
    const auto found = std::find(ids.begin(), ids.end(), id);
    if (extend && found != ids.end()) ids.erase(found);
    else if (id >= 0) ids.push_back(id);
    setSelectedNotes(std::move(ids));
    const auto selectedIds = selectedNotes_;
    const auto primary = selectedNote_;
    if (callback) callback(primary);
    if (safeThis != nullptr && multiCallback) multiCallback(selectedIds);
}
void PitchEditor::mouseDown(const juce::MouseEvent& e)
{
    const auto position = e.position;
    const juce::Component::SafePointer<PitchEditor> safeThis(this);
    grabKeyboardFocus();
    if (safeThis == nullptr) return;
    const auto id = hitTestNote(position);
    if (tool_ == PitchTool::pointer)
    {
        const auto alreadySelected = std::find(selectedNotes_.begin(), selectedNotes_.end(), id) != selectedNotes_.end();
        if (e.mods.isShiftDown() || !alreadySelected) selectFromUser(id, e.mods.isShiftDown());
        if (safeThis == nullptr) return;
        if (id < 0) { seekAt(position.x); return; }
        const auto note = std::find_if(notes_.begin(), notes_.end(), [id](const auto& value) { return value.id == id; });
        if (note != notes_.end())
        {
            const auto bounds = noteBounds(id);
            const auto edge = position.x < bounds.getX() + 5.0f ? -1 : position.x > bounds.getRight() - 5.0f ? 1 : 0;
            drag_ = { id, edge, note->startBeat, note->lengthBeats, note->midiNote, position, false };
            dragNotes_.clear();
            for (const auto& value : notes_)
                if (value.id == id || (edge == 0 && std::find(selectedNotes_.begin(), selectedNotes_.end(), value.id) != selectedNotes_.end()))
                    dragNotes_.push_back({ value.id, value.startBeat, value.lengthBeats, value.midiNote });
        }
    }
    else
    {
        const auto callback = onEditRequested;
        const auto rawBeat = viewport_.beatAtX(position.x);
        const auto beat = std::max(0.0, snapBeats_ > 0.0 ? std::round(rawBeat / snapBeats_) * snapBeats_ : rawBeat);
        const auto midiNote = midiNoteAt(position.y);
        const auto tool = tool_;
        if (callback) callback(tool, id, beat, midiNote);
    }
}
void PitchEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_.id < 0 || !isEnabled()) return;
    auto note = std::find_if(notes_.begin(), notes_.end(), [this](const auto& value) { return value.id == drag_.id; });
    if (note == notes_.end()) { drag_ = {}; return; }
    const auto delta = e.position - drag_.origin;
    if (!drag_.moved && delta.getDistanceFromOrigin() < 3.0f) return;
    drag_.moved = true;
    const auto step = snapBeats_ > 0.0 ? snapBeats_ : 0.0;
    const auto snap = [step](double beat) { return step > 0.0 ? std::round(beat / step) * step : beat; };
    const auto shift = static_cast<double>(delta.x) / viewport_.pixelsPerBeat;
    const auto minimum = step > 0.0 ? step : 0.01;
    if (drag_.edge < 0)
    {
        note->startBeat = std::clamp(snap(drag_.start + shift), 0.0, drag_.start + drag_.length - std::min(minimum, drag_.length));
        note->lengthBeats = drag_.start + drag_.length - note->startBeat;
    }
    else if (drag_.edge > 0)
        note->lengthBeats = std::max(minimum, snap(drag_.start + drag_.length + shift) - drag_.start);
    else
    {
        note->startBeat = std::max(0.0, snap(drag_.start + shift));
        const auto row = static_cast<float>(getHeight()) / static_cast<float>(highest_ - lowest_ + 1);
        note->midiNote = std::clamp(drag_.midi - juce::roundToInt(delta.y / std::max(1.0f, row)), lowest_, highest_);
    }
    if (drag_.edge == 0)
    {
        auto timeDelta = note->startBeat - drag_.start;
        auto pitchDelta = note->midiNote - drag_.midi;
        for (const auto& original : dragNotes_)
        { timeDelta = std::max(timeDelta, -original.startBeat); pitchDelta = std::clamp(pitchDelta, lowest_ - original.midiNote, highest_ - original.midiNote); }
        for (const auto& original : dragNotes_)
            if (auto moved = std::find_if(notes_.begin(), notes_.end(), [&original](const auto& value) { return value.id == original.id; }); moved != notes_.end())
            { moved->startBeat = original.startBeat + timeDelta; moved->midiNote = original.midiNote + pitchDelta; }
    }
    rebuildGeometry(); repaint();
}
void PitchEditor::mouseUp(const juce::MouseEvent&)
{
    const auto gesture = drag_;
    drag_ = {};
    const auto originals = std::move(dragNotes_);
    dragNotes_.clear();
    if (gesture.id < 0 || !gesture.moved) return;
    std::vector<NoteMovement> results;
    for (const auto& original : originals)
        if (auto note = std::find_if(notes_.begin(), notes_.end(), [&original](const auto& value) { return value.id == original.id; }); note != notes_.end())
            results.push_back({ note->id, note->startBeat, note->lengthBeats, note->midiNote });
    const auto batch = onNotesMoved;
    const auto single = onNoteMoved;
    if (batch) { batch(results); return; }
    const juce::Component::SafePointer<PitchEditor> safeThis(this);
    for (const auto& result : results)
    { if (safeThis == nullptr) break; if (single) single(result.id, result.startBeat, result.lengthBeats, result.midiNote); }
}

void PitchEditor::cancelNoteDrag()
{
    if (drag_.id < 0) return;
    for (const auto& original : dragNotes_)
        if (auto note = std::find_if(notes_.begin(), notes_.end(), [&original](const auto& value) { return value.id == original.id; }); note != notes_.end())
        { note->startBeat = original.startBeat; note->lengthBeats = original.lengthBeats; note->midiNote = original.midiNote; }
    drag_ = {}; dragNotes_.clear();
    rebuildGeometry(); repaint();
}
void PitchEditor::focusLost(FocusChangeType type) { cancelNoteDrag(); EditorSurface::focusLost(type); }
void PitchEditor::visibilityChanged() { if (!isShowing()) cancelNoteDrag(); }
void PitchEditor::enablementChanged() { if (!isEnabled()) cancelNoteDrag(); }
bool PitchEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { cancelNoteDrag(); selectFromUser(-1); return true; }
    if ((key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) && key.getKeyCode() == 'A')
    {
        std::vector<int> ids;
        for (const auto& note : notes_) ids.push_back(note.id);
        setSelectedNotes(std::move(ids));
        const auto callback = onNotesSelected;
        const auto selected = selectedNotes_;
        if (callback) callback(selected);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey)
    {
        if (notes_.empty()) return true;
        const auto found = std::find_if(notes_.begin(), notes_.end(), [this](const auto& note) { return note.id == selectedNote_; });
        auto index = found == notes_.end() ? -1 : static_cast<int>(std::distance(notes_.begin(), found));
        const auto count = static_cast<int>(notes_.size());
        index = index < 0 ? (key.getKeyCode() == juce::KeyPress::upKey ? count - 1 : 0)
                          : (index + (key.getKeyCode() == juce::KeyPress::upKey ? count - 1 : 1)) % count;
        selectFromUser(notes_[static_cast<std::size_t>(index)].id, key.getModifiers().isShiftDown());
        return true;
    }
    return EditorSurface::keyPressed(key);
}
}
