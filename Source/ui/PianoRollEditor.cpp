#include "PianoRollEditor.h"

//==============================================================================
namespace
{
    // Black key pattern within one octave (0 = C)
    constexpr bool kIsBlack[12] = { false, true,  false, true,  false,
                                    false, true,  false, true,  false, true, false };

    // Piano-key colour constants (independent of LookAndFeel)
    constexpr juce::uint32 kWhiteKey = 0xff3a3a50;
    constexpr juce::uint32 kBlackKey = 0xff141420;

    constexpr juce::uint32 kGridWhiteRow  = 0xff0a0a12;  // matches Theme::kBackground
    constexpr juce::uint32 kGridBlackRow  = 0xff070710;  // slightly darker for black-key rows

    constexpr juce::uint32 kBarLine       = 0x28ffffff;
    constexpr juce::uint32 kBeatLine      = 0x14ffffff;
    constexpr juce::uint32 kSubbeatLine   = 0x08ffffff;
    constexpr juce::uint32 kRowSep        = 0x18ffffff;
    constexpr juce::uint32 kCRowHighlight = 0x0cffffff;
} // namespace

//==============================================================================
PianoRollEditor::PianoRollEditor()
{
    auto setupViewButton = [this] (juce::TextButton& button)
    {
        addAndMakeVisible (button);
        button.setColour (juce::TextButton::buttonColourId,
                          juce::Colour (Theme::kSurface).brighter (0.08f));
        button.setColour (juce::TextButton::buttonOnColourId,
                          juce::Colour (Theme::kAccentCyan).withAlpha (0.7f));
        button.setColour (juce::TextButton::textColourOffId,
                          juce::Colour (Theme::kTextSecondary));
        button.setColour (juce::TextButton::textColourOnId,
                          juce::Colour (Theme::kBackground));
    };

    setupViewButton (yZoomInBtn_);
    setupViewButton (yZoomOutBtn_);
    setupViewButton (yUpBtn_);
    setupViewButton (yDownBtn_);
    addAndMakeVisible (sensitivityLabel_);
    addAndMakeVisible (sensitivitySlider_);

    yZoomInBtn_.onClick  = [this] { zoomPitchView (0.78, (topNote_ + bottomNote_) / 2); };
    yZoomOutBtn_.onClick = [this] { zoomPitchView (1.28, (topNote_ + bottomNote_) / 2); };
    yUpBtn_.onClick      = [this] { scrollPitchView (4); };
    yDownBtn_.onClick    = [this] { scrollPitchView (-4); };

    sensitivityLabel_.setFont (juce::FontOptions (9.0f).withStyle ("Bold"));
    sensitivityLabel_.setJustificationType (juce::Justification::centredRight);
    sensitivityLabel_.setColour (juce::Label::textColourId,
                                 juce::Colour (Theme::kTextSecondary));

    sensitivitySlider_.setSliderStyle (juce::Slider::LinearHorizontal);
    sensitivitySlider_.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    sensitivitySlider_.setRange (0.0, 1.0, 0.01);
    sensitivitySlider_.setValue (0.72, juce::dontSendNotification);
    sensitivitySlider_.setColour (juce::Slider::trackColourId,
                                  juce::Colour (Theme::kAccentCyan).withAlpha (0.75f));
    sensitivitySlider_.setColour (juce::Slider::backgroundColourId,
                                  juce::Colour (Theme::kBorder).withAlpha (0.5f));
    sensitivitySlider_.setColour (juce::Slider::thumbColourId,
                                  juce::Colour (Theme::kAccentCyan));
    sensitivitySlider_.onDragEnd = [this]
    {
        if (onDetectionSensitivityChanged)
            onDetectionSensitivityChanged ((float)sensitivitySlider_.getValue());
    };

    startTimerHz (30);
    // Piano roll starts empty; notes are populated when a track is selected
    // and analysis has completed. The "No pitch detected" overlay shows when
    // a track is selected but YIN found no monophonic pitch content.
}

PianoRollEditor::~PianoRollEditor()
{
    stopTimer();
}

//==============================================================================
void PianoRollEditor::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    // Background
    g.setColour (juce::Colour (Theme::kBackground));
    g.fillRect (bounds);

    // ── Time ruler (top strip) ──────────────────────────────────────────────
    auto rulerStrip = bounds.removeFromTop (kTimeRulerH);
    auto corner     = rulerStrip.removeFromLeft (kPianoKeyWidth);

    g.setColour (juce::Colour (Theme::kSurface));
    g.fillRect (corner);
    g.setColour (juce::Colour (Theme::kBorder));
    g.drawRect  (corner, 1);

    paintTimeRuler (g, rulerStrip);

    // ── Main area ───────────────────────────────────────────────────────────
    auto pianoArea = bounds.removeFromLeft (kPianoKeyWidth);
    auto noteArea  = bounds;

    paintPianoKeys (g, pianoArea);
    paintNoteGrid  (g, noteArea);
    paintWaveform  (g, noteArea);
    paintNotes             (g, noteArea);
    paintNoNotesOverlay    (g, noteArea);
    paintAnalyzingOverlay  (g, noteArea);
    paintProcessingOverlay (g, noteArea);
    paintPlayhead          (g, noteArea);
}

