#include "AnnotationEditorComponent.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace vocal_annotation
{

namespace
{
constexpr int kTimelineHeight = 26;
constexpr int kKeyboardWidth = 150;
constexpr int kLyricsHeight = 34;
constexpr int kMusicalContextHeight = 112;
constexpr int kInstrumentalWaveformHeight = 64;
constexpr int kTrackHeaderWidth = kKeyboardWidth;
constexpr int kCompactTrackHeight = 66;
constexpr double kMinNoteLength = 0.02;
constexpr double kBoundaryHitPixels = 7.0;
constexpr float kSelectedCornerRadius = 5.0f;

int instrumentalTrackHeight(const AnnotationDocument& document)
{
    if (! document.instrumentalFile.existsAsFile())
        return 0;

    return (document.tempoSegments.empty() && document.timeSignatures.empty() && document.chords.empty())
        ? kInstrumentalWaveformHeight
        : kMusicalContextHeight + kInstrumentalWaveformHeight;
}

int numeratorAtTime(const AnnotationDocument& document, double time)
{
    for (const auto& signature : document.timeSignatures)
        if (time >= signature.start && time < signature.end)
            return juce::jlimit(1, 12, signature.numerator);

    return 4;
}

std::vector<double> beatTimesFor(const AnnotationDocument& document, double visibleStart, double visibleEnd)
{
    std::vector<double> beats;
    const auto duration = juce::jmax(document.duration, visibleEnd);

    if (document.tempoSegments.empty())
    {
        const auto step = 60.0 / juce::jlimit(20.0, 300.0, document.bpm);
        auto time = std::floor(visibleStart / step) * step;
        while (time <= visibleEnd + step && time <= duration + 0.001)
        {
            if (time >= visibleStart - step)
                beats.push_back(time);
            time += step;
        }
        return beats;
    }

    for (const auto& tempo : document.tempoSegments)
    {
        const auto bpm = juce::jlimit(20.0, 300.0, tempo.bpm);
        const auto step = 60.0 / bpm;
        const auto segmentStart = juce::jmax(0.0, tempo.start);
        const auto segmentEnd = juce::jmin(duration, tempo.end > tempo.start ? tempo.end : duration);
        if (segmentEnd < visibleStart || segmentStart > visibleEnd)
            continue;

        auto beatIndex = std::floor((visibleStart - segmentStart) / step);
        auto time = segmentStart + juce::jmax(0.0, beatIndex) * step;
        while (time <= segmentEnd + 0.001 && time <= visibleEnd + step)
        {
            if (time >= visibleStart - step)
                beats.push_back(time);
            time += step;
        }
    }

    std::sort(beats.begin(), beats.end());
    beats.erase(std::unique(beats.begin(), beats.end(), [](double a, double b) { return std::abs(a - b) < 0.01; }), beats.end());
    return beats;
}

juce::Colour selectedShadowColour()
{
    return juce::Colours::black.withAlpha(0.70f);
}

const juce::MouseCursor& knifeCursor()
{
    static const juce::MouseCursor cursor = []
    {
        juce::Image image(juce::Image::ARGB, 32, 32, true);
        juce::Graphics g(image);
        juce::Path shadow;
        shadow.startNewSubPath(9.0f, 26.0f);
        shadow.lineTo(24.0f, 5.0f);
        shadow.lineTo(27.0f, 8.0f);
        shadow.lineTo(12.0f, 28.0f);
        shadow.closeSubPath();
        g.setColour(juce::Colours::black.withAlpha(0.72f));
        g.fillPath(shadow);

        juce::Path blade;
        blade.startNewSubPath(20.0f, 4.0f);
        blade.lineTo(27.0f, 7.0f);
        blade.lineTo(16.0f, 18.0f);
        blade.lineTo(12.0f, 16.0f);
        blade.closeSubPath();
        g.setColour(juce::Colours::white);
        g.fillPath(blade);
        g.setColour(juce::Colour(0xff9ca3af));
        g.strokePath(blade, juce::PathStrokeType(1.0f));

        juce::Path handle;
        handle.startNewSubPath(12.0f, 16.0f);
        handle.lineTo(16.0f, 18.0f);
        handle.lineTo(9.0f, 28.0f);
        handle.lineTo(5.0f, 26.0f);
        handle.closeSubPath();
        g.setColour(juce::Colour(0xff2f343c));
        g.fillPath(handle);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.strokePath(handle, juce::PathStrokeType(1.0f));
        return juce::MouseCursor(image, 14, 16);
    }();

    return cursor;
}

juce::Colour noteColourForPitch(int pitch)
{
    const auto hue = std::fmod(static_cast<float>(pitch) * 0.071f, 1.0f);
    return juce::Colour::fromHSV(hue, 0.55f, 0.82f, 0.82f);
}

juce::Colour syllableColourForIndex(size_t index)
{
    static constexpr juce::uint32 palette[] =
    {
        0xffa3e635, // lime
        0xfffacc15, // yellow
        0xffff3b4f, // red
        0xffec4899, // pink
        0xffc026d3, // magenta
        0xff7c3aed, // violet
        0xff3b82f6, // blue
        0xff06b6d4, // cyan
        0xff00c853  // green
    };

    return juce::Colour(palette[index % std::size(palette)]);
}

struct WaveformSegmentStart
{
    double time = 0.0;
    BoundaryKind kind = BoundaryKind::syllable;
};

juce::Colour waveformColourFor(BoundaryKind kind, size_t index)
{
    switch (kind)
    {
        case BoundaryKind::breath:         return juce::Colours::white;
        case BoundaryKind::noise:          return juce::Colour(0xff16345f);
        case BoundaryKind::pause:          return juce::Colour(0xff64748b);
        case BoundaryKind::syllable:
        case BoundaryKind::rearticulation:
        case BoundaryKind::legato:
        case BoundaryKind::ignore:         return syllableColourForIndex(index);
    }

    return syllableColourForIndex(index);
}

std::vector<WaveformSegmentStart> waveformSegmentStarts(const AnnotationDocument& document)
{
    std::vector<WaveformSegmentStart> starts;
    starts.reserve(document.boundaries.size() + document.regions.size() * 2);
    for (const auto& boundary : document.boundaries)
    {
        if (boundary.kind != BoundaryKind::ignore
            && boundary.time >= 0.0
            && boundary.time <= document.duration)
        {
            starts.push_back({ boundary.time, boundary.kind });
        }
    }

    for (const auto& region : document.regions)
    {
        const auto kind = boundaryKindFromString(region.kind);
        if (kind != BoundaryKind::breath && kind != BoundaryKind::noise && kind != BoundaryKind::pause)
            continue;

        if (region.end <= region.start || region.end < 0.0 || region.start > document.duration)
            continue;

        starts.push_back({ juce::jlimit(0.0, document.duration, region.start), kind });
        starts.push_back({ juce::jlimit(0.0, document.duration, region.end), BoundaryKind::ignore });
    }

    std::sort(starts.begin(), starts.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    std::vector<WaveformSegmentStart> merged;
    merged.reserve(starts.size());
    const auto kindPriority = [](BoundaryKind kind)
    {
        return kind == BoundaryKind::breath || kind == BoundaryKind::noise || kind == BoundaryKind::pause ? 2
             : kind == BoundaryKind::ignore ? 0
             : 1;
    };

    for (const auto& start : starts)
    {
        if (! merged.empty() && std::abs(start.time - merged.back().time) < 0.01)
        {
            if (kindPriority(start.kind) > kindPriority(merged.back().kind))
                merged.back().kind = start.kind;
            continue;
        }

        merged.push_back(start);
    }
    return merged;
}

bool isBlackKey(int midiPitch)
{
    switch (midiPitch % 12)
    {
        case 1:
        case 3:
        case 6:
        case 8:
        case 10:
            return true;
        default:
            return false;
    }
}
} // namespace

AnnotationEditorComponent::AnnotationEditorComponent(juce::AudioFormatManager& formatManager, AnnotationDocument& document)
    : document_(document),
      formatManager_(formatManager),
      thumbnailCache_(16),
      thumbnail_(512, formatManager_, thumbnailCache_),
      instrumentalThumbnail_(512, formatManager_, thumbnailCache_),
      backingThumbnail_(512, formatManager_, thumbnailCache_)
{
    setWantsKeyboardFocus(true);
}

void AnnotationEditorComponent::setListener(Listener* newListener)
{
    listener_ = newListener;
}

void AnnotationEditorComponent::setAudioFile(const juce::File& file)
{
    thumbnail_.setSource(new juce::FileInputSource(file));
    if (file.existsAsFile())
        selectedTrack_ = SelectedTrack::lead;
    fitToClip();
}

void AnnotationEditorComponent::setInstrumentalFile(const juce::File& file)
{
    instrumentalThumbnail_.setSource(file.existsAsFile() ? new juce::FileInputSource(file) : nullptr);
    if (file.existsAsFile() && ! document_.audioFile.existsAsFile())
        selectedTrack_ = SelectedTrack::instrumental;
    repaint();
}

void AnnotationEditorComponent::setBackingAudioFile(const juce::File& file)
{
    backingThumbnail_.setSource(file.existsAsFile() ? new juce::FileInputSource(file) : nullptr);
    if (file.existsAsFile())
        selectedTrack_ = SelectedTrack::backing;
    repaint();
}

void AnnotationEditorComponent::fitToClip()
{
    visibleStart_ = 0.0;
    visibleEnd_ = juce::jmax(1.0, document_.duration);
    repaint();
}

void AnnotationEditorComponent::setPlayheadTime(double seconds)
{
    const auto nextPlayheadTime = juce::jlimit(0.0, juce::jmax(0.0, document_.duration), seconds);
    if (std::abs(nextPlayheadTime - playheadTime_) < 0.0001)
        return;

    playheadTime_ = nextPlayheadTime;
    repaint();
}

void AnnotationEditorComponent::setHorizontalZoom(double zoom)
{
    const auto duration = juce::jmax(1.0, document_.duration);
    const auto currentCenter = (visibleStart_ + visibleEnd_) * 0.5;
    const auto visibleDuration = juce::jlimit(0.1, duration, duration / juce::jmax(1.0, zoom));
    visibleStart_ = currentCenter - visibleDuration * 0.5;
    visibleEnd_ = currentCenter + visibleDuration * 0.5;
    clampView();
    repaint();
}

void AnnotationEditorComponent::setPitchZoom(double zoom)
{
    const auto targetRange = juce::jlimit(8.0, 88.0, 72.0 / juce::jmax(1.0, zoom));
    zoomPitchAt(pitchCenter_, visibleSemitones_ / targetRange);
}

void AnnotationEditorComponent::deleteSelection()
{
    if (auto index = document_.findNoteIndex(selectedNoteId_))
    {
        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        document_.notes.erase(document_.notes.begin() + *index);
        selectedNoteId_.clear();
        notifyChanged();
        return;
    }

    if (auto index = document_.findBoundaryIndex(selectedBoundaryId_))
    {
        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        document_.boundaries.erase(document_.boundaries.begin() + *index);
        selectedBoundaryId_.clear();
        notifyChanged();
    }
}

void AnnotationEditorComponent::splitSelectedAtPlayhead()
{
    if (auto index = document_.findNoteIndex(selectedNoteId_))
        splitNoteAt(*index, playheadTime_);
}

void AnnotationEditorComponent::mergeSelectedWithNext()
{
    if (auto index = document_.findNoteIndex(selectedNoteId_))
    {
        sortNotes();
        index = document_.findNoteIndex(selectedNoteId_);
        if (! index || *index + 1 >= static_cast<int>(document_.notes.size()))
            return;

        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        auto& note = document_.notes[static_cast<size_t>(*index)];
        auto& next = document_.notes[static_cast<size_t>(*index + 1)];
        note.end = juce::jmax(note.end, next.end);
        note.voicedEnd = juce::jmax(note.voicedEnd, next.voicedEnd);
        note.curve.insert(note.curve.end(), next.curve.begin(), next.curve.end());
        note.lyric = note.lyric.trim() + (note.lyric.isNotEmpty() && next.lyric.isNotEmpty() ? " " : "") + next.lyric.trim();
        document_.notes.erase(document_.notes.begin() + *index + 1);
        recalculateNotePitchFromCurve(note);
        notifyChanged();
    }
}

void AnnotationEditorComponent::nudgeSelectedPitch(int semitones)
{
    if (auto index = document_.findNoteIndex(selectedNoteId_))
    {
        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        auto& note = document_.notes[static_cast<size_t>(*index)];
        note.pitch = juce::jlimit(0, 127, note.pitch + semitones);
        note.pitchExact = juce::jlimit(0.0, 127.0, note.pitchExact + static_cast<double>(semitones));
        for (auto& point : note.curve)
            point.midi += semitones;
        notifyChanged();
    }
}

void AnnotationEditorComponent::nudgeSelectedTime(double seconds)
{
    if (auto index = document_.findNoteIndex(selectedNoteId_))
    {
        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        auto& note = document_.notes[static_cast<size_t>(*index)];
        const auto length = note.end - note.start;
        note.start = juce::jlimit(0.0, juce::jmax(0.0, document_.duration - length), note.start + seconds);
        note.end = note.start + length;
        note.voicedStart = juce::jlimit(note.start, note.end, note.voicedStart + seconds);
        note.voicedEnd = juce::jlimit(note.start, note.end, note.voicedEnd + seconds);
        for (auto& point : note.curve)
            point.time = juce::jlimit(0.0, document_.duration, point.time + seconds);
        notifyChanged();
        return;
    }

    if (auto index = document_.findBoundaryIndex(selectedBoundaryId_))
    {
        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        auto& boundary = document_.boundaries[static_cast<size_t>(*index)];
        boundary.time = juce::jlimit(0.0, document_.duration, boundary.time + seconds);
        notifyChanged();
    }
}

void AnnotationEditorComponent::jumpToNextSuspicious(bool reverse)
{
    if (document_.notes.empty())
        return;

    const auto isSuspicious = [](const NoteBlock& note)
    {
        if (note.end - note.start > 2.5)
            return true;

        for (const auto& point : note.curve)
            if (point.confidence < 0.55)
                return true;

        return false;
    };

    sortNotes();
    const auto start = playheadTime_;
    const NoteBlock* target = nullptr;

    if (reverse)
    {
        for (auto it = document_.notes.rbegin(); it != document_.notes.rend(); ++it)
            if (it->start < start && isSuspicious(*it))
            {
                target = &*it;
                break;
            }
    }
    else
    {
        for (const auto& note : document_.notes)
            if (note.start > start && isSuspicious(note))
            {
                target = &note;
                break;
            }
    }

    if (target == nullptr)
        return;

    playheadTime_ = target->start;
    selectedNoteId_ = target->id;
    selectedBoundaryId_.clear();
    const auto window = visibleEnd_ - visibleStart_;
    visibleStart_ = juce::jlimit(0.0, juce::jmax(0.0, document_.duration - window), playheadTime_ - window * 0.25);
    visibleEnd_ = visibleStart_ + window;
    if (listener_ != nullptr)
        listener_->selectionChanged(selectedNoteId_, selectedBoundaryId_);
    repaint();
}

void AnnotationEditorComponent::setSelectedBoundaryKind(BoundaryKind kind)
{
    if (auto index = document_.findBoundaryIndex(selectedBoundaryId_))
    {
        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        document_.boundaries[static_cast<size_t>(*index)].kind = kind;
        notifyChanged();
    }
}

juce::String AnnotationEditorComponent::getSelectedNoteId() const
{
    return selectedNoteId_;
}

juce::String AnnotationEditorComponent::getSelectedBoundaryId() const
{
    return selectedBoundaryId_;
}

void AnnotationEditorComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff171a1f));
    drawTimeline(g, getTimelineBounds());
    drawTrackOverview(g);
    drawPianoKeyboard(g, getKeyboardBounds());
    if (selectedTrack_ == SelectedTrack::backing)
    {
        drawPianoRollBackground(g, getEditorBounds());
        drawBackingNotes(g, getEditorBounds());
    }
    else if (selectedTrack_ == SelectedTrack::lead)
    {
        drawWaveform(g, getEditorBounds());
        drawNotes(g, getEditorBounds());
        drawBoundaries(g, getEditorBounds());
        drawSplitPreview(g, getEditorBounds());
    }
    else
    {
        drawPianoRollBackground(g, getEditorBounds());
        g.setColour(juce::Colours::white.withAlpha(0.42f));
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText("Instrumental track selected", getEditorBounds().reduced(20), juce::Justification::centred);
    }
    drawLyrics(g, getLyricsBounds());
}

