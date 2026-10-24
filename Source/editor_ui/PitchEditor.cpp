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
    const auto pitch = std::isfinite(note.pitch) ? note.pitch : static_cast<double>(note.midiNote);
    const auto centreY = (static_cast<float>(static_cast<double>(highest) - pitch) + 0.5f) * row;
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
    setDescription("Pointer drags pitch; drag empty space to select notes. Command or Control click and right click use the secondary tool. Escape cancels an edit.");
}
PitchEditor::~PitchEditor() { stopTimer(); endPitchPreview(); }
void PitchEditor::endPitchPreview()
{
    if (!previewActive_) return;
    previewActive_ = false;
    lastPreviewPitch_ = std::numeric_limits<double>::quiet_NaN();
    auto callback = onPitchPreviewEnd;
    if (callback) callback();
}
void PitchEditor::setNotes(std::vector<NoteSegment> notes)
{
    stopTimer(); drag_ = {}; dragNotes_.clear(); marquee_ = {}; pencil_ = {};
    notes_ = std::move(notes);
    if (std::none_of(notes_.begin(), notes_.end(), [this](const auto& note) { return note.id == selectedNote_; })) selectedNote_ = -1;
    std::erase_if(selectedNotes_, [this](int id) { return std::none_of(notes_.begin(), notes_.end(), [id](const auto& note) { return note.id == id; }); });
    rebuildGeometry(); repaint(); endPitchPreview();
}
void PitchEditor::setNoteRange(int low, int high)
{
    const juce::Component::SafePointer<PitchEditor> safe(this);
    cancelNoteDrag(); if (safe == nullptr) return;
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
void PitchEditor::setSelectedNote(int id) { setSelectedNotes(id < 0 ? std::vector<int>{} : std::vector<int>{ id }); }
void PitchEditor::setTool(PitchTool tool)
{
    tool_ = tool;
    setMouseCursor(tool == PitchTool::pointer ? juce::MouseCursor::NormalCursor : juce::MouseCursor::CrosshairCursor);
}
void PitchEditor::setSecondaryTool(PitchTool tool) { secondaryTool_ = tool; }
void PitchEditor::setReadOnly(bool readOnly)
{
    if (readOnly == readOnly_) return;
    readOnly_ = readOnly;
    if (readOnly_) cancelNoteDrag();
}
void PitchEditor::setSnapBeats(double beats) { if (std::isfinite(beats) && beats >= 0.0) snapBeats_ = beats; }
void PitchEditor::setMinimumNoteLengthBeats(double beats) { if (std::isfinite(beats) && beats > 0.0) minimumLengthBeats_ = beats; }
void PitchEditor::resized()
{
    const juce::Component::SafePointer<PitchEditor> safe(this);
    cancelNoteDrag(); if (safe != nullptr) rebuildGeometry();
}
void PitchEditor::viewportChanged()
{
    const juce::Component::SafePointer<PitchEditor> safe(this);
    cancelNoteDrag(); if (safe != nullptr) { rebuildGeometry(); repaint(); }
}
int PitchEditor::midiNoteAt(float y) const noexcept { return juce::roundToInt(pitchAt(y)); }
double PitchEditor::pitchAt(float y) const noexcept
{
    if (getHeight() <= 0) return static_cast<double>(lowest_);
    const auto row = static_cast<double>(getHeight()) / static_cast<double>(highest_ - lowest_ + 1);
    return std::clamp(static_cast<double>(highest_) + 0.5 - static_cast<double>(y) / row,
                      static_cast<double>(lowest_), static_cast<double>(highest_));
}
double PitchEditor::snappedBeat(float x) const noexcept
{
    const auto raw = viewport_.beatAtX(x);
    return std::max(0.0, snapBeats_ > 0.0 ? std::round(raw / snapBeats_) * snapBeats_ : raw);
}
double PitchEditor::transformPitch(double requestedPitch, double startBeat) const
{
    const auto callback = pitchTransform;
    const auto result = callback ? callback(requestedPitch, startBeat) : requestedPitch;
    return std::clamp(std::isfinite(result) ? result : requestedPitch, 0.0, 127.0);
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
    for (auto it = visibleNotes_.rbegin(); it != visibleNotes_.rend(); ++it) if (it->bounds.contains(point)) return it->id;
    return -1;
}
void PitchEditor::rebuildGeometry()
{
    ++geometryRevision_; minorGrid_.clear(); majorGrid_.clear(); rowGrid_.clear(); visibleNotes_.clear();
    if (getWidth() <= 0 || getHeight() <= 0) return;
    const auto row = static_cast<float>(getHeight()) / static_cast<float>(highest_ - lowest_ + 1);
    for (auto index = 1; index <= highest_ - lowest_; ++index)
    { const auto y = static_cast<float>(index) * row; rowGrid_.startNewSubPath(0.0f, y); rowGrid_.lineTo(static_cast<float>(getWidth()), y); }
    const auto step = viewport_.pixelsPerBeat >= 32.0 ? 0.25 : 1.0;
    viewport_.forEachGridLine(viewport_.firstBeat, viewport_.beatAtX(static_cast<float>(getWidth())), step, [&](double beat, bool bar)
    {
        auto& path = bar ? majorGrid_ : minorGrid_; const auto x = std::floor(viewport_.xForBeat(beat)) + 0.5f;
        path.startNewSubPath(x, 0.0f); path.lineTo(x, static_cast<float>(getHeight()));
    });
    visibleNotes_.reserve(notes_.size()); const auto screen = getLocalBounds().toFloat();
    for (const auto& note : notes_)
    {
        const auto pitch = std::isfinite(note.pitch) ? note.pitch : static_cast<double>(note.midiNote);
        if (!std::isfinite(note.startBeat) || !std::isfinite(note.lengthBeats) || note.lengthBeats <= 0.0 || pitch < 0.0 || pitch > 127.0) continue;
        const auto bounds = noteRectangle(note, viewport_, highest_, row);
        if (bounds.getRight() < 0.0f || bounds.getX() > screen.getRight() || bounds.getWidth() <= 0.0f) continue;
        float minimum = 0.0f, maximum = 0.0f;
        for (const auto offset : note.pitchOffsets) if (std::isfinite(offset))
        { minimum = std::min(minimum, std::clamp(offset, -127.0f, 127.0f)); maximum = std::max(maximum, std::clamp(offset, -127.0f, 127.0f)); }
        const auto visual = bounds.getUnion(juce::Rectangle<float>(bounds.getX(), bounds.getCentreY() - maximum * row,
                                                                  bounds.getWidth(), std::max(1.0f, (maximum - minimum) * row)));
        if (!visual.intersects(screen)) continue;
        CachedNote cached { note.id, bounds, visual, {}, note.colour, note.ghost, std::isfinite(note.gainDb) ? std::clamp(note.gainDb, -24.0, 12.0) : 0.0 };
        if (!note.pitchOffsets.empty())
        {
            const auto count = note.pitchOffsets.size();
            const auto left = std::max(screen.getX(), bounds.getX()), right = std::min(screen.getRight(), bounds.getRight());
            const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor((left - bounds.getX()) / bounds.getWidth() * static_cast<float>(count - 1))));
            const auto last = std::min(count - 1, static_cast<std::size_t>(std::ceil((right - bounds.getX()) / bounds.getWidth() * static_cast<float>(count - 1))));
            const auto stride = std::max(std::size_t{1}, (last - first) / static_cast<std::size_t>(std::max(1.0f, (right - left) * 2.0f)));
            bool started = false;
            for (auto index = first; index <= last; index += stride)
            {
                const auto x = bounds.getX() + (count == 1 ? 0.0f : static_cast<float>(index) / static_cast<float>(count - 1) * bounds.getWidth());
                const auto offset = std::isfinite(note.pitchOffsets[index]) ? std::clamp(note.pitchOffsets[index], -127.0f, 127.0f) : 0.0f;
                const auto y = bounds.getCentreY() - offset * row;
                if (started) cached.contour.lineTo(x, y); else { cached.contour.startNewSubPath(x, y); started = true; }
                if (last - index < stride) break;
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
    for (auto note = highest_; note >= lowest_; --note) if (isBlackKey(note))
    { g.setColour(colour(tokens::surface).withAlpha(0.65f)); g.fillRect(0.0f, static_cast<float>(highest_ - note) * row, static_cast<float>(getWidth()), row); }
    g.setColour(colour(tokens::border).withAlpha(0.33f));
    g.strokePath(minorGrid_, juce::PathStrokeType(1.0f)); g.strokePath(rowGrid_, juce::PathStrokeType(0.6f));
    g.setColour(colour(tokens::faint).withAlpha(0.32f)); g.strokePath(majorGrid_, juce::PathStrokeType(1.0f));
    const auto clip = g.getClipBounds().toFloat();
    for (const auto& note : visibleNotes_)
    {
        if (!note.visualBounds.expanded(1.0f).intersects(clip)) continue;
        const auto brightness = std::clamp(static_cast<float>(std::pow(10.0, note.gainDb / 40.0)), 0.38f, 1.7f);
        const auto colourValue = (note.ghost ? colour(tokens::muted) : note.colour).withMultipliedBrightness(brightness);
        const auto selected = std::find(selectedNotes_.begin(), selectedNotes_.end(), note.id) != selectedNotes_.end();
        g.setColour(colourValue.darker(0.3f).withAlpha(note.ghost ? 0.23f : selected ? 0.72f : 0.6f)); g.fillRoundedRectangle(note.bounds, 1.5f);
        g.setColour(colourValue.withAlpha(note.ghost ? 0.4f : 0.75f)); g.drawRoundedRectangle(note.bounds.reduced(0.25f), 1.5f, 0.6f);
        g.setColour(colourValue.brighter(0.25f).withAlpha(note.ghost ? 0.52f : 0.98f));
        g.strokePath(note.contour, juce::PathStrokeType(note.ghost ? 0.8f : 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (selected) { g.setColour(colour(tokens::text)); g.drawRoundedRectangle(note.bounds.expanded(1.0f), 2.0f, 1.5f); }
        if (drag_.id == note.id && drag_.tool == PitchTool::gain)
        {
            g.setFont(font(tokens::fontSmall)); g.setColour(colour(tokens::text));
            g.drawText(juce::String(note.gainDb, 1) + " dB", note.bounds.withY(note.bounds.getY() - 18.0f).withHeight(18.0f), juce::Justification::centredLeft);
        }
    }
    if (pencil_.active)
    {
        NoteSegment preview; preview.startBeat = pencil_.start; preview.lengthBeats = pencil_.length; preview.pitch = pencil_.pitch;
        const auto bounds = noteRectangle(preview, viewport_, highest_, row);
        g.setColour(colour(tokens::olive).withAlpha(0.5f)); g.fillRoundedRectangle(bounds, 1.5f);
        g.setColour(colour(tokens::text)); g.drawRoundedRectangle(bounds, 1.5f, 1.0f);
    }
    if (marquee_.active && marquee_.moved)
    {
        g.setColour(colour(tokens::blue).withAlpha(0.15f)); g.fillRect(marquee_.bounds);
        g.setColour(colour(tokens::blue)); g.drawRect(marquee_.bounds, 1.0f);
    }
    drawFocus(g, *this);
}
void PitchEditor::selectFromUser(int id, bool extend)
{
    const auto single = onNoteSelected; const auto multiple = onNotesSelected;
    const juce::Component::SafePointer<PitchEditor> safe(this);
    auto ids = extend ? selectedNotes_ : std::vector<int>{};
    const auto found = std::find(ids.begin(), ids.end(), id);
    if (extend && found != ids.end()) ids.erase(found); else if (id >= 0) ids.push_back(id);
    setSelectedNotes(std::move(ids));
    const auto selected = selectedNotes_; const auto primary = selectedNote_;
    if (single) single(primary);
    if (safe != nullptr && multiple) multiple(selected);
}
void PitchEditor::mouseDown(const juce::MouseEvent& event)
{
    if (!isEnabled() || (!event.mods.isLeftButtonDown() && !event.mods.isRightButtonDown())) return;
    const juce::Component::SafePointer<PitchEditor> safe(this);
    cancelNoteDrag(); if (safe == nullptr) return;
    grabKeyboardFocus(); if (safe == nullptr) return;
    const auto position = event.position;
   #if JUCE_MAC
    const auto modified = event.mods.isCommandDown();
   #else
    const auto modified = event.mods.isCtrlDown();
   #endif
    const auto secondary = modified || event.mods.isRightButtonDown();
    const auto tool = secondary ? secondaryTool_ : tool_;
    const auto id = hitTestNote(position);
    if (readOnly_ && tool != PitchTool::pointer && tool != PitchTool::zoom) return;
    if (tool == PitchTool::zoom)
    {
        const auto callback = onViewportGesture;
        if (callback) callback(0.0, secondary || event.mods.isAltDown() ? 0.5 : 2.0, position.x);
        return;
    }
    if (tool == PitchTool::pencil)
    {
        const auto beat = snappedBeat(position.x); const auto pitch = pitchAt(position.y);
        const auto create = onNoteCreated;
        if (!create) { const auto callback = onEditRequested; if (callback) callback(tool, id, beat, midiNoteAt(position.y)); return; }
        selectFromUser(-1); if (safe == nullptr) return;
        const auto lengthProvider = defaultNoteLengthBeats;
        auto length = lengthProvider ? lengthProvider(beat) : 0.5;
        if (safe == nullptr) return;
        if (!std::isfinite(length) || length <= 0.0) length = 0.5;
        const auto transformed = transformPitch(pitch, beat);
        if (safe == nullptr) return;
        pencil_ = { true, beat, beat, std::max(minimumLengthBeats_, length), transformed, position, pitch };
        startTimerHz(30); repaint(); return;
    }
    if (tool == PitchTool::pointer)
    {
        const auto selected = std::find(selectedNotes_.begin(), selectedNotes_.end(), id) != selectedNotes_.end();
        if (event.mods.isShiftDown() || !selected) selectFromUser(id, event.mods.isShiftDown());
        if (safe == nullptr) return;
        if (id < 0)
        {
            marquee_ = { true, false, position, {}, selectedNotes_ }; startTimerHz(30); return;
        }
        if (readOnly_ || std::find(selectedNotes_.begin(), selectedNotes_.end(), id) == selectedNotes_.end()) return;
    }
    else if (tool == PitchTool::eraser || tool == PitchTool::scissors || tool == PitchTool::join)
    {
        if (id < 0) return;
        const auto callback = onEditRequested;
        if (callback) callback(tool, id, snappedBeat(position.x), midiNoteAt(position.y));
        return;
    }
    else
    {
        if (id < 0) return;
        selectFromUser(id); if (safe == nullptr) return;
    }
    const auto note = std::find_if(notes_.begin(), notes_.end(), [id](const auto& value) { return value.id == id; });
    if (note == notes_.end()) return;
    const auto pitch = std::isfinite(note->pitch) ? note->pitch : static_cast<double>(note->midiNote);
    const auto gain = std::isfinite(note->gainDb) ? std::clamp(note->gainDb, -24.0, 12.0) : 0.0;
    const auto edge = tool == PitchTool::flex ? (position.x < noteBounds(id).getCentreX() ? -1 : 1) : 0;
    drag_ = { id, edge, note->startBeat, note->lengthBeats, pitch, gain, tool == PitchTool::gain ? gain : 1.0, tool, position, false };
    for (const auto& value : notes_)
        if (value.id == id || (tool == PitchTool::pointer && std::find(selectedNotes_.begin(), selectedNotes_.end(), value.id) != selectedNotes_.end())) dragNotes_.push_back(value);
    startTimerHz(30);
    if (tool == PitchTool::pointer)
    {
        const auto callback = onPitchPreview;
        if (callback) { previewActive_ = true; lastPreviewPitch_ = pitch; callback(pitch, true); }
    }
}
void PitchEditor::mouseDrag(const juce::MouseEvent& event)
{
    if (!isEnabled()) return;
    if (pencil_.active)
    {
        if (std::abs(event.position.x - pencil_.origin.x) < 3.0f) return;
        const auto beat = snappedBeat(event.position.x);
        const auto start = std::min(pencil_.anchor, beat);
        const auto length = std::max(minimumLengthBeats_, std::abs(beat - pencil_.anchor));
        const juce::Component::SafePointer<PitchEditor> safe(this);
        const auto transformed = transformPitch(pencil_.requestedPitch, start);
        if (safe == nullptr || !pencil_.active) return;
        pencil_.start = start; pencil_.length = length; pencil_.pitch = transformed;
        repaint(); return;
    }
    if (marquee_.active)
    {
        if (!marquee_.moved && event.position.getDistanceFrom(marquee_.origin) < 3.0f) return;
        marquee_.moved = true; marquee_.bounds = juce::Rectangle<float>(marquee_.origin, event.position);
        auto selected = marquee_.baseSelection;
        for (const auto& note : notes_)
            if (noteBounds(note.id).intersects(marquee_.bounds) && std::find(selected.begin(), selected.end(), note.id) == selected.end()) selected.push_back(note.id);
        setSelectedNotes(std::move(selected)); repaint(); return;
    }
    if (drag_.id < 0) return;
    auto note = std::find_if(notes_.begin(), notes_.end(), [this](const auto& value) { return value.id == drag_.id; });
    if (note == notes_.end()) { cancelNoteDrag(); return; }
    const auto delta = event.position - drag_.origin;
    const auto row = static_cast<double>(getHeight()) / static_cast<double>(highest_ - lowest_ + 1);
    if (!drag_.moved && std::abs(drag_.tool == PitchTool::flex ? delta.x : delta.y) < (drag_.tool == PitchTool::pointer ? static_cast<float>(row * 0.01) : 0.1f)) return;
    drag_.moved = true;
    if (drag_.tool == PitchTool::pointer)
    {
        auto pitchDelta = -static_cast<double>(delta.y) / std::max(1.0, row);
        for (const auto& original : dragNotes_)
        {
            const auto originalPitch = std::isfinite(original.pitch) ? original.pitch : static_cast<double>(original.midiNote);
            pitchDelta = std::clamp(pitchDelta, static_cast<double>(lowest_) - originalPitch, static_cast<double>(highest_) - originalPitch);
        }
        const juce::Component::SafePointer<PitchEditor> safe(this);
        const auto gestureId = drag_.id;
        for (const auto& original : dragNotes_)
        {
            const auto transformed = transformPitch((std::isfinite(original.pitch) ? original.pitch : static_cast<double>(original.midiNote)) + pitchDelta,
                                                    original.startBeat);
            if (safe == nullptr || drag_.id != gestureId) return;
            if (auto moved = std::find_if(notes_.begin(), notes_.end(), [&original](const auto& value) { return value.id == original.id; }); moved != notes_.end())
            {
                moved->pitch = transformed; moved->midiNote = juce::roundToInt(transformed); moved->startBeat = original.startBeat;
            }
        }
    }
    else if (drag_.tool == PitchTool::flex)
    {
        const auto beatShift = static_cast<double>(delta.x) / viewport_.pixelsPerBeat;
        const auto snap = [this](double beat) { return snapBeats_ > 0.0 ? std::round(beat / snapBeats_) * snapBeats_ : beat; };
        if (drag_.edge < 0)
        {
            note->startBeat = std::clamp(snap(drag_.start + beatShift), 0.0, drag_.start + drag_.length - std::min(minimumLengthBeats_, drag_.length));
            note->lengthBeats = drag_.start + drag_.length - note->startBeat;
        }
        else note->lengthBeats = std::max(minimumLengthBeats_, snap(drag_.start + drag_.length + beatShift) - drag_.start);
        const juce::Component::SafePointer<PitchEditor> safe(this);
        const auto gestureId = drag_.id;
        const auto transformed = transformPitch(drag_.pitch, note->startBeat);
        if (safe == nullptr || drag_.id != gestureId) return;
        note->pitch = transformed; note->midiNote = juce::roundToInt(transformed);
    }
    else if (drag_.tool == PitchTool::vibrato)
    {
        drag_.expression = std::clamp(1.0 - static_cast<double>(delta.y) / 64.0, 0.0, 4.0);
        if (!dragNotes_.empty())
            for (std::size_t index = 0; index < note->pitchOffsets.size(); ++index)
                note->pitchOffsets[index] = std::isfinite(dragNotes_.front().pitchOffsets[index])
                    ? std::clamp(std::clamp(dragNotes_.front().pitchOffsets[index], -127.0f, 127.0f) * static_cast<float>(drag_.expression), -127.0f, 127.0f) : 0.0f;
    }
    else if (drag_.tool == PitchTool::gain)
    { drag_.expression = std::clamp(drag_.gainDb - static_cast<double>(delta.y) / 8.0, -24.0, 12.0); note->gainDb = drag_.expression; }
    rebuildGeometry(); repaint();
    if (drag_.tool == PitchTool::pointer && previewActive_)
    {
        const auto pitch = note->pitch;
        if (std::abs(pitch - lastPreviewPitch_) >= 0.01)
        { lastPreviewPitch_ = pitch; const auto callback = onPitchPreview; if (callback) callback(pitch, false); }
    }
}
void PitchEditor::mouseUp(const juce::MouseEvent&)
{
    stopTimer();
    if (pencil_.active)
    {
        const auto result = pencil_; pencil_ = {}; repaint();
        const auto callback = onNoteCreated; if (callback) callback(result.start, result.length, result.pitch); return;
    }
    if (marquee_.active)
    {
        const auto changed = marquee_.moved; marquee_ = {}; repaint();
        if (changed)
        {
            const auto single = onNoteSelected; const auto multiple = onNotesSelected;
            const auto selected = selectedNotes_; const auto primary = selectedNote_;
            const juce::Component::SafePointer<PitchEditor> safe(this);
            if (single) single(primary);
            if (safe != nullptr && multiple) multiple(selected);
        }
        return;
    }
    const auto gesture = drag_; drag_ = {}; repaint();
    auto originals = std::move(dragNotes_); dragNotes_.clear();
    std::vector<NoteMovement> results;
    bool changed = false;
    if (gesture.id >= 0 && gesture.moved)
        for (const auto& original : originals)
            if (auto note = std::find_if(notes_.begin(), notes_.end(), [&original](const auto& value) { return value.id == original.id; }); note != notes_.end())
            {
                const auto pitch = std::isfinite(note->pitch) ? note->pitch : static_cast<double>(note->midiNote);
                const auto oldPitch = std::isfinite(original.pitch) ? original.pitch : static_cast<double>(original.midiNote);
                results.push_back({ note->id, note->startBeat, note->lengthBeats, note->midiNote, pitch });
                if (gesture.tool == PitchTool::gain)
                    changed = changed || !juce::approximatelyEqual(note->gainDb, original.gainDb);
                else if (gesture.tool == PitchTool::vibrato)
                {
                    for (std::size_t index = 0; index < note->pitchOffsets.size(); ++index)
                        changed = changed || !juce::approximatelyEqual(note->pitchOffsets[index], original.pitchOffsets[index]);
                }
                else changed = changed || !juce::approximatelyEqual(pitch, oldPitch)
                    || !juce::approximatelyEqual(note->startBeat, original.startBeat)
                    || !juce::approximatelyEqual(note->lengthBeats, original.lengthBeats);
            }
    const juce::Component::SafePointer<PitchEditor> safe(this);
    endPitchPreview(); if (safe == nullptr || gesture.id < 0 || !gesture.moved || !changed) return;
    if (gesture.tool == PitchTool::vibrato || gesture.tool == PitchTool::gain)
    { const auto callback = onNoteExpressionEdited; if (callback) callback(gesture.tool, gesture.id, gesture.expression); return; }
    const auto batch = onNotesMoved; const auto single = onNoteMoved;
    if (batch) { batch(results); return; }
    for (const auto& result : results)
    { if (safe == nullptr) break; if (single) single(result.id, result.startBeat, result.lengthBeats, result.midiNote); }
}
void PitchEditor::cancelNoteDrag()
{
    const auto changed = drag_.id >= 0 || pencil_.active || marquee_.active;
    for (const auto& original : dragNotes_)
        if (auto note = std::find_if(notes_.begin(), notes_.end(), [&original](const auto& value) { return value.id == original.id; }); note != notes_.end()) *note = original;
    if (marquee_.active) setSelectedNotes(std::move(marquee_.baseSelection));
    stopTimer(); drag_ = {}; dragNotes_.clear(); marquee_ = {}; pencil_ = {};
    if (changed) { rebuildGeometry(); repaint(); }
    endPitchPreview();
}
void PitchEditor::timerCallback()
{
    if (!juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown() || !isShowing()) cancelNoteDrag();
}
void PitchEditor::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    const auto callback = onWheelGesture;
    if (callback) callback(event, wheel); else EditorSurface::mouseWheelMove(event, wheel);
}
void PitchEditor::focusLost(FocusChangeType type)
{
    const juce::Component::SafePointer<PitchEditor> safe(this);
    cancelNoteDrag(); if (safe != nullptr) EditorSurface::focusLost(type);
}
void PitchEditor::visibilityChanged() { if (!isShowing()) cancelNoteDrag(); }
void PitchEditor::enablementChanged() { if (!isEnabled()) cancelNoteDrag(); }
bool PitchEditor::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled()) return false;
    if (key == juce::KeyPress::escapeKey)
    {
        const juce::Component::SafePointer<PitchEditor> safe(this);
        cancelNoteDrag(); if (safe != nullptr) selectFromUser(-1); return true;
    }
    if ((key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) && key.getKeyCode() == 'A')
    {
        std::vector<int> ids; for (const auto& note : notes_) ids.push_back(note.id);
        setSelectedNotes(std::move(ids)); const auto callback = onNotesSelected; const auto selected = selectedNotes_;
        if (callback) callback(selected); return true;
    }
    if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey)
    {
        if (notes_.empty()) return true;
        const auto found = std::find_if(notes_.begin(), notes_.end(), [this](const auto& note) { return note.id == selectedNote_; });
        auto index = found == notes_.end() ? -1 : static_cast<int>(std::distance(notes_.begin(), found));
        const auto count = static_cast<int>(notes_.size());
        index = index < 0 ? (key.getKeyCode() == juce::KeyPress::upKey ? count - 1 : 0)
                          : (index + (key.getKeyCode() == juce::KeyPress::upKey ? count - 1 : 1)) % count;
        selectFromUser(notes_[static_cast<std::size_t>(index)].id, key.getModifiers().isShiftDown()); return true;
    }
    return EditorSurface::keyPressed(key);
}
}