void PianoRollEditor::resized()
{
    auto controls = getLocalBounds().removeFromTop (kTimeRulerH).removeFromRight (218).reduced (4, 3);
    sensitivityLabel_.setBounds (controls.removeFromLeft (26));
    controls.removeFromLeft (4);
    sensitivitySlider_.setBounds (controls.removeFromLeft (76));
    controls.removeFromLeft (7);
    yUpBtn_.setBounds      (controls.removeFromLeft (24));
    controls.removeFromLeft (3);
    yDownBtn_.setBounds    (controls.removeFromLeft (24));
    controls.removeFromLeft (5);
    yZoomInBtn_.setBounds  (controls.removeFromLeft (24));
    controls.removeFromLeft (3);
    yZoomOutBtn_.setBounds (controls.removeFromLeft (24));
}

//==============================================================================
void PianoRollEditor::paintTimeRuler (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour (juce::Colour (Theme::kSurface));
    g.fillRect (bounds);

    g.setColour (juce::Colour (Theme::kBorder));
    g.drawHorizontalLine (bounds.getBottom() - 1,
                          (float)bounds.getX(), (float)bounds.getRight());

    const double viewDuration = viewEndSec_ - viewStartSec_;
    const double secPerBeat   = 60.0 / bpm_;
    const double secPerBar    = secPerBeat * 4.0;
    const double pxPerSec     = bounds.getWidth() / viewDuration;
    const double pxPerBar     = pxPerSec * secPerBar;

    g.setFont (juce::FontOptions (9.0f));

    int bar = (int)std::floor (viewStartSec_ / secPerBar);
    for (double t = bar * secPerBar; t <= viewEndSec_ + secPerBar; t += secPerBar, ++bar)
    {
        if (t < viewStartSec_ - 0.001) continue;

        float x = (float)bounds.getX()
                  + (float)((t - viewStartSec_) / viewDuration * bounds.getWidth());
        if (x > (float)bounds.getRight() + 1.0f) break;

        // Bar tick + label
        g.setColour (juce::Colour (Theme::kTextSecondary));
        g.drawVerticalLine ((int)x, (float)bounds.getY(), (float)bounds.getBottom());
        g.drawText (juce::String (bar + 1),
                    (int)x + 2, bounds.getY() + 2,
                    40, bounds.getHeight() - 4,
                    juce::Justification::centredLeft, true);

        // Beat sub-ticks
        if (pxPerBar > 48.0)
        {
            g.setColour (juce::Colour (Theme::kBorder));
            for (int beat = 1; beat < 4; ++beat)
            {
                float bx = x + (float)(beat * pxPerSec * secPerBeat);
                g.drawVerticalLine ((int)bx,
                                    (float)bounds.getBottom() - 6.0f,
                                    (float)bounds.getBottom());
            }
        }
    }
}

//==============================================================================
void PianoRollEditor::paintPianoKeys (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const int   numSemitones = topNote_ - bottomNote_ + 1;
    const float rowH         = (float)bounds.getHeight() / (float)numSemitones;

    for (int i = 0; i < numSemitones; ++i)
    {
        const int   note   = topNote_ - i;
        const int   octave = note / 12 - 1;
        const int   semi   = note % 12;
        const float y      = (float)bounds.getY() + (float)i * rowH;

        auto rowRect = juce::Rectangle<float> ((float)bounds.getX(), y,
                                               (float)bounds.getWidth(), rowH);

        if (kIsBlack[semi])
        {
            // Full-width very dark background
            g.setColour (juce::Colour (kBlackKey));
            g.fillRect (rowRect);
        }
        else
        {
            // White key — lighter, fills full width
            g.setColour (juce::Colour (kWhiteKey));
            g.fillRect (rowRect);

            // Label every C note
            if (semi == 0)
            {
                g.setColour (juce::Colour (Theme::kAccentCyan).withAlpha (0.7f));
                g.setFont   (juce::FontOptions (8.0f));
                g.drawText  ("C" + juce::String (octave),
                             bounds.getX() + 2, (int)y,
                             bounds.getWidth() - 4, (int)rowH,
                             juce::Justification::centredRight, true);
            }
        }

        // Row separator
        g.setColour (juce::Colour (kRowSep));
        g.drawHorizontalLine ((int)y,
                              (float)bounds.getX(), (float)bounds.getRight());
    }

    // Right border
    g.setColour (juce::Colour (Theme::kBorder));
    g.drawVerticalLine (bounds.getRight() - 1,
                        (float)bounds.getY(), (float)bounds.getBottom());
}