void AnnotationEditorComponent::resized() {}

void AnnotationEditorComponent::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();
    dragMode_ = DragMode::none;
    dragStartTime_ = xToTime(event.position.x);
    playheadTime_ = dragStartTime_;
    lastDragAuditionPitch_ = -1;
    splitPreviewActive_ = false;

    if (getTimelineBounds().contains(event.getPosition()))
    {
        playheadTime_ = xToTime(event.position.x);
        if (listener_ != nullptr)
        {
            listener_->playheadPositionRequested(playheadTime_);
            listener_->selectionChanged(selectedNoteId_, selectedBoundaryId_);
        }
        repaint();
        return;
    }

    const auto handleTrackClick = [this, position = event.getPosition()](juce::Rectangle<int> row, SelectedTrack track)
    {
        if (row.isEmpty() || ! row.contains(position))
            return false;

        auto header = row.removeFromLeft(kTrackHeaderWidth);
        if (header.contains(position))
        {
            auto area = header.reduced(8, 6);
            area.removeFromTop(20);
            auto buttons = area.removeFromTop(24);
            const auto muteButton = buttons.removeFromLeft(26);
            buttons.removeFromLeft(4);
            const auto soloButton = buttons.removeFromLeft(26);

            auto toggleMute = [this](bool& muted)
            {
                muted = ! muted;
                if (listener_ != nullptr)
                    listener_->trackPlaybackStateChanged(instrumentalMuted_,
                                                         instrumentalSolo_,
                                                         leadMuted_,
                                                         leadSolo_,
                                                         backingMuted_,
                                                         backingSolo_);
            };
            auto toggleSolo = [this](bool& solo)
            {
                solo = ! solo;
                if (listener_ != nullptr)
                    listener_->trackPlaybackStateChanged(instrumentalMuted_,
                                                         instrumentalSolo_,
                                                         leadMuted_,
                                                         leadSolo_,
                                                         backingMuted_,
                                                         backingSolo_);
            };

            if (muteButton.contains(position))
            {
                if (track == SelectedTrack::instrumental)
                    toggleMute(instrumentalMuted_);
                else if (track == SelectedTrack::lead)
                    toggleMute(leadMuted_);
                else
                    toggleMute(backingMuted_);

                repaint();
                return true;
            }

            if (soloButton.contains(position))
            {
                if (track == SelectedTrack::instrumental)
                    toggleSolo(instrumentalSolo_);
                else if (track == SelectedTrack::lead)
                    toggleSolo(leadSolo_);
                else
                    toggleSolo(backingSolo_);

                repaint();
                return true;
            }
        }

        selectedTrack_ = track;
        selectedNoteId_.clear();
        selectedBoundaryId_.clear();
        if (listener_ != nullptr)
            listener_->selectionChanged({}, {});
        repaint();
        return true;
    };

    if (handleTrackClick(getInstrumentalOverviewBounds(), SelectedTrack::instrumental)
        || handleTrackClick(getLeadOverviewBounds(), SelectedTrack::lead)
        || handleTrackClick(getBackingOverviewBounds(), SelectedTrack::backing))
        return;

    if (event.mods.isPopupMenu())
    {
        if (auto noteIndex = noteAt(event.position))
        {
            selectNote(*noteIndex);
            showNoteContextMenu(*noteIndex);
            repaint();
            return;
        }
    }

    if (event.mods.isCommandDown() && selectedNoteId_.isNotEmpty())
    {
        if (auto noteIndex = noteAt(event.position))
            splitNoteAt(*noteIndex, xToTime(event.position.x));

        repaint();
        return;
    }

    if (auto boundary = boundaryAt(event.position))
    {
        selectBoundary(*boundary);
        dragMode_ = DragMode::moveBoundary;
        originalBoundaryTime_ = document_.boundaries[static_cast<size_t>(*boundary)].time;
        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        repaint();
        return;
    }

    if (auto noteIndex = noteAt(event.position))
    {
        if (event.mods.isCommandDown())
        {
            splitNoteAt(*noteIndex, xToTime(event.position.x));
            return;
        }

        selectNote(*noteIndex);
        const auto& note = document_.notes[static_cast<size_t>(*noteIndex)];
        if (listener_ != nullptr)
            listener_->noteAuditionRequested(note.id);

        originalNoteStart_ = note.start;
        originalNoteEnd_ = note.end;
        originalNotePitch_ = note.pitch;
        lastDragAuditionPitch_ = note.pitch;
        originalNoteSnapshot_ = note;
        dragStartPitch_ = yToPitch(event.position.y);

        const auto left = timeToX(note.start);
        const auto right = timeToX(note.end);
        if (std::abs(event.position.x - left) < 8.0f)
            dragMode_ = DragMode::resizeNoteStart;
        else if (std::abs(event.position.x - right) < 8.0f)
            dragMode_ = DragMode::resizeNoteEnd;
        else
            dragMode_ = DragMode::moveNote;

        if (listener_ != nullptr)
            listener_->beginUndoableAction();
        repaint();
        return;
    }

    if (auto boundaryIndex = waveformPartBoundaryAt(event.position))
    {
        selectBoundary(*boundaryIndex);
        if (listener_ != nullptr)
            if (const auto range = getSelectedTimeRange())
                listener_->waveformAuditionRequested(range->start, range->end);
        repaint();
        return;
    }

    if (auto noteIndex = waveformPartNoteAt(event.position))
    {
        selectNote(*noteIndex);
        const auto& note = document_.notes[static_cast<size_t>(*noteIndex)];
        if (listener_ != nullptr)
            listener_->waveformAuditionRequested(note.start, note.end);
        repaint();
        return;
    }

    clearSelection();
    repaint();
}