//==============================================================================
void PianoRollEditor::paintNoteGrid (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const int   numSemitones = topNote_ - bottomNote_ + 1;
    const float rowH         = (float)bounds.getHeight() / (float)numSemitones;

    // ── Row backgrounds ──────────────────────────────────────────────────────
    for (int i = 0; i < numSemitones; ++i)
    {
        const int   note = topNote_ - i;
        const int   semi = note % 12;
        const float y    = (float)bounds.getY() + (float)i * rowH;

        g.setColour (juce::Colour (kIsBlack[semi] ? kGridBlackRow : kGridWhiteRow));
        g.fillRect  (juce::Rectangle<float> ((float)bounds.getX(), y,
                                             (float)bounds.getWidth(), rowH));

        // Subtle row separator
        g.setColour (juce::Colour (kRowSep));
        g.drawHorizontalLine ((int)y, (float)bounds.getX(), (float)bounds.getRight());

        // Highlight C note row more visibly
        if (semi == 0)
        {
            g.setColour (juce::Colour (kCRowHighlight));
            g.drawHorizontalLine ((int)y, (float)bounds.getX(), (float)bounds.getRight());
        }
    }

    // ── Vertical bar / beat / sub-beat lines ─────────────────────────────────
    const double viewDuration = viewEndSec_ - viewStartSec_;
    const double secPerBeat   = 60.0 / bpm_;
    const double secPerBar    = secPerBeat * 4.0;
    const double pxPerSec     = bounds.getWidth() / viewDuration;
    const double pxPerBar     = pxPerSec * secPerBar;

    int bar = (int)std::floor (viewStartSec_ / secPerBar);
    for (double t = bar * secPerBar; t <= viewEndSec_ + secPerBar; t += secPerBar, ++bar)
    {
        if (t < viewStartSec_ - 0.001) continue;

        float x = (float)bounds.getX()
                  + (float)((t - viewStartSec_) / viewDuration * bounds.getWidth());
        if (x > (float)bounds.getRight() + 1.0f) break;

        // Bar line — brightest
        g.setColour (juce::Colour (kBarLine));
        g.drawVerticalLine ((int)x, (float)bounds.getY(), (float)bounds.getBottom());

        if (pxPerBar > 48.0)
        {
            // Beat lines
            for (int beat = 1; beat < 4; ++beat)
            {
                float bx = x + (float)(beat * pxPerSec * secPerBeat);
                g.setColour (juce::Colour (kBeatLine));
                g.drawVerticalLine ((int)bx,
                                    (float)bounds.getY(), (float)bounds.getBottom());

                // 16th-note subdivisions at high zoom
                if (pxPerBar > 240.0)
                {
                    for (int sub = 1; sub < 4; ++sub)
                    {
                        float sx = x + (float)((beat + sub * 0.25) * pxPerSec * secPerBeat);
                        g.setColour (juce::Colour (kSubbeatLine));
                        g.drawVerticalLine ((int)sx,
                                            (float)bounds.getY(), (float)bounds.getBottom());
                    }
                }
            }
        }
    }
}