void AnnotationEditorComponent::mouseDrag(const juce::MouseEvent& event)
{
    const auto currentTime = xToTime(event.position.x);
    const auto deltaTime = currentTime - dragStartTime_;

    if (dragMode_ == DragMode::moveBoundary)
    {
        if (auto index = document_.findBoundaryIndex(selectedBoundaryId_))
        {
            document_.boundaries[static_cast<size_t>(*index)].time = juce::jlimit(0.0, document_.duration, originalBoundaryTime_ + deltaTime);
            notifyChanged();
        }
        return;
    }

    auto noteIndex = document_.findNoteIndex(selectedNoteId_);
    if (! noteIndex)
        return;

    auto& note = document_.notes[static_cast<size_t>(*noteIndex)];
    if (dragMode_ == DragMode::resizeNoteStart)
    {
        note.start = juce::jlimit(0.0, originalNoteEnd_ - kMinNoteLength, originalNoteStart_ + deltaTime);
        note.voicedStart = juce::jlimit(note.start, note.end, note.voicedStart);
    }
    else if (dragMode_ == DragMode::resizeNoteEnd)
    {
        note.end = juce::jlimit(originalNoteStart_ + kMinNoteLength, document_.duration, originalNoteEnd_ + deltaTime);
        note.voicedEnd = juce::jlimit(note.start, note.end, note.voicedEnd);
    }
    else if (dragMode_ == DragMode::moveNote)
    {
        if (event.mods.isShiftDown())
        {
            const auto length = originalNoteEnd_ - originalNoteStart_;
            note.start = juce::jlimit(0.0, juce::jmax(0.0, document_.duration - length), originalNoteStart_ + deltaTime);
            note.end = note.start + length;
            note.voicedStart = juce::jlimit(note.start, note.end, originalNoteSnapshot_.voicedStart + deltaTime);
            note.voicedEnd = juce::jlimit(note.start, note.end, originalNoteSnapshot_.voicedEnd + deltaTime);
            for (size_t i = 0; i < note.curve.size() && i < originalNoteSnapshot_.curve.size(); ++i)
                note.curve[i].time = juce::jlimit(0.0, document_.duration, originalNoteSnapshot_.curve[i].time + deltaTime);
        }
        else
        {
            const auto deltaPitch = yToPitch(event.position.y) - dragStartPitch_;
            note.pitch = juce::jlimit(0, 127, originalNotePitch_ + deltaPitch);
            note.pitchExact = juce::jlimit(0.0, 127.0, originalNoteSnapshot_.pitchExact + static_cast<double>(deltaPitch));
            for (size_t i = 0; i < note.curve.size() && i < originalNoteSnapshot_.curve.size(); ++i)
                note.curve[i].midi = originalNoteSnapshot_.curve[i].midi + deltaPitch;

            if (note.pitch != lastDragAuditionPitch_)
            {
                lastDragAuditionPitch_ = note.pitch;
                if (listener_ != nullptr)
                    listener_->noteAuditionRequested(note.id);
            }
        }
    }

    notifyChanged();
}

void AnnotationEditorComponent::mouseUp(const juce::MouseEvent&)
{
    dragMode_ = DragMode::none;
    sortNotes();
}

void AnnotationEditorComponent::mouseMove(const juce::MouseEvent& event)
{
    if (event.mods.isCommandDown())
    {
        if (auto noteIndex = noteAt(event.position))
        {
            const auto& note = document_.notes[static_cast<size_t>(*noteIndex)];
            const auto previewTime = xToTime(event.position.x);
            const auto canSplit = previewTime > note.start + kMinNoteLength && previewTime < note.end - kMinNoteLength;
            const auto changed = splitPreviewActive_ != canSplit
                || splitPreviewNoteId_ != note.id
                || std::abs(splitPreviewTime_ - previewTime) > 0.002;

            splitPreviewActive_ = canSplit;
            splitPreviewNoteId_ = note.id;
            splitPreviewTime_ = previewTime;
            setMouseCursor(knifeCursor());
            if (changed)
                repaint();
            return;
        }
    }

    if (splitPreviewActive_)
    {
        splitPreviewActive_ = false;
        repaint();
    }

    if (noteAt(event.position) || boundaryAt(event.position) || waveformPartBoundaryAt(event.position) || waveformPartNoteAt(event.position))
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    else
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void AnnotationEditorComponent::mouseExit(const juce::MouseEvent&)
{
    if (splitPreviewActive_)
    {
        splitPreviewActive_ = false;
        repaint();
    }

    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void AnnotationEditorComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (auto noteIndex = noteAt(event.position))
    {
        if (event.mods.isAltDown())
        {
            splitNoteAt(*noteIndex, xToTime(event.position.x));
            return;
        }

        selectNote(*noteIndex);
        editSelectedNoteLyric();
        return;
    }

    if (selectedTrack_ == SelectedTrack::lead && getEditorBounds().contains(event.getPosition()))
    {
        if (event.mods.isCommandDown() || event.mods.isPopupMenu())
            createNoteAt(event.position);
        else
            addBoundaryAt(xToTime(event.position.x));
    }
}

bool AnnotationEditorComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        deleteSelection();
        return true;
    }

    if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')
    {
        splitSelectedAtPlayhead();
        return true;
    }

    if (key.getTextCharacter() == 'm' || key.getTextCharacter() == 'M')
    {
        mergeSelectedWithNext();
        return true;
    }

    if (key == juce::KeyPress::upKey || key == juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0))
    {
        nudgeSelectedPitch(key.getModifiers().isShiftDown() ? 12 : 1);
        return true;
    }

    if (key == juce::KeyPress::downKey || key == juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0))
    {
        nudgeSelectedPitch(key.getModifiers().isShiftDown() ? -12 : -1);
        return true;
    }

    if (key == juce::KeyPress::leftKey)
    {
        nudgeSelectedTime(-0.01);
        return true;
    }

    if (key == juce::KeyPress::rightKey)
    {
        nudgeSelectedTime(0.01);
        return true;
    }

    if (key == juce::KeyPress::tabKey)
    {
        jumpToNextSuspicious(key.getModifiers().isShiftDown());
        return true;
    }

    return false;
}

void AnnotationEditorComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    auto scrollableBounds = getLocalBounds();
    scrollableBounds.removeFromTop(kTimelineHeight);
    scrollableBounds.removeFromBottom(kLyricsHeight);
    scrollableBounds.removeFromLeft(kTrackHeaderWidth);
    if ((! scrollableBounds.contains(event.getPosition()) && ! getTimelineBounds().contains(event.getPosition()))
        || document_.duration <= 0.0)
        return;

    const auto dominantDelta = std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;

    if (event.mods.isCommandDown() || event.mods.isCtrlDown())
    {
        const auto scale = std::exp(static_cast<double>(dominantDelta) * (wheel.isSmooth ? 2.2 : 0.35));
        zoomTimeAt(xToTime(event.position.x), scale);
        return;
    }

    if (event.mods.isAltDown())
    {
        const auto scale = std::exp(static_cast<double>(dominantDelta) * (wheel.isSmooth ? 2.2 : 0.35));
        zoomPitchAt(yToPitch(event.position.y), scale);
        return;
    }

    const auto secondsPerWheelUnit = (visibleEnd_ - visibleStart_) * (wheel.isSmooth ? 0.82 : 0.18);
    auto horizontalDelta = static_cast<double>(wheel.deltaX);
    if (event.mods.isShiftDown())
        horizontalDelta += static_cast<double>(wheel.deltaY);

    if (std::abs(horizontalDelta) > 0.0001)
        scrollTime(-horizontalDelta * secondsPerWheelUnit);

    if (! event.mods.isShiftDown() && std::abs(wheel.deltaY) > 0.0001f)
    {
        const auto semitonesPerWheelUnit = visibleSemitones_ * (wheel.isSmooth ? 0.18 : 0.06);
        scrollPitch(static_cast<double>(wheel.deltaY) * semitonesPerWheelUnit);
    }
}

void AnnotationEditorComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    auto zoomableBounds = getLocalBounds();
    zoomableBounds.removeFromTop(kTimelineHeight);
    zoomableBounds.removeFromBottom(kLyricsHeight);
    zoomableBounds.removeFromLeft(kTrackHeaderWidth);
    if ((! zoomableBounds.contains(event.getPosition()) && ! getTimelineBounds().contains(event.getPosition()))
        || scaleFactor <= 0.0f)
        return;

    if (event.mods.isAltDown())
        zoomPitchAt(yToPitch(event.position.y), scaleFactor);
    else
        zoomTimeAt(xToTime(event.position.x), scaleFactor);
}

juce::Rectangle<int> AnnotationEditorComponent::getTimelineBounds() const
{
    return getLocalBounds().removeFromTop(kTimelineHeight).withTrimmedLeft(kTrackHeaderWidth);
}

juce::Rectangle<int> AnnotationEditorComponent::getTrackOverviewBounds() const
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(kTimelineHeight);
    bounds.removeFromBottom(kLyricsHeight);

    auto height = 0;
    if (! document_.tempoSegments.empty() || ! document_.timeSignatures.empty() || ! document_.chords.empty())
        height += kMusicalContextHeight;
    if (document_.instrumentalFile.existsAsFile())
        height += kCompactTrackHeight;
    if (document_.audioFile.existsAsFile())
        height += kCompactTrackHeight;
    if (! document_.backingNotes.empty())
        height += kCompactTrackHeight;

    if (height <= 0)
        return {};

    return bounds.removeFromTop(juce::jmin(height, juce::jmax(0, bounds.getHeight() - 140)));
}

juce::Rectangle<int> AnnotationEditorComponent::getTrackHeaderBounds() const
{
    return getTrackOverviewBounds().removeFromLeft(kTrackHeaderWidth);
}

juce::Rectangle<int> AnnotationEditorComponent::getTrackLaneBounds() const
{
    auto bounds = getTrackOverviewBounds();
    bounds.removeFromLeft(kTrackHeaderWidth);
    return bounds;
}

juce::Rectangle<int> AnnotationEditorComponent::getMusicalContextOverviewBounds() const
{
    if (document_.tempoSegments.empty() && document_.timeSignatures.empty() && document_.chords.empty())
        return {};

    return getTrackOverviewBounds().removeFromTop(kMusicalContextHeight);
}

juce::Rectangle<int> AnnotationEditorComponent::getInstrumentalOverviewBounds() const
{
    if (! document_.instrumentalFile.existsAsFile())
        return {};

    auto bounds = getTrackOverviewBounds();
    if (! getMusicalContextOverviewBounds().isEmpty())
        bounds.removeFromTop(kMusicalContextHeight);
    return bounds.removeFromTop(kCompactTrackHeight);
}

juce::Rectangle<int> AnnotationEditorComponent::getLeadOverviewBounds() const
{
    if (! document_.audioFile.existsAsFile())
        return {};

    auto bounds = getTrackOverviewBounds();
    if (! getMusicalContextOverviewBounds().isEmpty())
        bounds.removeFromTop(kMusicalContextHeight);
    if (document_.instrumentalFile.existsAsFile())
        bounds.removeFromTop(kCompactTrackHeight);
    return bounds.removeFromTop(kCompactTrackHeight);
}

juce::Rectangle<int> AnnotationEditorComponent::getBackingOverviewBounds() const
{
    if (document_.backingNotes.empty())
        return {};

    auto bounds = getTrackOverviewBounds();
    if (! getMusicalContextOverviewBounds().isEmpty())
        bounds.removeFromTop(kMusicalContextHeight);
    if (document_.instrumentalFile.existsAsFile())
        bounds.removeFromTop(kCompactTrackHeight);
    if (document_.audioFile.existsAsFile())
        bounds.removeFromTop(kCompactTrackHeight);
    return bounds.removeFromTop(kCompactTrackHeight);
}

juce::Rectangle<int> AnnotationEditorComponent::getKeyboardBounds() const
{
    auto bounds = getEditorBounds();
    bounds.setX(0);
    bounds.setWidth(kKeyboardWidth);
    return bounds;
}

juce::Rectangle<int> AnnotationEditorComponent::getEditorBounds() const
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(kTimelineHeight);
    bounds.removeFromBottom(kLyricsHeight);
    bounds.removeFromTop(getTrackOverviewBounds().getHeight());
    bounds.removeFromLeft(kKeyboardWidth);
    return bounds;
}

juce::Rectangle<int> AnnotationEditorComponent::getVocalEditorBounds() const
{
    return selectedTrack_ == SelectedTrack::lead ? getEditorBounds() : juce::Rectangle<int> {};
}

juce::Rectangle<int> AnnotationEditorComponent::getBackingEditorBounds() const
{
    return selectedTrack_ == SelectedTrack::backing ? getEditorBounds() : juce::Rectangle<int> {};
}

juce::Rectangle<int> AnnotationEditorComponent::getInstrumentalHeaderBounds() const
{
    if (! document_.instrumentalFile.existsAsFile())
        return {};

    auto bounds = getLocalBounds();
    bounds.removeFromTop(kTimelineHeight);
    bounds.removeFromBottom(kLyricsHeight);
    auto header = bounds.removeFromLeft(kKeyboardWidth);
    return header.removeFromTop(instrumentalTrackHeight(document_));
}

juce::Rectangle<int> AnnotationEditorComponent::getInstrumentalTrackBounds() const
{
    if (! document_.instrumentalFile.existsAsFile())
        return {};

    auto bounds = getLocalBounds();
    bounds.removeFromTop(kTimelineHeight);
    bounds.removeFromBottom(kLyricsHeight);
    bounds.removeFromLeft(kKeyboardWidth);
    return bounds.removeFromTop(instrumentalTrackHeight(document_));
}

juce::Rectangle<int> AnnotationEditorComponent::getLyricsBounds() const
{
    auto bounds = getLocalBounds().removeFromBottom(kLyricsHeight);
    bounds.removeFromLeft(kTrackHeaderWidth);
    return bounds;
}

double AnnotationEditorComponent::xToTime(float x) const
{
    const auto bounds = getEditorBounds().toFloat();
    const auto proportion = (x - bounds.getX()) / juce::jmax(1.0f, bounds.getWidth());
    return juce::jlimit(0.0, document_.duration, visibleStart_ + static_cast<double>(proportion) * (visibleEnd_ - visibleStart_));
}

float AnnotationEditorComponent::timeToX(double time) const
{
    const auto bounds = getEditorBounds().toFloat();
    const auto proportion = (time - visibleStart_) / juce::jmax(0.001, visibleEnd_ - visibleStart_);
    return bounds.getX() + static_cast<float>(proportion) * bounds.getWidth();
}

int AnnotationEditorComponent::yToPitch(float y) const
{
    const auto bounds = getEditorBounds().toFloat();
    const auto topPitch = pitchCenter_ + visibleSemitones_ * 0.5;
    const auto proportion = (y - bounds.getY()) / juce::jmax(1.0f, bounds.getHeight());
    return juce::jlimit(0, 127, juce::roundToInt(topPitch - static_cast<double>(proportion) * visibleSemitones_));
}

float AnnotationEditorComponent::pitchToY(double pitch) const
{
    return pitchToYInBounds(pitch, getEditorBounds());
}

float AnnotationEditorComponent::pitchToYInBounds(double pitch, juce::Rectangle<int> bounds) const
{
    const auto boundsFloat = bounds.toFloat();
    const auto topPitch = pitchCenter_ + visibleSemitones_ * 0.5;
    const auto proportion = (topPitch - pitch) / visibleSemitones_;
    return boundsFloat.getY() + static_cast<float>(proportion) * boundsFloat.getHeight();
}

juce::Rectangle<float> AnnotationEditorComponent::noteBoundsFor(const NoteBlock& note) const
{
    return noteBoundsFor(note, getEditorBounds());
}

juce::Rectangle<float> AnnotationEditorComponent::noteBoundsFor(const NoteBlock& note, juce::Rectangle<int> bounds) const
{
    const auto laneHeight = juce::jmax(8.0f,
                                       static_cast<float>(bounds.getHeight())
                                           / static_cast<float>(visibleSemitones_));
    // Position the block at the detected pitch while the piano-roll lane remains
    // the quantized target, matching the visual offset used by Flex Pitch.
    const auto centerY = pitchToYInBounds(note.pitchExact, bounds);
    return { timeToX(note.start),
             centerY - laneHeight * 0.5f,
             timeToX(note.end) - timeToX(note.start),
             laneHeight };
}

void AnnotationEditorComponent::scrollTime(double deltaSeconds)
{
    visibleStart_ += deltaSeconds;
    visibleEnd_ += deltaSeconds;
    clampView();
    repaint();
}

void AnnotationEditorComponent::scrollPitch(double deltaSemitones)
{
    const auto halfRange = visibleSemitones_ * 0.5;
    pitchCenter_ = juce::jlimit(halfRange, 127.0 - halfRange, pitchCenter_ + deltaSemitones);
    repaint();
}

void AnnotationEditorComponent::zoomTimeAt(double pivotTime, double scaleFactor)
{
    scaleFactor = juce::jlimit(0.15, 8.0, scaleFactor);
    const auto oldWindow = juce::jmax(0.1, visibleEnd_ - visibleStart_);
    const auto newWindow = juce::jlimit(0.08, juce::jmax(0.1, document_.duration), oldWindow / scaleFactor);
    const auto pivotRatio = (pivotTime - visibleStart_) / oldWindow;
    visibleStart_ = pivotTime - pivotRatio * newWindow;
    visibleEnd_ = visibleStart_ + newWindow;
    clampView();
    repaint();
}

void AnnotationEditorComponent::zoomPitchAt(double pivotPitch, double scaleFactor)
{
    scaleFactor = juce::jlimit(0.15, 8.0, scaleFactor);
    const auto oldRange = visibleSemitones_;
    const auto newRange = juce::jlimit(8.0, 88.0, oldRange / scaleFactor);
    visibleSemitones_ = newRange;
    pitchCenter_ = pivotPitch + (pitchCenter_ - pivotPitch) * (newRange / oldRange);
    const auto halfRange = visibleSemitones_ * 0.5;
    pitchCenter_ = juce::jlimit(halfRange, 127.0 - halfRange, pitchCenter_);
    repaint();
}

std::optional<int> AnnotationEditorComponent::noteAt(juce::Point<float> position) const
{
    if (selectedTrack_ != SelectedTrack::lead)
        return std::nullopt;

    for (int i = static_cast<int>(document_.notes.size()) - 1; i >= 0; --i)
    {
        const auto& note = document_.notes[static_cast<size_t>(i)];
        if (noteBoundsFor(note).expanded(0.0f, 2.0f).contains(position))
            return i;
    }

    return std::nullopt;
}

std::optional<int> AnnotationEditorComponent::boundaryAt(juce::Point<float> position) const
{
    if (selectedTrack_ != SelectedTrack::lead)
        return std::nullopt;

    const auto bounds = getEditorBounds().toFloat();
    if (! bounds.contains(position))
        return std::nullopt;

    for (int i = static_cast<int>(document_.boundaries.size()) - 1; i >= 0; --i)
        if (std::abs(timeToX(document_.boundaries[static_cast<size_t>(i)].time) - position.x) <= kBoundaryHitPixels)
            return i;

    return std::nullopt;
}

std::optional<int> AnnotationEditorComponent::waveformPartBoundaryAt(juce::Point<float> position) const
{
    if (selectedTrack_ != SelectedTrack::lead)
        return std::nullopt;

    if (! waveformContains(position))
        return std::nullopt;

    const auto time = xToTime(position.x);
    std::vector<int> boundaryIndices;
    boundaryIndices.reserve(document_.boundaries.size());
    for (int i = 0; i < static_cast<int>(document_.boundaries.size()); ++i)
    {
        const auto& boundary = document_.boundaries[static_cast<size_t>(i)];
        if (boundary.kind == BoundaryKind::ignore)
            continue;

        if (boundary.time >= 0.0 && boundary.time <= document_.duration)
            boundaryIndices.push_back(i);
    }

    std::sort(boundaryIndices.begin(), boundaryIndices.end(), [this](int a, int b)
    {
        return document_.boundaries[static_cast<size_t>(a)].time < document_.boundaries[static_cast<size_t>(b)].time;
    });

    for (int i = 0; i < static_cast<int>(boundaryIndices.size()); ++i)
    {
        const auto boundaryIndex = boundaryIndices[static_cast<size_t>(i)];
        const auto start = document_.boundaries[static_cast<size_t>(boundaryIndex)].time;
        const auto end = i + 1 < static_cast<int>(boundaryIndices.size())
            ? document_.boundaries[static_cast<size_t>(boundaryIndices[static_cast<size_t>(i + 1)])].time
            : document_.duration;

        if (time >= start && time < end)
            return boundaryIndex;
    }

    return std::nullopt;
}

std::optional<int> AnnotationEditorComponent::waveformPartNoteAt(juce::Point<float> position) const
{
    if (selectedTrack_ != SelectedTrack::lead)
        return std::nullopt;

    if (! waveformContains(position))
        return std::nullopt;

    const auto time = xToTime(position.x);
    for (int i = static_cast<int>(document_.notes.size()) - 1; i >= 0; --i)
    {
        const auto& note = document_.notes[static_cast<size_t>(i)];
        if (time >= note.start && time <= note.end)
            return i;
    }

    return std::nullopt;
}

bool AnnotationEditorComponent::waveformContains(juce::Point<float> position) const
{
    const auto bounds = getVocalEditorBounds();
    const auto waveformBounds = bounds.reduced(0, 8).toFloat();
    if (! waveformBounds.contains(position) || ! document_.audioFile.existsAsFile())
        return false;

    const auto duration = juce::jmax(0.001, visibleEnd_ - visibleStart_);
    const auto secondsPerPixel = duration / juce::jmax(1.0f, waveformBounds.getWidth());
    const auto time = xToTime(position.x);
    const auto channels = juce::jmax(1, thumbnail_.getNumChannels());
    const auto channelHeight = waveformBounds.getHeight() / static_cast<float>(channels);
    const auto channelIndex = juce::jlimit(0, channels - 1, static_cast<int>((position.y - waveformBounds.getY()) / juce::jmax(1.0f, channelHeight)));

    float minimum = 0.0f;
    float maximum = 0.0f;
    thumbnail_.getApproximateMinMax(juce::jmax(0.0, time - secondsPerPixel * 1.5),
                                    juce::jmin(document_.duration, time + secondsPerPixel * 1.5),
                                    channelIndex,
                                    minimum,
                                    maximum);

    const auto channelY = waveformBounds.getY() + static_cast<float>(channelIndex) * channelHeight;
    const auto centerY = channelY + channelHeight * 0.5f;
    const auto halfHeight = channelHeight * 0.5f * 0.88f;
    const auto top = centerY - maximum * halfHeight;
    const auto bottom = centerY - minimum * halfHeight;
    return position.y >= top - 6.0f && position.y <= bottom + 6.0f;
}