//==============================================================================
void PianoRollEditor::paintNotes (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const int   numSemitones = topNote_ - bottomNote_ + 1;
    const float rowH         = (float)bounds.getHeight() / (float)numSemitones;
    const float noteH        = juce::jmax (2.0f, rowH - 1.0f);

    const juce::ScopedTryLock lock (notesLock_);
    if (! lock.isLocked()) return;   // skip frame if writer holds lock

    // ── Drag tooltip state (populated below if dragging) ─────────────────────
    juce::String dragLabel;
    juce::Point<float> dragLabelPos;
    std::vector<float> syllableMarkerXs;
    struct LyricLabel
    {
        juce::String text;
        float x;
        float w;
    };
    std::vector<LyricLabel> lyricLabels;

    for (int i = 0; i < notes_.size(); ++i)
    {
        const auto& note = notes_.getReference (i);

        // Skip notes outside visible pitch range or time window
        if (note.midiPitch < bottomNote_ || note.midiPitch > topNote_) continue;

        const double noteEnd = note.startSeconds + note.durationSeconds;
        if (noteEnd  < viewStartSec_) continue;
        if (note.startSeconds > viewEndSec_) continue;

        const float y      = pitchToY (note.midiPitch, bounds);
        float       xStart = secondsToX (note.startSeconds, bounds);
        float       xEnd   = secondsToX (noteEnd,           bounds);

        // Clamp to visible area
        xStart = juce::jmax (xStart, (float)bounds.getX());
        xEnd   = juce::jmin (xEnd,   (float)bounds.getRight());
        float w = juce::jmax (2.0f, xEnd - xStart);

        auto noteRect = juce::Rectangle<float> (xStart, y + 0.5f, w, noteH - 1.0f);

        // ── Body colour: interpolate toward orange as |cents| → 50 ──────────
        const float deviationRatio = juce::jlimit (0.0f, 1.0f,
                                                   std::abs (note.centsOffset) / 50.0f);
        const auto  inTuneColour   = note.colour;
        const auto  outOfTune      = juce::Colour (0xffff8800);  // orange
        const auto  bodyColour     = inTuneColour.interpolatedWith (outOfTune, deviationRatio * 0.7f);

        const float alpha     = 0.55f + note.confidence * 0.45f;
        const auto  baseColor = bodyColour.withAlpha (alpha);

        // Highlight selected/dragged note
        const bool isDragged = drag_.active && drag_.noteIdx == i;
        g.setColour (isDragged ? baseColor.brighter (0.25f) : baseColor);
        g.fillRoundedRectangle (noteRect, 2.0f);
        if (note.syllableStart)
            syllableMarkerXs.push_back (xStart);

        // Top-edge highlight
        g.setColour (baseColor.brighter (0.5f).withAlpha (alpha));
        g.drawHorizontalLine ((int)(y + 0.5f), xStart, xStart + w);

        // Outline (brighter when dragged)
        g.setColour (isDragged ? juce::Colours::white.withAlpha (0.7f)
                               : baseColor.brighter (0.25f));
        g.drawRoundedRectangle (noteRect, 2.0f, isDragged ? 1.5f : 1.0f);

        if (note.syllableStart)
        {
            const float markerH = juce::jmax (8.0f, noteH + 5.0f);
            const float markerTop = y + rowH * 0.5f - markerH * 0.5f;
            const auto markerColour = juce::Colour (0xffffd166);
            g.setColour (markerColour.withAlpha (0.22f));
            g.fillRoundedRectangle (juce::Rectangle<float> (xStart - 2.0f, markerTop,
                                                            5.0f, markerH),
                                    2.0f);
            g.setColour (markerColour.withAlpha (0.88f));
            g.drawVerticalLine ((int)std::round (xStart),
                                markerTop,
                                markerTop + markerH);
        }

        if (note.pitchContourSize >= 2)
        {
            juce::Path contourPath;
            bool hasStarted = false;

            for (int pointIdx = 0; pointIdx < note.pitchContourSize; ++pointIdx)
            {
                const auto& point = note.pitchContour[(size_t)pointIdx];
                const double absTime = note.startSeconds + (double)point.offsetSeconds;
                if (absTime < viewStartSec_ - 0.05 || absTime > viewEndSec_ + 0.05)
                    continue;

                const float pitchFloor = std::floor (point.midiPitch);
                const float pitchFrac = point.midiPitch - pitchFloor;
                const float px = secondsToX (absTime, bounds);
                const float py = pitchToY ((int)pitchFloor, bounds) + rowH * (1.0f - pitchFrac);
                const float clampedY = juce::jlimit ((float)bounds.getY() + 1.0f,
                                                     (float)bounds.getBottom() - 1.0f,
                                                     py);

                if (! hasStarted)
                {
                    contourPath.startNewSubPath (px, clampedY);
                    hasStarted = true;
                }
                else
                {
                    contourPath.lineTo (px, clampedY);
                }
            }

            if (hasStarted)
            {
                g.setColour (juce::Colours::white.withAlpha (0.18f));
                g.strokePath (contourPath, juce::PathStrokeType (3.0f,
                                                                 juce::PathStrokeType::curved,
                                                                 juce::PathStrokeType::rounded));
                g.setColour (juce::Colours::white.withAlpha (0.80f));
                g.strokePath (contourPath, juce::PathStrokeType (1.4f,
                                                                 juce::PathStrokeType::curved,
                                                                 juce::PathStrokeType::rounded));
            }
        }

        // ── Cents indicator line ─────────────────────────────────────────────
        // Line Y within the row: 0¢ = center, +50¢ = top, -50¢ = bottom.
        //   indicatorY = rowTop + rowH/2 * (1 - centsOffset/50)
        if (rowH >= 5.0f)  // only visible when rows are tall enough
        {
            const float indicatorY = y + (rowH * 0.5f) * (1.0f - note.centsOffset / 50.0f);
            const float iy = juce::jlimit (y + 1.0f, y + rowH - 2.0f, indicatorY);

            // Outer glow
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawHorizontalLine ((int)iy - 1, xStart + 2.0f, xStart + w - 2.0f);
            g.drawHorizontalLine ((int)iy + 1, xStart + 2.0f, xStart + w - 2.0f);

            // Bright marker
            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.drawHorizontalLine ((int)iy, xStart + 2.0f, xStart + w - 2.0f);
        }

        // ── Collect drag tooltip info ─────────────────────────────────────────
        if (isDragged)
        {
            const int centsRounded = (int)std::round (note.centsOffset);
            const juce::String centsStr = (centsRounded >= 0)
                                          ? "+" + juce::String (centsRounded) + "c"
                                          : juce::String (centsRounded) + "c";
            dragLabel    = midiNoteName (note.midiPitch) + "  " + centsStr;
            dragLabelPos = { xStart + w * 0.5f, y - 4.0f };
        }

        if (note.lyric.isNotEmpty() && w >= 8.0f)
            lyricLabels.push_back ({ note.lyric, xStart, w });
    }

    std::sort (syllableMarkerXs.begin(), syllableMarkerXs.end());
    syllableMarkerXs.erase (std::unique (syllableMarkerXs.begin(), syllableMarkerXs.end(),
                                         [] (float a, float b) { return std::abs (a - b) < 3.0f; }),
                            syllableMarkerXs.end());

    for (float markerX : syllableMarkerXs)
    {
        const auto markerColour = juce::Colour (0xffffd166);
        g.setColour (markerColour.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> (markerX - 1.5f, (float)bounds.getY(),
                                            3.0f, (float)bounds.getHeight()));
        g.setColour (markerColour.withAlpha (0.82f));
        g.drawVerticalLine ((int)std::round (markerX),
                            (float)bounds.getY(),
                            (float)bounds.getBottom());
    }

    if (! lyricLabels.empty())
    {
        auto lyricBand = bounds.toFloat().removeFromBottom (42.0f).reduced (4.0f, 3.0f);
        g.setColour (juce::Colour (0xaa05050a));
        g.fillRoundedRectangle (lyricBand, 4.0f);
        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));

        for (const auto& label : lyricLabels)
        {
            auto textRect = juce::Rectangle<float> (label.x, lyricBand.getY() + 5.0f,
                                                    juce::jmax (34.0f, label.w),
                                                    lyricBand.getHeight() - 10.0f);
            textRect = textRect.getIntersection (lyricBand);
            if (textRect.getWidth() < 14.0f)
                continue;

            g.setColour (juce::Colour (Theme::kBackground).withAlpha (0.76f));
            g.fillRoundedRectangle (textRect.reduced (1.0f, 2.0f), 4.0f);
            g.setColour (juce::Colour (0xffffd166).withAlpha (0.9f));
            g.drawText (label.text, textRect.toNearestInt(),
                        juce::Justification::centred, true);
        }
    }

    // ── Draw drag tooltip ─────────────────────────────────────────────────────
    if (drag_.active && dragLabel.isNotEmpty())
    {
        g.setFont (juce::FontOptions (11.0f).withStyle ("Bold"));
        juce::GlyphArrangement ga;
        ga.addLineOfText (g.getCurrentFont(), dragLabel, 0.0f, 0.0f);
        const float textW  = ga.getBoundingBox (0, -1, true).getWidth() + 14.0f;
        const float textH  = 18.0f;
        float       lx     = dragLabelPos.x - textW * 0.5f;
        float       ly     = dragLabelPos.y - textH - 2.0f;

        // Clamp inside bounds
        lx = juce::jlimit ((float)bounds.getX(), (float)bounds.getRight()  - textW, lx);
        ly = juce::jlimit ((float)bounds.getY(), (float)bounds.getBottom() - textH, ly);

        auto labelRect = juce::Rectangle<float> (lx, ly, textW, textH);
        g.setColour (juce::Colour (0xee101018));
        g.fillRoundedRectangle (labelRect, 4.0f);
        g.setColour (juce::Colour (Theme::kAccentCyan));
        g.drawRoundedRectangle (labelRect, 4.0f, 1.0f);
        g.setColour (juce::Colours::white);
        g.drawText (dragLabel, labelRect.toNearestInt(), juce::Justification::centred, false);
    }
}