void AnnotationEditorComponent::selectNote(int noteIndex)
{
    selectedNoteId_ = document_.notes[static_cast<size_t>(noteIndex)].id;
    selectedBoundaryId_.clear();
    if (listener_ != nullptr)
        listener_->selectionChanged(selectedNoteId_, selectedBoundaryId_);
}

void AnnotationEditorComponent::selectBoundary(int boundaryIndex)
{
    selectedBoundaryId_ = document_.boundaries[static_cast<size_t>(boundaryIndex)].id;
    selectedNoteId_.clear();
    if (listener_ != nullptr)
        listener_->selectionChanged(selectedNoteId_, selectedBoundaryId_);
}

void AnnotationEditorComponent::clearSelection()
{
    selectedNoteId_.clear();
    selectedBoundaryId_.clear();
    if (listener_ != nullptr)
        listener_->selectionChanged(selectedNoteId_, selectedBoundaryId_);
}

void AnnotationEditorComponent::notifyChanged()
{
    if (listener_ != nullptr)
        listener_->annotationChanged();
    repaint();
}

void AnnotationEditorComponent::clampView()
{
    const auto duration = juce::jmax(1.0, document_.duration);
    const auto window = juce::jlimit(0.1, duration, visibleEnd_ - visibleStart_);
    visibleStart_ = juce::jlimit(0.0, juce::jmax(0.0, duration - window), visibleStart_);
    visibleEnd_ = visibleStart_ + window;
}

void AnnotationEditorComponent::sortNotes()
{
    std::sort(document_.notes.begin(), document_.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
}

void AnnotationEditorComponent::editSelectedNoteLyric()
{
    auto index = document_.findNoteIndex(selectedNoteId_);
    if (! index)
        return;

    const auto noteId = selectedNoteId_;
    const auto lyric = document_.notes[static_cast<size_t>(*index)].lyric;
    auto* window = new juce::AlertWindow("Note lyric", "Text attached to " + noteId, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("lyric", lyric);
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->centreAroundComponent(this, 360, 160);
    window->enterModalState(true,
                            juce::ModalCallbackFunction::create([this, noteId, window](int result)
                            {
                                if (result == 1)
                                {
                                    if (auto noteToEdit = document_.findNoteIndex(noteId))
                                    {
                                        if (listener_ != nullptr)
                                            listener_->beginUndoableAction();
                                        document_.notes[static_cast<size_t>(*noteToEdit)].lyric = window->getTextEditorContents("lyric");
                                        notifyChanged();
                                    }
                                }

                                delete window;
                            }),
                            false);
}

void AnnotationEditorComponent::addBoundaryAt(double time)
{
    BoundaryMarker boundary;
    if (listener_ != nullptr)
        listener_->beginUndoableAction();
    boundary.id = document_.nextBoundaryId();
    boundary.time = juce::jlimit(0.0, document_.duration, time);
    document_.boundaries.push_back(std::move(boundary));
    selectBoundary(static_cast<int>(document_.boundaries.size()) - 1);
    notifyChanged();
}

void AnnotationEditorComponent::createNoteAt(juce::Point<float> position)
{
    const auto start = xToTime(position.x);
    if (listener_ != nullptr)
        listener_->beginUndoableAction();
    NoteBlock note;
    note.id = document_.nextNoteId();
    note.start = start;
    note.end = juce::jmin(document_.duration, start + 0.5);
    note.pitch = yToPitch(position.y);
    note.pitchExact = static_cast<double>(note.pitch);
    note.voicedStart = note.start;
    note.voicedEnd = note.end;
    note.flags.add("manual");
    note.curve.push_back({ note.start, static_cast<double>(note.pitch), 1.0 });
    note.curve.push_back({ note.end, static_cast<double>(note.pitch), 1.0 });
    const auto newNoteId = note.id;
    document_.notes.push_back(std::move(note));
    sortNotes();
    if (auto index = document_.findNoteIndex(newNoteId))
        selectNote(*index);
    notifyChanged();
}

void AnnotationEditorComponent::showNoteContextMenu(int noteIndex)
{
    if (noteIndex < 0 || noteIndex >= static_cast<int>(document_.notes.size()))
        return;

    const auto noteId = document_.notes[static_cast<size_t>(noteIndex)].id;
    juce::PopupMenu menu;
    menu.addItem(1, "Recalculate note pitch");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                        [this, noteId](int result)
                        {
                            if (result != 1)
                                return;

                            if (auto index = document_.findNoteIndex(noteId))
                            {
                                if (listener_ != nullptr)
                                {
                                    juce::StringArray ids;
                                    ids.add(noteId);
                                    listener_->notesRecalculationRequested(ids);
                                }
                                else
                                {
                                    recalculateNotePitchFromCurve(document_.notes[static_cast<size_t>(*index)]);
                                    notifyChanged();
                                }
                            }
                        });
}

double AnnotationEditorComponent::noteMidiAtTime(const NoteBlock& note, double time)
{
    if (note.curve.empty())
        return static_cast<double>(note.pitch);

    auto curve = note.curve;
    std::sort(curve.begin(), curve.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    if (time <= curve.front().time)
        return curve.front().midi;
    if (time >= curve.back().time)
        return curve.back().midi;

    for (int i = 1; i < static_cast<int>(curve.size()); ++i)
    {
        const auto& previous = curve[static_cast<size_t>(i - 1)];
        const auto& next = curve[static_cast<size_t>(i)];
        if (time <= next.time)
        {
            const auto proportion = juce::jlimit(0.0, 1.0, (time - previous.time) / juce::jmax(0.001, next.time - previous.time));
            return previous.midi + (next.midi - previous.midi) * proportion;
        }
    }

    return curve.back().midi;
}

void AnnotationEditorComponent::recalculateNotePitchFromCurve(NoteBlock& note)
{
    struct PitchSample
    {
        double midi = 60.0;
        double weight = 1.0;
    };

    std::vector<PitchSample> samples;
    samples.reserve(note.curve.size());
    const auto duration = note.end - note.start;
    const auto trim = juce::jmin(0.08, duration * 0.22);
    const auto useTrimmedCore = duration >= 0.18 && trim > 0.0;
    for (const auto& point : note.curve)
    {
        if (point.time < note.start - 0.001 || point.time > note.end + 0.001)
            continue;
        if (useTrimmedCore && (point.time < note.start + trim || point.time > note.end - trim))
            continue;
        if (! std::isfinite(point.midi) || point.midi < 0.0 || point.midi > 127.0)
            continue;

        samples.push_back({ point.midi, std::pow(juce::jlimit(0.05, 1.0, point.confidence), 1.4) });
    }

    if (samples.size() < 2)
    {
        samples.clear();
        for (const auto& point : note.curve)
        {
            if (point.time >= note.start - 0.001
                && point.time <= note.end + 0.001
                && std::isfinite(point.midi)
                && point.midi >= 0.0
                && point.midi <= 127.0)
            {
                samples.push_back({ point.midi, std::pow(juce::jlimit(0.05, 1.0, point.confidence), 1.4) });
            }
        }
    }

    if (samples.empty())
    {
        note.pitchExact = static_cast<double>(note.pitch);
        note.curve.clear();
        note.curve.push_back({ note.start, static_cast<double>(note.pitch), 0.45 });
        note.curve.push_back({ note.end, static_cast<double>(note.pitch), 0.45 });
        return;
    }

    constexpr auto binWidth = 0.25;
    std::map<int, double> binWeights;
    for (const auto& sample : samples)
        binWeights[static_cast<int>(std::floor(sample.midi / binWidth))] += sample.weight;

    auto bestBin = binWeights.begin()->first;
    auto bestWeight = -1.0;
    for (const auto& [bin, _] : binWeights)
    {
        auto total = 0.0;
        for (int neighbour = bin - 1; neighbour <= bin + 1; ++neighbour)
            if (auto found = binWeights.find(neighbour); found != binWeights.end())
                total += found->second;

        if (total > bestWeight)
        {
            bestWeight = total;
            bestBin = bin;
        }
    }

    auto totalWeight = 0.0;
    for (const auto& sample : samples)
        totalWeight += sample.weight;

    const auto binCenter = (static_cast<double>(bestBin) + 0.5) * binWidth;
    std::vector<PitchSample> cluster;
    cluster.reserve(samples.size());
    auto clusterWeight = 0.0;
    for (const auto& sample : samples)
    {
        if (std::abs(sample.midi - binCenter) <= 0.58)
        {
            cluster.push_back(sample);
            clusterWeight += sample.weight;
        }
    }

    if (cluster.size() >= 2 && clusterWeight >= totalWeight * 0.30)
        samples = std::move(cluster);

    std::sort(samples.begin(), samples.end(), [](const auto& a, const auto& b) { return a.midi < b.midi; });
    totalWeight = 0.0;
    for (const auto& sample : samples)
        totalWeight += sample.weight;

    auto representative = samples[samples.size() / 2].midi;
    auto cumulative = 0.0;
    for (const auto& sample : samples)
    {
        cumulative += sample.weight;
        if (cumulative >= totalWeight * 0.5)
        {
            representative = sample.midi;
            break;
        }
    }

    note.pitchExact = juce::jlimit(0.0, 127.0, representative);
    note.pitch = juce::jlimit(0, 127, static_cast<int>(std::round(note.pitchExact)));
}

void AnnotationEditorComponent::splitNoteAt(int noteIndex, double splitTime)
{
    auto& note = document_.notes[static_cast<size_t>(noteIndex)];
    if (splitTime <= note.start + kMinNoteLength || splitTime >= note.end - kMinNoteLength)
        return;

    if (listener_ != nullptr)
        listener_->beginUndoableAction();
    const auto leftId = note.id;
    const auto splitMidi = noteMidiAtTime(note, splitTime);
    auto right = note;
    right.id = document_.nextNoteId();
    right.start = splitTime;
    right.voicedStart = juce::jmax(splitTime, right.voicedStart);
    if (note.syllableId.isNotEmpty())
    {
        right.lyric.clear();
        right.flags.addIfNotAlreadyThere("melisma_continuation");
        right.flags.addIfNotAlreadyThere("legato_from_previous");
        note.flags.addIfNotAlreadyThere("legato_to_next");
    }

    std::vector<PitchCurvePoint> leftCurve;
    std::vector<PitchCurvePoint> rightCurve;
    for (const auto& point : note.curve)
    {
        if (point.time < splitTime)
            leftCurve.push_back(point);
        else
            rightCurve.push_back(point);
    }

    note.end = splitTime;
    note.voicedEnd = juce::jmin(splitTime, note.voicedEnd);
    leftCurve.push_back({ splitTime, splitMidi, 0.65 });
    rightCurve.insert(rightCurve.begin(), { splitTime, splitMidi, 0.65 });
    note.curve = std::move(leftCurve);
    right.curve = std::move(rightCurve);
    recalculateNotePitchFromCurve(note);
    recalculateNotePitchFromCurve(right);
    const auto rightId = right.id;

    document_.notes.push_back(std::move(right));
    sortNotes();
    notifyChanged();

    if (listener_ != nullptr)
    {
        juce::StringArray ids;
        ids.add(leftId);
        ids.add(rightId);
        listener_->notesRecalculationRequested(ids);
    }
}

void AnnotationEditorComponent::drawPianoKeyboard(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    g.setColour(juce::Colour(0xff101318));
    g.fillRect(bounds);
    const auto lowPitch = static_cast<int>(std::floor(pitchCenter_ - visibleSemitones_ * 0.5));
    const auto highPitch = static_cast<int>(std::ceil(pitchCenter_ + visibleSemitones_ * 0.5));

    for (int pitch = lowPitch; pitch <= highPitch; ++pitch)
    {
        const auto y1 = pitchToY(pitch + 0.5);
        const auto y2 = pitchToY(pitch - 0.5);
        const juce::Rectangle<float> key(static_cast<float>(bounds.getX()), y1, static_cast<float>(bounds.getWidth()), y2 - y1);
        g.setColour(isBlackKey(pitch) ? juce::Colour(0xff252a32) : juce::Colour(0xffdde1e7));
        g.fillRect(key);
        g.setColour(juce::Colour(0xff454b55));
        g.drawRect(key);

        if (pitch % 12 == 0 && key.getHeight() > 10.0f)
        {
            g.setColour(isBlackKey(pitch) ? juce::Colours::white : juce::Colours::black);
            g.drawText("C" + juce::String((pitch / 12) - 1), key.reduced(4.0f, 0.0f), juce::Justification::centredLeft);
        }
    }
}

void AnnotationEditorComponent::drawTimeline(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    g.setColour(juce::Colour(0xff222731));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xff707783));

    const auto firstSecond = static_cast<int>(std::floor(visibleStart_));
    const auto lastSecond = static_cast<int>(std::ceil(visibleEnd_));
    for (int second = firstSecond; second <= lastSecond; ++second)
    {
        const auto x = timeToX(second);
        g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
        g.drawText(juce::String(second) + "s", juce::roundToInt(x) + 3, bounds.getY() + 3, 42, 18, juce::Justification::left);
    }
}

void AnnotationEditorComponent::drawTrackOverview(juce::Graphics& g)
{
    const auto bounds = getTrackOverviewBounds();
    if (bounds.isEmpty())
        return;

    g.setColour(juce::Colour(0xff111821));
    g.fillRect(bounds);

    if (auto row = getMusicalContextOverviewBounds(); ! row.isEmpty())
    {
        auto header = row.removeFromLeft(kTrackHeaderWidth);
        g.setColour(juce::Colour(0xff242b34));
        g.fillRect(header);
        g.setColour(juce::Colour(0xff0f1720));
        g.drawRect(header);

        auto labels = header.reduced(10, 0);
        auto tempoLabel = labels.removeFromTop(36);
        auto signatureLabel = labels.removeFromTop(32);
        auto chordLabel = labels;

        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.setColour(juce::Colours::white.withAlpha(0.78f));
        g.drawText("Tempo", tempoLabel, juce::Justification::centredLeft);
        g.drawText("Signature", signatureLabel, juce::Justification::centredLeft);
        g.drawText("Chord", chordLabel, juce::Justification::centredLeft);

        drawMusicalContext(g, row);
    }

    if (auto row = getInstrumentalOverviewBounds(); ! row.isEmpty())
    {
        drawTrackHeader(g, row.removeFromLeft(kTrackHeaderWidth), "Instrumental", SelectedTrack::instrumental);
        drawCompactWaveformTrack(g, row.reduced(0, 4), instrumentalThumbnail_, juce::Colour(0xffd1d5db), "Instrumental");
    }

    if (auto row = getLeadOverviewBounds(); ! row.isEmpty())
    {
        drawTrackHeader(g, row.removeFromLeft(kTrackHeaderWidth), "Voice Main", SelectedTrack::lead);
        drawCompactWaveformTrack(g, row.reduced(0, 4), thumbnail_, juce::Colour(0xff7da2ff), "Voice Main");
    }

    if (auto row = getBackingOverviewBounds(); ! row.isEmpty())
    {
        drawTrackHeader(g, row.removeFromLeft(kTrackHeaderWidth), document_.backingStyleName.isNotEmpty() ? document_.backingStyleName : "Back Vocal", SelectedTrack::backing);
        if (document_.backingAudioFile.existsAsFile())
            drawCompactWaveformTrack(g, row.reduced(0, 4), backingThumbnail_, juce::Colour(0xff5eead4), "Back Vocal Audio");
        else
            drawCompactNoteTrack(g, row.reduced(0, 4), document_.backingNotes, juce::Colour(0xff5eead4), "Back Vocal");
    }

    auto laneBounds = bounds;
    laneBounds.removeFromLeft(kTrackHeaderWidth);
    if (! laneBounds.isEmpty())
    {
        juce::Graphics::ScopedSaveState clipState(g);
        g.reduceClipRegion(laneBounds);
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.drawVerticalLine(juce::roundToInt(timeToX(playheadTime_)),
                           static_cast<float>(laneBounds.getY()),
                           static_cast<float>(laneBounds.getBottom()));
    }
}

void AnnotationEditorComponent::drawTrackHeader(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title, SelectedTrack track) const
{
    const auto selected = selectedTrack_ == track;
    g.setColour(selected ? juce::Colour(0xff2f3b4d) : juce::Colour(0xff242b34));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xff0f1720));
    g.drawRect(bounds);

    auto area = bounds.reduced(8, 6);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.setColour(juce::Colours::white.withAlpha(selected ? 0.95f : 0.78f));
    g.drawText(title, area.removeFromTop(20), juce::Justification::centredLeft);

    auto buttons = area.removeFromTop(24);
    const auto drawButton = [&g](juce::Rectangle<int> button, const juce::String& text, bool active)
    {
        g.setColour(active ? juce::Colour(0xff60a5fa) : juce::Colour(0xff3f4650));
        g.fillRoundedRectangle(button.toFloat(), 3.0f);
        g.setColour(juce::Colours::white.withAlpha(active ? 0.96f : 0.72f));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(text, button, juce::Justification::centred);
    };

    const auto muted = track == SelectedTrack::instrumental ? instrumentalMuted_
                     : track == SelectedTrack::lead ? leadMuted_
                     : backingMuted_;
    const auto solo = track == SelectedTrack::instrumental ? instrumentalSolo_
                    : track == SelectedTrack::lead ? leadSolo_
                    : backingSolo_;
    drawButton(buttons.removeFromLeft(26), "M", muted);
    buttons.removeFromLeft(4);
    drawButton(buttons.removeFromLeft(26), "S", solo);
}

void AnnotationEditorComponent::drawCompactWaveformTrack(juce::Graphics& g,
                                                         juce::Rectangle<int> bounds,
                                                         juce::AudioThumbnail& thumbnail,
                                                         juce::Colour colour,
                                                         const juce::String& label)
{
    if (bounds.isEmpty())
        return;

    g.setColour(juce::Colour(0xff15202b));
    g.fillRect(bounds);
    drawMusicalGrid(g, bounds);
    g.setColour(colour.withAlpha(0.58f));
    thumbnail.drawChannels(g, bounds.reduced(0, 5), visibleStart_, visibleEnd_, 0.82f);
    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText(label, bounds.reduced(8, 2), juce::Justification::topRight);
}

void AnnotationEditorComponent::drawCompactNoteTrack(juce::Graphics& g,
                                                     juce::Rectangle<int> bounds,
                                                     const std::vector<NoteBlock>& notes,
                                                     juce::Colour colour,
                                                     const juce::String& label) const
{
    if (bounds.isEmpty())
        return;

    g.setColour(juce::Colour(0xff15202b));
    g.fillRect(bounds);
    drawMusicalGrid(g, bounds);

    int low = 127;
    int high = 0;
    for (const auto& note : notes)
    {
        low = juce::jmin(low, note.pitch);
        high = juce::jmax(high, note.pitch);
    }
    if (low > high)
    {
        low = 48;
        high = 72;
    }
    const auto range = juce::jmax(6, high - low + 2);

    for (const auto& note : notes)
    {
        if (note.end < visibleStart_ || note.start > visibleEnd_)
            continue;

        const auto x = timeToX(note.start);
        const auto right = timeToX(note.end);
        const auto yNorm = 1.0f - static_cast<float>(note.pitch - low + 1) / static_cast<float>(range);
        const auto y = static_cast<float>(bounds.getY()) + 8.0f + yNorm * static_cast<float>(juce::jmax(1, bounds.getHeight() - 18));
        juce::Rectangle<float> rect(x, y, juce::jmax(2.0f, right - x), 8.0f);
        g.setColour(colour.withAlpha(0.82f));
        g.fillRoundedRectangle(rect, 2.5f);
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.drawRoundedRectangle(rect, 2.5f, 1.0f);
    }

    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText(label, bounds.reduced(8, 2), juce::Justification::topRight);
}

void AnnotationEditorComponent::drawPianoRollBackground(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    g.setColour(juce::Colour(0xff1d222b));
    g.fillRect(bounds);

    const auto lowPitch = static_cast<int>(std::floor(pitchCenter_ - visibleSemitones_ * 0.5));
    const auto highPitch = static_cast<int>(std::ceil(pitchCenter_ + visibleSemitones_ * 0.5));
    for (int pitch = lowPitch; pitch <= highPitch; ++pitch)
    {
        const auto top = pitchToY(pitch + 0.5);
        const auto bottom = pitchToY(pitch - 0.5);
        const juce::Rectangle<float> lane(static_cast<float>(bounds.getX()),
                                          top,
                                          static_cast<float>(bounds.getWidth()),
                                          bottom - top);
        g.setColour(isBlackKey(pitch) ? juce::Colour(0xff181d25) : juce::Colour(0xff20262f));
        g.fillRect(lane);
        g.setColour(juce::Colour(0xff353c47));
        g.drawHorizontalLine(juce::roundToInt(bottom),
                             static_cast<float>(bounds.getX()),
                             static_cast<float>(bounds.getRight()));
    }

    drawMusicalGrid(g, bounds);
    g.setColour(juce::Colours::white.withAlpha(0.8f));
    g.drawVerticalLine(juce::roundToInt(timeToX(playheadTime_)), static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
}

void AnnotationEditorComponent::drawInstrumentalHeader(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    g.setColour(juce::Colour(0xff101318));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xff303846));
    g.drawRect(bounds);

    auto area = bounds;
    if (! document_.tempoSegments.empty() || ! document_.timeSignatures.empty() || ! document_.chords.empty())
    {
        auto tempoRow = area.removeFromTop(36);
        auto signatureRow = area.removeFromTop(32);
        auto chordRow = area.removeFromTop(kMusicalContextHeight - 68);

        const auto drawLabel = [&g](juce::Rectangle<int> row, const juce::String& text)
        {
            g.setFont(13.0f);
            g.setColour(juce::Colour(0xffe5e7eb).withAlpha(0.92f));
            g.drawText(text, row.reduced(8, 0), juce::Justification::centredLeft);
        };

        g.setColour(juce::Colour(0xff111827));
        g.fillRect(tempoRow);
        g.setColour(juce::Colour(0xff1f2937));
        g.fillRect(signatureRow);
        g.setColour(juce::Colour(0xff111827));
        g.fillRect(chordRow);

        drawLabel(tempoRow, "Tempo");
        drawLabel(signatureRow, "Signature");
        drawLabel(chordRow, "Chord");

        g.setColour(juce::Colour(0xff334155));
        g.drawHorizontalLine(tempoRow.getBottom(), static_cast<float>(bounds.getX()), static_cast<float>(bounds.getRight()));
        g.drawHorizontalLine(signatureRow.getBottom(), static_cast<float>(bounds.getX()), static_cast<float>(bounds.getRight()));
    }

    g.setFont(13.0f);
    g.setColour(juce::Colour(0xffe5e7eb).withAlpha(0.86f));
    g.drawText("Inst", area.reduced(8, 0), juce::Justification::centredLeft);
}