//==============================================================================
void PianoRollEditor::paintNoNotesOverlay (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    if (! noNotesPlaceholder_) return;

    // Subtle centred label
    g.setColour (juce::Colour (Theme::kTextSecondary).withAlpha (0.45f));
    g.setFont   (juce::FontOptions (13.0f));
    g.drawText  ("No pitch detected - load a vocal / melody track",
                 bounds, juce::Justification::centred, true);
}

//==============================================================================
void PianoRollEditor::paintWaveform (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    if (thumbnail_ == nullptr || thumbnail_->getNumChannels() == 0) return;

    // Very subtle waveform in bottom quarter — stays out of the way of notes
    auto waveRect = bounds.removeFromBottom (bounds.getHeight() / 4);
    g.setColour (juce::Colour (Theme::kAccentCyan).withAlpha (0.12f));
    thumbnail_->drawChannels (g, waveRect, viewStartSec_, viewEndSec_, 0.5f);
}

//==============================================================================
void PianoRollEditor::paintNoPresetBanner (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    // Show only when: notes exist (user can edit) AND RVC preset is not loaded.
    // Hidden while processing, analyzing, or if no notes are present yet.
    if (hasVoicePreset_ || isProcessing_ || isAnalyzing_) return;
    {
        juce::ScopedLock lock (notesLock_);
        if (notes_.isEmpty()) return;
    }

    // ── Amber warning strip pinned to the bottom of the note area ─────────
    const int   bannerH = 28;
    const auto  strip   = bounds.removeFromBottom (bannerH).toFloat();

    // Background — semi-transparent amber
    g.setColour (juce::Colour (0xdd2a1a00));
    g.fillRect  (strip);

    // Top border line in amber
    g.setColour (juce::Colour (0xffff9500));
    g.drawHorizontalLine ((int)strip.getY(),
                          strip.getX(), strip.getRight());

    // Warning icon + text
    g.setFont   (juce::FontOptions (11.0f));
    g.setColour (juce::Colour (0xffffc060));

    const juce::String msg =
        "!  No voice preset loaded - corrections use DSP fallback "
        "(audible artifacts on large pitch changes).  "
        "Load a Voice Preset for AI-quality results.";

    g.drawText (msg, strip.reduced (8.0f, 0.0f).toNearestInt(),
                juce::Justification::centredLeft, true);
}

//==============================================================================
void PianoRollEditor::paintPlayhead (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const double pos = playheadPos_.load();
    if (pos < viewStartSec_ || pos > viewEndSec_) return;

    const float x = secondsToX (pos, bounds);

    // Glow halo
    g.setColour (juce::Colour (Theme::kAccentCyan).withAlpha (0.12f));
    g.fillRect  (juce::Rectangle<float> (x - 2.0f, (float)bounds.getY(),
                                         5.0f, (float)bounds.getHeight()));

    // Line
    g.setColour (juce::Colour (Theme::kAccentCyan));
    g.drawVerticalLine ((int)x, (float)bounds.getY(), (float)bounds.getBottom());

    // Triangle head
    juce::Path head;
    head.addTriangle (x - 5.0f, (float)bounds.getY(),
                      x + 5.0f, (float)bounds.getY(),
                      x,        (float)bounds.getY() + 9.0f);
    g.fillPath (head);
}

//==============================================================================
// Coordinate helpers

float PianoRollEditor::pitchToY (int midiPitch,
                                 juce::Rectangle<int> bounds) const noexcept
{
    const int   numSemitones = topNote_ - bottomNote_ + 1;
    const float rowH         = (float)bounds.getHeight() / (float)numSemitones;
    return (float)bounds.getY() + (float)(topNote_ - midiPitch) * rowH;
}

float PianoRollEditor::secondsToX (double sec,
                                   juce::Rectangle<int> bounds) const noexcept
{
    const double viewDuration = viewEndSec_ - viewStartSec_;
    return (float)bounds.getX()
           + (float)((sec - viewStartSec_) / viewDuration * bounds.getWidth());
}

//==============================================================================
// Public API

void PianoRollEditor::setPlayheadPosition (double positionInSeconds)
{
    playheadPos_.store (positionInSeconds);
}

void PianoRollEditor::setAudioThumbnail (juce::AudioThumbnail* thumbnail)
{
    thumbnail_ = thumbnail;
    repaint();
}

void PianoRollEditor::addNote (const MidiNote& note)
{
    {
        const juce::ScopedLock lock (notesLock_);
        notes_.add (note);
    }
    repaint();
}

void PianoRollEditor::clearNotes()
{
    {
        const juce::ScopedLock lock (notesLock_);
        notes_.clear();
    }
    repaint();
}

void PianoRollEditor::setVisibleTimeRange (double startSec, double endSec)
{
    viewStartSec_ = startSec;
    viewEndSec_   = endSec;
    repaint();
}

//==============================================================================
void PianoRollEditor::mouseWheelMove (const juce::MouseEvent& e,
                                      const juce::MouseWheelDetails& wheel)
{
    const double viewDuration = viewEndSec_ - viewStartSec_;

    if (e.mods.isAltDown())
    {
        const double zoomFactor = 1.0 - (double)wheel.deltaY * 0.45;
        zoomPitchView (juce::jlimit (0.55, 1.85, zoomFactor), yToPitch (e.y));
    }
    else if (e.mods.isCommandDown())
    {
        // Horizontal zoom — pivot at mouse X position
        const double zoomFactor  = 1.0 - (double)wheel.deltaY * 0.35;
        const double clampedZoom = juce::jlimit (0.4, 2.5, zoomFactor);
        const double mouseRelX   = (double)e.x / (double)getWidth();
        const double pivotSec    = viewStartSec_ + mouseRelX * viewDuration;
        const double newDuration = juce::jlimit (1.0, 120.0,
                                                 viewDuration * clampedZoom);

        viewStartSec_ = juce::jmax (0.0, pivotSec - mouseRelX * newDuration);
        viewEndSec_   = viewStartSec_ + newDuration;
    }
    else if (e.mods.isShiftDown())
    {
        const int scrollSemitones = (int)std::round ((double)wheel.deltaY * 6.0);
        scrollPitchView (scrollSemitones);
    }
    else
    {
        // Horizontal scroll. Trackpads may report vertical deltas during a
        // horizontal gesture, so keep pitch scrolling behind Shift.
        if (std::abs (wheel.deltaX) > 0.0f)
        {
            const double scrollSec = -(double)wheel.deltaX * viewDuration * 0.25;
            viewStartSec_ = juce::jmax (0.0, viewStartSec_ + scrollSec);
            viewEndSec_   = viewStartSec_ + viewDuration;
        }

        if (std::abs (wheel.deltaY) > 0.0f)
        {
            const double scrollSec = -(double)wheel.deltaY * viewDuration * 0.18;
            viewStartSec_ = juce::jmax (0.0, viewStartSec_ + scrollSec);
            viewEndSec_ = viewStartSec_ + viewDuration;
        }
    }

    repaint();
}