void AnnotationEditorComponent::drawMusicalGrid(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    const auto beats = beatTimesFor(document_, visibleStart_, visibleEnd_);
    for (size_t i = 0; i < beats.size(); ++i)
    {
        const auto time = beats[i];
        const auto x = juce::roundToInt(timeToX(time));
        const auto numerator = numeratorAtTime(document_, time);
        const auto isBar = numerator > 0 && static_cast<int>(i) % numerator == 0;
        g.setColour(isBar ? juce::Colour(0xffa78bfa).withAlpha(0.30f)
                          : juce::Colour(0xffcbd5e1).withAlpha(0.13f));
        g.drawVerticalLine(x, static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
    }
}

void AnnotationEditorComponent::drawMusicalContext(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    auto rows = bounds;
    auto tempoRow = rows.removeFromTop(36);
    auto signatureRow = rows.removeFromTop(32);
    auto chordRow = rows;

    g.setColour(juce::Colour(0xff111827));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xff334155));
    g.drawHorizontalLine(tempoRow.getBottom(), static_cast<float>(bounds.getX()), static_cast<float>(bounds.getRight()));
    g.drawHorizontalLine(signatureRow.getBottom(), static_cast<float>(bounds.getX()), static_cast<float>(bounds.getRight()));

    for (const auto& tempo : document_.tempoSegments)
    {
        if (tempo.end <= visibleStart_ || tempo.start >= visibleEnd_)
            continue;

        const auto x = juce::roundToInt(timeToX(juce::jmax(tempo.start, visibleStart_)));
        const auto right = juce::roundToInt(timeToX(juce::jmin(tempo.end, visibleEnd_)));
        if (right <= x)
            continue;

        const auto rect = juce::Rectangle<int>(x, tempoRow.getY() + 7, right - x, tempoRow.getHeight() - 12);
        g.setColour(juce::Colour(0xff2563eb).withAlpha(0.26f));
        g.fillRect(rect);
        g.setColour(juce::Colour(0xff60a5fa).withAlpha(0.9f));
        g.drawRect(rect);
        g.drawText(juce::String(tempo.bpm, 0), rect.reduced(5, 0), juce::Justification::centredLeft);
    }

    for (const auto& signature : document_.timeSignatures)
    {
        if (signature.end <= visibleStart_ || signature.start >= visibleEnd_)
            continue;

        const auto x = juce::roundToInt(timeToX(juce::jmax(signature.start, visibleStart_)));
        const auto right = juce::roundToInt(timeToX(juce::jmin(signature.end, visibleEnd_)));
        if (right <= x)
            continue;

        const auto rect = juce::Rectangle<int>(x, signatureRow.getY(), right - x, signatureRow.getHeight());
        g.setColour(juce::Colour(0xff475569).withAlpha(0.55f));
        g.fillRect(rect);
        g.setColour(juce::Colour(0xffe5e7eb));
        g.drawText(juce::String(signature.numerator) + "/" + juce::String(signature.denominator),
                   rect.reduced(6, 0),
                   juce::Justification::centredLeft);
    }

    for (const auto& chord : document_.chords)
    {
        if (chord.end <= visibleStart_ || chord.start >= visibleEnd_)
            continue;

        const auto x = juce::roundToInt(timeToX(juce::jmax(chord.start, visibleStart_)));
        const auto right = juce::roundToInt(timeToX(juce::jmin(chord.end, visibleEnd_)));
        if (right <= x)
            continue;

        const auto rect = juce::Rectangle<int>(x, chordRow.getY() + 4, right - x, chordRow.getHeight() - 8);
        g.setColour(juce::Colour(0xfff8fafc).withAlpha(0.70f));
        g.fillRoundedRectangle(rect.toFloat(), 3.0f);
        g.setColour(juce::Colour(0xff1f2937));
        g.drawText(chord.name, rect.reduced(7, 0), juce::Justification::centredLeft);
    }

    drawMusicalGrid(g, bounds);
}

void AnnotationEditorComponent::drawWaveform(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    if (bounds.isEmpty())
        return;

    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    g.setColour(juce::Colour(0xff1d222b));
    g.fillRect(bounds);

    const auto lowPitch = static_cast<int>(std::floor(pitchCenter_ - visibleSemitones_ * 0.5));
    const auto highPitch = static_cast<int>(std::ceil(pitchCenter_ + visibleSemitones_ * 0.5));
    for (int pitch = lowPitch; pitch <= highPitch; ++pitch)
    {
        const auto top = pitchToY(pitch + 0.5);
        const auto bottom = pitchToY(pitch - 0.5);
        const juce::Rectangle<float> lane(static_cast<float>(bounds.getX()),
                                          top,
                                          static_cast<float>(bounds.getWidth()),
                                          bottom - top);
        g.setColour(isBlackKey(pitch) ? juce::Colour(0xff181d25) : juce::Colour(0xff20262f));
        g.fillRect(lane);
        g.setColour(juce::Colour(0xff353c47));
        g.drawHorizontalLine(juce::roundToInt(bottom),
                             static_cast<float>(bounds.getX()),
                             static_cast<float>(bounds.getRight()));
    }

    drawMusicalGrid(g, bounds);

    if (document_.audioFile.existsAsFile())
    {
        const auto waveformBounds = bounds.reduced(0, 8);

        g.setColour(juce::Colour(0xff7da2ff).withAlpha(0.2f));
        thumbnail_.drawChannels(g, waveformBounds, visibleStart_, visibleEnd_, 0.56f);

        const auto segmentStarts = waveformSegmentStarts(document_);

        for (size_t i = 0; i < segmentStarts.size(); ++i)
        {
            const auto start = segmentStarts[i].time;
            const auto end = i + 1 < segmentStarts.size() ? segmentStarts[i + 1].time : document_.duration;
            if (end <= visibleStart_ || start >= visibleEnd_ || end <= start)
                continue;

            const auto clippedStart = juce::jmax(start, visibleStart_);
            const auto clippedEnd = juce::jmin(end, visibleEnd_);
            const auto x = juce::roundToInt(timeToX(clippedStart));
            const auto right = juce::roundToInt(timeToX(clippedEnd));
            if (right <= x)
                continue;

            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(juce::Rectangle<int>(x, waveformBounds.getY(),
                                                    right - x, waveformBounds.getHeight()));

            const auto segmentColour = waveformColourFor(segmentStarts[i].kind, i);
            const auto isBreath = segmentStarts[i].kind == BoundaryKind::breath;
            g.setColour(segmentColour.withAlpha(isBreath ? 0.16f : 0.10f));
            g.fillRect(waveformBounds);
            g.setColour(segmentColour.withAlpha(isBreath ? 0.62f : 0.2f));
            thumbnail_.drawChannels(g, waveformBounds, visibleStart_, visibleEnd_, 0.88f);
        }

        drawSelectedWaveformBorder(g, waveformBounds);
    }
    else
    {
        g.setColour(juce::Colour(0xff858c98));
        g.drawText("Open a WAV, AIFF, or FLAC file", bounds, juce::Justification::centred);
    }

    g.setColour(juce::Colours::white.withAlpha(0.8f));
    g.drawVerticalLine(juce::roundToInt(timeToX(playheadTime_)), static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
}

void AnnotationEditorComponent::drawInstrumentalTrack(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    if (bounds.isEmpty())
        return;

    g.setColour(juce::Colour(0xff111821));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xff303846));
    g.drawHorizontalLine(bounds.getY(), static_cast<float>(bounds.getX()), static_cast<float>(bounds.getRight()));

    auto trackArea = bounds;
    if (! document_.tempoSegments.empty() || ! document_.timeSignatures.empty() || ! document_.chords.empty())
        drawMusicalContext(g, trackArea.removeFromTop(kMusicalContextHeight));

    auto trackBounds = trackArea.reduced(0, 7);
    g.setColour(juce::Colour(0xff9aa4b2).withAlpha(0.12f));
    g.fillRect(trackBounds);
    drawMusicalGrid(g, trackBounds);
    g.setColour(juce::Colour(0xffd1d5db).withAlpha(0.44f));
    instrumentalThumbnail_.drawChannels(g, trackBounds, visibleStart_, visibleEnd_, 0.82f);

}

void AnnotationEditorComponent::drawSelectedWaveformBorder(juce::Graphics& g, juce::Rectangle<int> waveformBounds)
{
    const auto selectedRange = getSelectedTimeRange();
    if (! selectedRange || selectedRange->end <= visibleStart_ || selectedRange->start >= visibleEnd_)
        return;

    const auto clippedStart = juce::jmax(selectedRange->start, visibleStart_);
    const auto clippedEnd = juce::jmin(selectedRange->end, visibleEnd_);
    const auto x = timeToX(clippedStart);
    const auto right = timeToX(clippedEnd);
    if (right <= x + 1.0f)
        return;

    const auto segmentStarts = waveformSegmentStarts(document_);

    auto selectedColour = juce::Colour(0xff06b6d4);
    const auto selectedMidpoint = (selectedRange->start + selectedRange->end) * 0.5;
    for (size_t i = 0; i < segmentStarts.size(); ++i)
    {
        const auto start = segmentStarts[i].time;
        const auto end = i + 1 < segmentStarts.size() ? segmentStarts[i + 1].time : document_.duration;
        if (selectedMidpoint >= start - 0.005 && selectedMidpoint <= end + 0.005)
        {
            selectedColour = waveformColourFor(segmentStarts[i].kind, i);
            break;
        }
    }

    juce::Graphics::ScopedSaveState state(g);
    const auto clipLeft = juce::roundToInt(x) - 3;
    const auto clipRight = juce::roundToInt(right) + 3;
    g.reduceClipRegion(juce::Rectangle<int>(clipLeft,
                                            waveformBounds.getY(),
                                            juce::jmax(1, clipRight - clipLeft),
                                            waveformBounds.getHeight()));

    g.setColour(selectedColour.withAlpha(1.0f));
    thumbnail_.drawChannels(g, waveformBounds, visibleStart_, visibleEnd_, 0.84f);
}