//==============================================================================
void PianoRollEditor::paintAnalyzingOverlay (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    if (! isAnalyzing_) return;

    // Semi-transparent dim
    g.setColour (juce::Colour (0x88000014));
    g.fillRect  (bounds);

    const float cx = (float)bounds.getCentreX();
    const float cy = (float)bounds.getCentreY();
    const float r  = 28.0f;

    // Background ring
    g.setColour (juce::Colour (Theme::kBorder).withAlpha (0.4f));
    juce::Path bgRing;
    bgRing.addArc (cx - r, cy - r, r * 2.0f, r * 2.0f,
                   0.0f, juce::MathConstants<float>::twoPi, true);
    g.strokePath (bgRing, juce::PathStrokeType (3.0f,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

    // Spinning arc (~120° sweep)
    const float sweep = juce::MathConstants<float>::twoPi / 3.0f;
    juce::Path arc;
    arc.addArc (cx - r, cy - r, r * 2.0f, r * 2.0f,
                spinnerAngle_, spinnerAngle_ + sweep, true);

    juce::ColourGradient grad (juce::Colour (Theme::kAccentCyan).withAlpha (0.9f),
                               cx, cy - r,
                               juce::Colour (Theme::kAccentCyan).withAlpha (0.1f),
                               cx, cy + r, false);
    g.setGradientFill (grad);
    g.strokePath (arc, juce::PathStrokeType (3.0f,
                                             juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    // Label
    g.setColour (juce::Colour (Theme::kTextSecondary));
    g.setFont   (juce::FontOptions (12.0f));
    g.drawText  ("Analysing pitch...",
                 (int)(cx - 70.0f), (int)(cy + r + 12.0f), 140, 18,
                 juce::Justification::centred, true);
}

//==============================================================================
void PianoRollEditor::paintProcessingOverlay (juce::Graphics& g,
                                              juce::Rectangle<int> bounds)
{
    if (! isProcessing_) return;

    g.setColour (juce::Colour (0x88000014));
    g.fillRect  (bounds);

    const float cx = (float)bounds.getCentreX();
    const float cy = (float)bounds.getCentreY();
    const float r  = 28.0f;

    g.setColour (juce::Colour (Theme::kBorder).withAlpha (0.4f));
    juce::Path ring;
    ring.addArc (cx - r, cy - r, r * 2.0f, r * 2.0f,
                 0.0f, juce::MathConstants<float>::twoPi, true);
    g.strokePath (ring, juce::PathStrokeType (3.0f));

    const float sweep = juce::MathConstants<float>::twoPi / 3.0f;
    juce::Path arc;
    arc.addArc (cx - r, cy - r, r * 2.0f, r * 2.0f,
                spinnerAngle_, spinnerAngle_ + sweep, true);
    juce::ColourGradient grad (juce::Colour (Theme::kAccentPurple).withAlpha (0.9f),
                               cx, cy - r,
                               juce::Colour (Theme::kAccentPurple).withAlpha (0.1f),
                               cx, cy + r, false);
    g.setGradientFill (grad);
    g.strokePath (arc, juce::PathStrokeType (3.0f,
                                             juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    g.setColour (juce::Colour (Theme::kTextSecondary));
    g.setFont   (juce::FontOptions (12.0f));
    g.drawText  ("Applying pitch correction...",
                 (int)(cx - 100.0f), (int)(cy + r + 12.0f), 200, 18,
                 juce::Justification::centred, true);
}

//==============================================================================
// Helpers

juce::Rectangle<int> PianoRollEditor::noteAreaBounds() const noexcept
{
    auto b = getLocalBounds();
    b.removeFromTop  (kTimeRulerH);
    b.removeFromLeft (kPianoKeyWidth);
    return b;
}

int PianoRollEditor::noteIndexAt (float gx, float gy) const noexcept
{
    const auto bounds = noteAreaBounds();

    const int   numSemitones = topNote_ - bottomNote_ + 1;
    const float rowH         = (float)bounds.getHeight() / (float)numSemitones;
    const double viewDur     = viewEndSec_ - viewStartSec_;

    for (int i = notes_.size() - 1; i >= 0; --i)  // topmost note last → hit first
    {
        const auto& note = notes_.getReference (i);
        if (note.midiPitch < bottomNote_ || note.midiPitch > topNote_) continue;

        const double noteEnd = note.startSeconds + note.durationSeconds;
        if (noteEnd < viewStartSec_ || note.startSeconds > viewEndSec_) continue;

        const float ny  = (float)bounds.getY() + (float)(topNote_ - note.midiPitch) * rowH;
        const float nx1 = (float)bounds.getX()
                          + (float)((note.startSeconds - viewStartSec_) / viewDur
                                    * bounds.getWidth());
        const float nx2 = (float)bounds.getX()
                          + (float)((noteEnd - viewStartSec_) / viewDur
                                    * bounds.getWidth());

        if (gx >= nx1 && gx <= nx2 && gy >= ny && gy <= ny + rowH)
            return i;
    }
    return -1;
}

juce::String PianoRollEditor::midiNoteName (int midiPitch) noexcept
{
    static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    const int octave = midiPitch / 12 - 1;
    const int semi   = midiPitch % 12;
    return juce::String (names[semi]) + juce::String (octave);
}

void PianoRollEditor::scrollPitchView (int semitones)
{
    const int range = topNote_ - bottomNote_;
    topNote_ = juce::jlimit (range, 127, topNote_ + semitones);
    bottomNote_ = topNote_ - range;
    repaint();
}

void PianoRollEditor::zoomPitchView (double factor, int pivotPitch)
{
    const int oldRange = topNote_ - bottomNote_ + 1;
    const int newRange = juce::jlimit (12, 72, (int)std::round ((double)oldRange * factor));
    const double pivotRatio = (oldRange > 1)
        ? (double)(topNote_ - pivotPitch) / (double)(oldRange - 1)
        : 0.5;

    int newTop = pivotPitch + (int)std::round (pivotRatio * (double)(newRange - 1));
    newTop = juce::jlimit (newRange - 1, 127, newTop);
    topNote_ = newTop;
    bottomNote_ = topNote_ - newRange + 1;
    repaint();
}

int PianoRollEditor::yToPitch (int y) const noexcept
{
    const auto bounds = noteAreaBounds();
    const int numSemitones = topNote_ - bottomNote_ + 1;
    const float rowH = (float)bounds.getHeight() / (float)numSemitones;
    const int offset = (int)std::floor ((float)(y - bounds.getY()) / rowH);
    return juce::jlimit (0, 127, topNote_ - offset);
}

//==============================================================================
// Mouse interaction

void PianoRollEditor::mouseMove (const juce::MouseEvent& e)
{
    const int idx = noteIndexAt ((float)e.x, (float)e.y);
    setMouseCursor (idx >= 0 ? juce::MouseCursor::UpDownLeftRightResizeCursor
                             : juce::MouseCursor::NormalCursor);
}

void PianoRollEditor::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown()) return;

    const int idx = noteIndexAt ((float)e.x, (float)e.y);
    if (idx < 0) return;

    const juce::ScopedLock lock (notesLock_);
    const auto& note = notes_.getReference (idx);

    drag_.active     = true;
    drag_.noteIdx    = idx;
    drag_.origStart  = note.startSeconds;
    drag_.origPitch  = note.midiPitch;
    drag_.origCents  = note.centsOffset;
    drag_.mouseDownX = e.getPosition().x;
    drag_.mouseDownY = e.getPosition().y;
}

void PianoRollEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag_.active) return;

    const auto bounds = noteAreaBounds();
    const int  numSemitones = topNote_ - bottomNote_ + 1;
    const float rowH        = (float)bounds.getHeight() / (float)numSemitones;
    const double viewDur    = viewEndSec_ - viewStartSec_;
    const double pxPerSec   = bounds.getWidth() / viewDur;

    // ── Vertical: cents / semitone change ────────────────────────────────────
    // 1 pixel = (100 / rowH) cents  →  rowH pixels = 1 semitone
    const int   deltaY       = drag_.mouseDownY - e.getPosition().y;  // up = positive
    const float deltaCents   = (float)deltaY * (100.0f / rowH);

    const float origTotal    = (float)drag_.origPitch * 100.0f + drag_.origCents;
    const float newTotal     = origTotal + deltaCents;
    const int   newPitch     = juce::jlimit (0, 127, (int)std::round (newTotal / 100.0f));
    const float newCents     = juce::jlimit (-50.0f, 50.0f,
                                             newTotal - (float)newPitch * 100.0f);

    // ── Horizontal: time shift ────────────────────────────────────────────────
    const int    deltaX    = e.getPosition().x - drag_.mouseDownX;
    const double deltaTime = (double)deltaX / pxPerSec;
    const double newStart  = juce::jmax (0.0, drag_.origStart + deltaTime);

    // ── Apply ─────────────────────────────────────────────────────────────────
    {
        const juce::ScopedLock lock (notesLock_);
        if (drag_.noteIdx < notes_.size())
        {
            auto& note          = notes_.getReference (drag_.noteIdx);
            note.midiPitch      = newPitch;
            note.centsOffset    = newCents;
            note.startSeconds   = newStart;
        }
    }
    repaint();
}

void PianoRollEditor::mouseUp (const juce::MouseEvent&)
{
    const bool wasDragging = drag_.active;
    drag_.active = false;
    setMouseCursor (juce::MouseCursor::NormalCursor);

    if (wasDragging && onNotesEdited)
    {
        // Snapshot notes under lock and pass to the processor pipeline
        std::vector<MidiNote> snapshot;
        {
            const juce::ScopedLock lock (notesLock_);
            snapshot.reserve ((size_t)notes_.size());
            for (const auto& n : notes_)
                snapshot.push_back (n);
        }
        onNotesEdited (snapshot);
    }

    repaint();
}

//==============================================================================
void PianoRollEditor::timerCallback()
{
    if (isAnalyzing_)
    {
        spinnerAngle_ += 0.12f;   // ~2 rev/s at 30 Hz
        if (spinnerAngle_ > juce::MathConstants<float>::twoPi)
            spinnerAngle_ -= juce::MathConstants<float>::twoPi;
    }
    repaint();
}