void AnnotationEditorComponent::drawNotes(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    for (const auto& note : document_.notes)
    {
        if (note.end < visibleStart_ || note.start > visibleEnd_)
            continue;

        const auto noteBounds = noteBoundsFor(note);
        auto colour = noteColourForPitch(note.pitch);
        const auto targetLane = juce::Rectangle<float>(noteBounds.getX(),
                                                        pitchToY(note.pitch + 0.5),
                                                        noteBounds.getWidth(),
                                                        pitchToY(note.pitch - 0.5) - pitchToY(note.pitch + 0.5));
        const auto filledBounds = noteBounds.getIntersection(targetLane);

        if (! filledBounds.isEmpty())
        {
            juce::Graphics::ScopedSaveState fillState(g);
            g.reduceClipRegion(filledBounds.toNearestIntEdges());
            g.setColour(colour.darker(0.22f).withAlpha(0.94f));
            g.fillRoundedRectangle(noteBounds, 3.0f);
        }

        g.setColour(juce::Colours::black.withAlpha(0.82f));
        g.drawRoundedRectangle(noteBounds, 3.0f, 1.2f);

        const juce::Rectangle<float> voiced(timeToX(note.voicedStart),
                                            filledBounds.getY() + filledBounds.getHeight() * 0.28f,
                                            timeToX(note.voicedEnd) - timeToX(note.voicedStart),
                                            filledBounds.getHeight() * 0.44f);
        if (! filledBounds.isEmpty())
        {
            g.setColour(juce::Colours::black.withAlpha(0.22f));
            g.fillRect(voiced.getIntersection(filledBounds));
        }

        const auto isSelected = note.id == selectedNoteId_;
        if (isSelected)
        {
            g.setColour(selectedShadowColour());
            g.drawRoundedRectangle(noteBounds.expanded(2.0f), kSelectedCornerRadius, 3.0f);
            g.setColour(colour.brighter(0.35f));
            g.drawRoundedRectangle(noteBounds.expanded(1.0f), kSelectedCornerRadius, 2.2f);
        }

        if (note.curve.size() > 1)
        {
            juce::Path curve;
            bool started = false;
            for (const auto& point : note.curve)
            {
                const auto x = timeToX(point.time);
                const auto y = pitchToY(point.midi);
                if (! started)
                {
                    curve.startNewSubPath(x, y);
                    started = true;
                }
                else
                {
                    curve.lineTo(x, y);
                }
            }
            g.setColour(juce::Colour(0xffd2e8ff).withAlpha(0.5f));
            g.strokePath(curve, juce::PathStrokeType(1.35f));
        }

        if (note.lyric.isNotEmpty() && noteBounds.getWidth() > 24.0f)
        {
            g.setColour(isSelected ? juce::Colours::white : juce::Colours::black.withAlpha(0.82f));
            auto lyricBounds = noteBounds.reduced(4.0f, 0.0f);
            if (noteBounds.getWidth() > 68.0f)
                lyricBounds.removeFromRight(38.0f);
            g.drawText(note.lyric, lyricBounds, juce::Justification::centredLeft);
        }

        if (noteBounds.getWidth() > 54.0f && noteBounds.getHeight() >= 10.0f)
        {
            const auto cents = juce::roundToInt((note.pitchExact - static_cast<double>(note.pitch)) * 100.0);
            const auto centsText = (cents > 0 ? "+" : "") + juce::String(cents) + "c";
            g.setFont(juce::FontOptions(10.0f));
            g.setColour(juce::Colours::white.withAlpha(0.78f));
            g.drawText(centsText, noteBounds.reduced(4.0f, 0.0f), juce::Justification::centredRight);
        }
    }

    for (size_t i = 1; i < document_.notes.size(); ++i)
    {
        const auto& previous = document_.notes[i - 1];
        const auto& current = document_.notes[i];
        if (! current.flags.contains("legato_from_previous")
            || std::abs(previous.end - current.start) > 0.015
            || previous.end < visibleStart_
            || current.start > visibleEnd_)
        {
            continue;
        }

        const auto previousBounds = noteBoundsFor(previous);
        const auto currentBounds = noteBoundsFor(current);
        const auto previousInset = juce::jmin(22.0f, previousBounds.getWidth() * 0.35f);
        const auto currentInset = juce::jmin(22.0f, currentBounds.getWidth() * 0.35f);
        const auto start = juce::Point<float>(previousBounds.getRight() - previousInset,
                                              previousBounds.getY() - 3.0f);
        const auto end = juce::Point<float>(currentBounds.getX() + currentInset,
                                            currentBounds.getY() - 3.0f);
        const auto lift = juce::jmax(8.0f, std::abs(end.y - start.y) * 0.35f + 5.0f);
        const auto controlY = juce::jmin(start.y, end.y) - lift;

        juce::Path slur;
        slur.startNewSubPath(start);
        slur.cubicTo(start.x + (end.x - start.x) * 0.32f,
                     controlY,
                     start.x + (end.x - start.x) * 0.68f,
                     controlY,
                     end.x,
                     end.y);
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.strokePath(slur, juce::PathStrokeType(2.0f,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }
}

void AnnotationEditorComponent::drawBackingNotes(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    g.setColour(juce::Colour(0xff101820));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xff344054));
    g.drawHorizontalLine(bounds.getY(), static_cast<float>(bounds.getX()), static_cast<float>(bounds.getRight()));
    g.setColour(juce::Colour(0xff5eead4).withAlpha(0.25f));
    g.drawHorizontalLine(bounds.getY() + 1, static_cast<float>(bounds.getX()), static_cast<float>(bounds.getRight()));

    drawMusicalGrid(g, bounds);

    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.setColour(juce::Colours::white.withAlpha(0.48f));
    g.drawText(document_.backingStyleName.isNotEmpty() ? document_.backingStyleName : "Backing vocal",
               bounds.reduced(8, 4).removeFromTop(20),
               juce::Justification::centredLeft);

    for (const auto& note : document_.backingNotes)
    {
        if (note.end < visibleStart_ || note.start > visibleEnd_)
            continue;

        const auto noteBounds = noteBoundsFor(note, bounds);
        auto colour = juce::Colour(0xff5eead4);
        const auto targetLane = juce::Rectangle<float>(noteBounds.getX(),
                                                        pitchToYInBounds(note.pitch + 0.5, bounds),
                                                        noteBounds.getWidth(),
                                                        pitchToYInBounds(note.pitch - 0.5, bounds)
                                                            - pitchToYInBounds(note.pitch + 0.5, bounds));
        const auto filledBounds = noteBounds.getIntersection(targetLane);

        if (! filledBounds.isEmpty())
        {
            juce::Graphics::ScopedSaveState fillState(g);
            g.reduceClipRegion(filledBounds.toNearestIntEdges());
            g.setColour(colour.darker(0.28f).withAlpha(0.94f));
            g.fillRoundedRectangle(noteBounds, 3.0f);
        }

        g.setColour(juce::Colours::black.withAlpha(0.82f));
        g.drawRoundedRectangle(noteBounds, 3.0f, 1.2f);

        if (note.lyric.isNotEmpty() && noteBounds.getWidth() > 32.0f)
        {
            g.setColour(juce::Colours::black.withAlpha(0.82f));
            g.drawText(note.lyric, noteBounds.reduced(4.0f, 0.0f), juce::Justification::centredLeft);
        }
    }
}

void AnnotationEditorComponent::drawBoundaries(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(bounds);

    for (const auto& boundary : document_.boundaries)
    {
        if (boundary.source == "pyin_draft")
            continue;

        if (boundary.time < visibleStart_ || boundary.time > visibleEnd_)
            continue;

        const auto x = timeToX(boundary.time);
        const auto isSelectedBoundary = boundary.id == selectedBoundaryId_;
        const auto colour = isSelectedBoundary ? juce::Colours::white : juce::Colour(0xffffd166);
        g.setColour(colour.withAlpha(0.2f));
        g.drawLine(x,
                   static_cast<float>(bounds.getY()),
                   x,
                   static_cast<float>(bounds.getBottom()),
                   isSelectedBoundary ? 2.2f : 1.2f);
    }
}

void AnnotationEditorComponent::drawSplitPreview(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (! splitPreviewActive_ || splitPreviewTime_ < visibleStart_ || splitPreviewTime_ > visibleEnd_)
        return;

    const auto noteIndex = document_.findNoteIndex(splitPreviewNoteId_);
    if (! noteIndex)
        return;

    const auto& note = document_.notes[static_cast<size_t>(*noteIndex)];
    if (splitPreviewTime_ <= note.start + kMinNoteLength || splitPreviewTime_ >= note.end - kMinNoteLength)
        return;

    const auto x = timeToX(splitPreviewTime_);
    const auto noteBounds = noteBoundsFor(note);
    const auto noteTop = noteBounds.getY();
    const auto noteBottom = noteBounds.getBottom();
    const float dashPattern[] = { 5.0f, 4.0f };

    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(bounds);

    g.setColour(juce::Colours::white.withAlpha(0.18f));
    g.drawDashedLine(juce::Line<float>(x, static_cast<float>(bounds.getY()), x, static_cast<float>(bounds.getBottom())),
                     dashPattern,
                     2,
                     1.1f);

    g.setColour(juce::Colours::black.withAlpha(0.70f));
    g.drawDashedLine(juce::Line<float>(x + 1.0f, noteTop - 7.0f, x + 1.0f, noteBottom + 7.0f),
                     dashPattern,
                     2,
                     4.0f);
    g.setColour(juce::Colours::white);
    g.drawDashedLine(juce::Line<float>(x, noteTop - 7.0f, x, noteBottom + 7.0f),
                     dashPattern,
                     2,
                     2.2f);
}

std::optional<AnnotationEditorComponent::TimeRange> AnnotationEditorComponent::getSelectedTimeRange() const
{
    if (auto noteIndex = document_.findNoteIndex(selectedNoteId_))
    {
        const auto& note = document_.notes[static_cast<size_t>(*noteIndex)];
        return TimeRange{ note.start, note.end };
    }

    if (auto boundaryIndex = document_.findBoundaryIndex(selectedBoundaryId_))
    {
        const auto& selectedBoundary = document_.boundaries[static_cast<size_t>(*boundaryIndex)];
        auto end = document_.duration;

        for (const auto& boundary : document_.boundaries)
        {
            if (boundary.kind == BoundaryKind::ignore)
                continue;

            if (boundary.time > selectedBoundary.time + 0.005)
                end = juce::jmin(end, boundary.time);
        }

        return TimeRange{ juce::jlimit(0.0, document_.duration, selectedBoundary.time),
                          juce::jlimit(0.0, document_.duration, end) };
    }

    return std::nullopt;
}

void AnnotationEditorComponent::drawLyrics(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    g.setColour(juce::Colour(0xff20252d));
    g.fillRect(bounds);
    g.setColour(juce::Colour(0xff596170));
    g.drawRect(bounds);

    for (const auto& note : document_.notes)
    {
        if (note.lyric.isEmpty() || note.end < visibleStart_ || note.start > visibleEnd_)
            continue;

        auto groupEnd = note.end;
        auto groupSelected = note.id == selectedNoteId_;
        if (note.syllableId.isNotEmpty())
        {
            for (const auto& groupedNote : document_.notes)
            {
                if (groupedNote.syllableId != note.syllableId)
                    continue;

                groupEnd = juce::jmax(groupEnd, groupedNote.end);
                groupSelected = groupSelected || groupedNote.id == selectedNoteId_;
            }
        }

        g.setColour(groupSelected ? juce::Colours::white : juce::Colour(0xffd6dce7));
        g.drawText(note.lyric,
                   juce::roundToInt(timeToX(note.start)) + 3,
                   bounds.getY() + 4,
                   juce::jmax(1, juce::roundToInt(timeToX(groupEnd) - timeToX(note.start)) - 6),
                   bounds.getHeight() - 8,
                   juce::Justification::centredLeft);
    }
}

} // namespace vocal_annotation
