#include "AnnotationEditorComponent.h"
#include "AnnotationJson.h"
#include "AnnotationValidator.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

namespace vocal_annotation
{

namespace
{
constexpr int kToolbarHeight = 42;
constexpr int kInspectorWidth = 260;
constexpr size_t kMaxUndoSnapshots = 64;
constexpr double kAutosaveDelayMs = 5000.0;
constexpr double kTwoPi = juce::MathConstants<double>::twoPi;
constexpr int kMaxPlaybackNotes = 4096;
constexpr int kMaxPlaybackCurvePoints = 24;

enum class PlaybackMode
{
    notesAndSound = 1,
    notesOnly = 2
};

struct PlaybackNote
{
    double start = 0.0;
    double end = 0.0;
    double frequency = 440.0;
    bool legatoFromPrevious = false;
    bool legatoToNext = false;
    std::array<PitchCurvePoint, kMaxPlaybackCurvePoints> curve {};
    int curveCount = 0;
};

struct PlaybackNoteState
{
    std::array<PlaybackNote, kMaxPlaybackNotes> notes {};
    int count = 0;
};

struct LyricSyllable
{
    juce::String text;
    bool startsWord = true;
    bool endsWord = true;
};

struct TimedLyricSyllable
{
    juce::String text;
    double start = 0.0;
    double end = 0.0;
    BoundaryKind kind = BoundaryKind::syllable;
    double confidence = 0.5;
};

struct AnalysisWindow
{
    double start = 0.0;
    double end = 0.0;
};

bool isNonVocalBoundary(BoundaryKind kind)
{
    return kind == BoundaryKind::breath
        || kind == BoundaryKind::noise
        || kind == BoundaryKind::pause;
}

int removeMicroNonVocalBoundaries(std::vector<BoundaryMarker>& boundaries,
                                  double minimumNonVocalDuration = 0.08,
                                  double minimumMusicalDuration = 0.05)
{
    std::sort(boundaries.begin(), boundaries.end(),
              [](const auto& a, const auto& b) { return a.time < b.time; });

    auto removed = 0;
    for (size_t i = 0; i + 1 < boundaries.size();)
    {
        const auto& left = boundaries[i];
        const auto& right = boundaries[i + 1];
        const auto leftIsNonVocal = isNonVocalBoundary(left.kind);
        const auto rightIsNonVocal = isNonVocalBoundary(right.kind);
        const auto minimumDuration = leftIsNonVocal || rightIsNonVocal
            ? minimumNonVocalDuration
            : minimumMusicalDuration;
        if (right.time - left.time >= minimumDuration)
        {
            ++i;
            continue;
        }

        if (! leftIsNonVocal && ! rightIsNonVocal)
        {
            const auto hasLyricText = [](const BoundaryMarker& boundary)
            {
                const auto text = boundary.text.trim();
                return text.isNotEmpty() && text != toString(boundary.kind);
            };
            const auto leftHasLyric = hasLyricText(left);
            const auto rightHasLyric = hasLyricText(right);
            if (leftHasLyric && rightHasLyric && left.text.trim() != right.text.trim())
            {
                ++i;
                continue;
            }

            // Generic close candidates describe the same onset. Keep the
            // earlier one so an unvoiced consonant remains part of the syllable.
            const auto eraseIndex = leftHasLyric != rightHasLyric
                ? (leftHasLyric ? i + 1 : i)
                : i + 1;
            boundaries.erase(boundaries.begin() + static_cast<std::ptrdiff_t>(eraseIndex));
            ++removed;
            if (i > 0)
                --i;
            continue;
        }

        auto eraseIndex = leftIsNonVocal ? i : i + 1;
        if (leftIsNonVocal && rightIsNonVocal)
            eraseIndex = left.confidence < right.confidence ? i : i + 1;

        boundaries.erase(boundaries.begin() + static_cast<std::ptrdiff_t>(eraseIndex));
        ++removed;
        if (i > 0)
            --i;
    }

    return removed;
}

bool isVocalBoundary(BoundaryKind kind)
{
    return kind == BoundaryKind::syllable
        || kind == BoundaryKind::rearticulation
        || kind == BoundaryKind::legato;
}

bool isNonVocalRegionKind(const juce::String& kind)
{
    return kind == "breath" || kind == "noise" || kind == "pause";
}

int normalizeNonVocalRegions(std::vector<AnnotationRegion>& regions,
                             std::vector<BoundaryMarker>& boundaries,
                             double documentDuration,
                             double mergeGapSeconds = 0.20)
{
    std::vector<AnnotationRegion> nonVocal;
    std::vector<AnnotationRegion> other;
    nonVocal.reserve(regions.size());
    other.reserve(regions.size());

    for (const auto& region : regions)
    {
        if (isNonVocalRegionKind(region.kind))
        {
            auto clipped = region;
            clipped.start = juce::jlimit(0.0, documentDuration, clipped.start);
            clipped.end = juce::jlimit(clipped.start, documentDuration, clipped.end);
            if (clipped.end - clipped.start >= 0.04)
                nonVocal.push_back(std::move(clipped));
        }
        else
        {
            other.push_back(region);
        }
    }

    if (nonVocal.empty())
        return 0;

    std::sort(nonVocal.begin(), nonVocal.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    std::vector<AnnotationRegion> merged;
    merged.reserve(nonVocal.size());
    auto mergedCount = 0;

    for (const auto& region : nonVocal)
    {
        if (merged.empty()
            || region.kind != merged.back().kind
            || region.start > merged.back().end + mergeGapSeconds)
        {
            merged.push_back(region);
            continue;
        }

        auto& last = merged.back();
        last.end = juce::jmax(last.end, region.end);
        ++mergedCount;
    }

    boundaries.erase(std::remove_if(boundaries.begin(),
                                    boundaries.end(),
                                    [&merged](const auto& boundary)
                                    {
                                        if (! isVocalBoundary(boundary.kind))
                                            return false;

                                        return std::any_of(merged.begin(), merged.end(),
                                                           [&boundary](const auto& region)
                                                           {
                                                               return boundary.time > region.start + 0.015
                                                                   && boundary.time < region.end - 0.015;
                                                           });
                                    }),
                     boundaries.end());

    boundaries.erase(std::remove_if(boundaries.begin(),
                                    boundaries.end(),
                                    [](const auto& boundary)
                                    {
                                        return isNonVocalBoundary(boundary.kind)
                                            && boundary.source == "gtsinger_tcn";
                                    }),
                     boundaries.end());

    std::sort(boundaries.begin(), boundaries.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    regions = std::move(other);
    regions.insert(regions.end(), merged.begin(), merged.end());
    return mergedCount;
}

int collapseShortNoteGaps(std::vector<NoteBlock>& notes,
                          std::vector<BoundaryMarker>& boundaries,
                          double maximumGap = 0.12)
{
    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });

    auto collapsed = 0;
    for (size_t i = 1; i < notes.size(); ++i)
    {
        auto& previous = notes[i - 1];
        auto& current = notes[i];
        const auto gapStart = previous.end;
        const auto gapEnd = current.start;
        const auto gap = gapEnd - gapStart;
        if (gap <= 0.0 || gap > maximumGap)
            continue;

        const auto sharedBoundary = (gapStart + gapEnd) * 0.5;
        previous.end = sharedBoundary;
        previous.voicedEnd = juce::jlimit(previous.voicedStart, previous.end, previous.voicedEnd);
        current.start = sharedBoundary;
        current.voicedStart = juce::jlimit(current.start, current.voicedEnd, current.voicedStart);

        boundaries.erase(std::remove_if(boundaries.begin(), boundaries.end(),
                                        [gapStart, gapEnd](const auto& boundary)
                                        {
                                            return isNonVocalBoundary(boundary.kind)
                                                && boundary.time >= gapStart - 0.001
                                                && boundary.time <= gapEnd + 0.001;
                                        }),
                         boundaries.end());
        ++collapsed;
    }

    return collapsed;
}

std::vector<AnalysisWindow> buildVocalPitchAnalysisWindows(const AnnotationDocument& document,
                                                           double paddingSeconds = 0.06,
                                                           double mergeGapSeconds = 0.18,
                                                           double minimumWindowSeconds = 0.12)
{
    struct Interval
    {
        double start = 0.0;
        double end = 0.0;
    };

    std::vector<Interval> excluded;
    for (const auto& region : document.regions)
    {
        if (region.kind != "breath" && region.kind != "noise" && region.kind != "pause")
            continue;

        const auto start = juce::jlimit(0.0, document.duration, region.start);
        const auto end = juce::jlimit(start, document.duration, region.end);
        if (end - start >= 0.04)
            excluded.push_back({ start, end });
    }

    if (excluded.empty() || document.duration <= 0.0)
        return {};

    std::sort(excluded.begin(), excluded.end(), [](const auto& a, const auto& b) { return a.start < b.start; });

    std::vector<Interval> mergedExcluded;
    for (const auto& interval : excluded)
    {
        if (mergedExcluded.empty() || interval.start > mergedExcluded.back().end + 0.01)
        {
            mergedExcluded.push_back(interval);
            continue;
        }

        mergedExcluded.back().end = juce::jmax(mergedExcluded.back().end, interval.end);
    }

    std::vector<AnalysisWindow> windows;
    auto cursor = 0.0;
    for (const auto& interval : mergedExcluded)
    {
        if (interval.start > cursor)
        {
            const auto start = juce::jlimit(0.0, document.duration, cursor - paddingSeconds);
            const auto end = juce::jlimit(start, document.duration, interval.start + paddingSeconds);
            if (end - start >= minimumWindowSeconds)
                windows.push_back({ start, end });
        }

        cursor = juce::jmax(cursor, interval.end);
    }

    if (cursor < document.duration)
    {
        const auto start = juce::jlimit(0.0, document.duration, cursor - paddingSeconds);
        const auto end = document.duration;
        if (end - start >= minimumWindowSeconds)
            windows.push_back({ start, end });
    }

    if (windows.empty())
        return {};

    std::vector<AnalysisWindow> mergedWindows;
    for (const auto& window : windows)
    {
        if (mergedWindows.empty() || window.start > mergedWindows.back().end + mergeGapSeconds)
        {
            mergedWindows.push_back(window);
            continue;
        }

        mergedWindows.back().end = juce::jmax(mergedWindows.back().end, window.end);
    }

    auto coveredDuration = 0.0;
    for (const auto& window : mergedWindows)
        coveredDuration += juce::jmax(0.0, window.end - window.start);

    // If AI Parts did not carve out meaningful non-vocal sections, keep the old
    // full-file path. It avoids edge effects without buying any real speed.
    if (coveredDuration >= document.duration * 0.94)
        return {};

    return mergedWindows;
}

juce::String analysisPythonExecutable()
{
    const auto root = juce::File(SYNTHETIC_OBSIDIAN_ROOT);
   #if JUCE_WINDOWS
    const auto venvPython = root.getChildFile("research")
                                .getChildFile(".venv_seed_vc")
                                .getChildFile("Scripts")
                                .getChildFile("python.exe");
    if (venvPython.existsAsFile())
        return venvPython.getFullPathName();
    return "python";
   #else
    const juce::File anacondaPython("/opt/anaconda3/bin/python");
    if (anacondaPython.existsAsFile())
        return anacondaPython.getFullPathName();

    const auto venvPython = root.getChildFile("research")
                                .getChildFile(".venv_seed_vc")
                                .getChildFile("bin")
                                .getChildFile("python");
    if (venvPython.existsAsFile())
        return venvPython.getFullPathName();
    return "python3";
   #endif
}

struct AcousticBoundaryCandidate
{
    double time = 0.0;
    double strength = 0.0;
};

juce::String resultMessage(const juce::String& action, const juce::Result& result)
{
    return result.wasOk() ? action + " complete." : action + " failed: " + result.getErrorMessage();
}

double midiNoteToFrequency(double midiNote)
{
    return 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
}

double playbackMidiAtTime(const PlaybackNote& note, double time)
{
    if (note.curveCount <= 0)
        return 69.0 + 12.0 * std::log2(note.frequency / 440.0);

    if (time <= note.curve[0].time)
        return note.curve[0].midi;

    for (int i = 1; i < note.curveCount; ++i)
    {
        const auto& previous = note.curve[static_cast<size_t>(i - 1)];
        const auto& next = note.curve[static_cast<size_t>(i)];
        if (time <= next.time)
        {
            const auto proportion = juce::jlimit(0.0, 1.0, (time - previous.time) / juce::jmax(0.001, next.time - previous.time));
            return previous.midi + (next.midi - previous.midi) * proportion;
        }
    }

    return note.curve[static_cast<size_t>(note.curveCount - 1)].midi;
}

bool isLyricLetter(juce_wchar character)
{
    return juce::CharacterFunctions::isLetter(character) || character == '\'';
}

bool isVowel(juce_wchar character, int index)
{
    const auto lower = juce::CharacterFunctions::toLowerCase(character);
    return lower == 'a' || lower == 'e' || lower == 'i' || lower == 'o' || lower == 'u' || (lower == 'y' && index > 0);
}

juce::String cleanLyricWord(const juce::String& token)
{
    juce::String result;
    for (int i = 0; i < token.length(); ++i)
        if (isLyricLetter(token[i]))
            result += juce::String::charToString(token[i]);

    return result.trimCharactersAtStart("'").trimCharactersAtEnd("'");
}

std::vector<juce::String> splitWordIntoSyllables(const juce::String& word)
{
    std::vector<std::pair<int, int>> vowelGroups;
    for (int i = 0; i < word.length();)
    {
        if (! isVowel(word[i], i))
        {
            ++i;
            continue;
        }

        const auto start = i;
        while (i + 1 < word.length() && isVowel(word[i + 1], i + 1))
            ++i;
        vowelGroups.push_back({ start, i });
        ++i;
    }

    if (vowelGroups.size() > 1)
    {
        const auto finalIndex = word.length() - 1;
        const auto lastGroup = vowelGroups.back();
        const auto lastIsSilentE = lastGroup.first == finalIndex
                                  && lastGroup.second == finalIndex
                                  && juce::CharacterFunctions::toLowerCase(word[finalIndex]) == 'e';
        if (lastIsSilentE)
            vowelGroups.pop_back();
    }

    if (vowelGroups.size() <= 1)
        return { word };

    std::vector<int> splitPositions;
    for (int i = 0; i + 1 < static_cast<int>(vowelGroups.size()); ++i)
    {
        const auto previousVowelEnd = vowelGroups[static_cast<size_t>(i)].second;
        const auto nextVowelStart = vowelGroups[static_cast<size_t>(i + 1)].first;
        const auto consonantCount = nextVowelStart - previousVowelEnd - 1;
        const auto split = consonantCount >= 2 ? nextVowelStart - 1 : juce::jmax(previousVowelEnd + 1, nextVowelStart - 1);
        if (split > 0 && split < word.length())
            splitPositions.push_back(split);
    }

    std::vector<juce::String> syllables;
    int start = 0;
    for (const auto split : splitPositions)
    {
        auto syllable = word.substring(start, split);
        if (syllable.isNotEmpty())
            syllables.push_back(syllable);
        start = split;
    }

    auto last = word.substring(start);
    if (last.isNotEmpty())
        syllables.push_back(last);

    return syllables.empty() ? std::vector<juce::String>{ word } : syllables;
}

std::vector<LyricSyllable> lyricSyllablesFromText(const juce::String& text)
{
    std::vector<LyricSyllable> result;
    const auto roughTokens = juce::StringArray::fromTokens(text, " \t\r\n\".,;:!?()[]{}", "");
    for (const auto& token : roughTokens)
    {
        const auto word = cleanLyricWord(token);
        if (word.isEmpty())
            continue;

        const auto syllables = splitWordIntoSyllables(word);
        for (int i = 0; i < static_cast<int>(syllables.size()); ++i)
        {
            LyricSyllable lyric;
            lyric.text = syllables[static_cast<size_t>(i)];
            lyric.startsWord = i == 0;
            lyric.endsWord = i + 1 == static_cast<int>(syllables.size());

            if (! lyric.endsWord)
                lyric.text << "-";
            if (! lyric.startsWord)
                lyric.text = "-" + lyric.text;

            result.push_back(std::move(lyric));
        }
    }

    return result;
}
} // namespace

class MainComponent final : public juce::AudioAppComponent,
                            private AnnotationEditorComponent::Listener,
                            private juce::Timer
{
public:
    MainComponent()
        : editor_(formatManager_, document_)
    {
        formatManager_.registerBasicFormats();
        editor_.setListener(this);

        addToolbarButton(openAudioButton_, "Open Audio", [this] { chooseAudioFile(); });
        addToolbarButton(openInstrumentalButton_, "Open Inst", [this] { chooseInstrumentalFile(); });
        addToolbarButton(loadJsonButton_, "Load JSON", [this] { chooseJsonFile(); });
        addToolbarButton(loadLyricsButton_, "Lyrics", [this] { chooseLyricsFile(); });
        addToolbarButton(analyzeButton_, "Analyze", [this] { runCombinedAnalysis(); });
        addToolbarButton(aiPartsButton_, "AI Parts", [this] { runAiPartsAnalysis(); });
        addToolbarButton(playButton_, "Play", [this] { togglePlayback(); });
        addToolbarButton(saveJsonButton_, "Save JSON", [this] { saveJson(); });
        addToolbarButton(undoButton_, "Undo", [this] { undo(); });
        addToolbarButton(redoButton_, "Redo", [this] { redo(); });
        addToolbarButton(exportMidiButton_, "Export MIDI", [this] { exportMidi(); });
        addToolbarButton(validateButton_, "Validate", [this] { updateValidationStatus(); });
        addToolbarButton(fitButton_, "Fit", [this] { editor_.fitToClip(); });

        configureSlider(horizontalZoomSlider_, 1.0, 32.0, 1.0, [this] { editor_.setHorizontalZoom(horizontalZoomSlider_.getValue()); });
        configureSlider(pitchZoomSlider_, 1.0, 6.0, 3.0, [this] { editor_.setPitchZoom(pitchZoomSlider_.getValue()); });

        playbackModeBox_.addItem("Notes + Sound", static_cast<int>(PlaybackMode::notesAndSound));
        playbackModeBox_.addItem("Notes Only", static_cast<int>(PlaybackMode::notesOnly));
        playbackModeBox_.setSelectedId(static_cast<int>(PlaybackMode::notesAndSound), juce::dontSendNotification);
        playbackModeBox_.onChange = [this]
        {
            playbackMode_.store(playbackModeBox_.getSelectedId());
        };
        addAndMakeVisible(playbackModeBox_);

        configureTextEditor(bpmEditor_, "120.0", [this]
        {
            beginUndoableAction();
            document_.bpm = juce::jlimit(20.0, 300.0, bpmEditor_.getText().getDoubleValue());
            markChanged();
        });
        configureTextEditor(keyEditor_, "C major", [this]
        {
            beginUndoableAction();
            document_.key = keyEditor_.getText().trim();
            markChanged();
        });

        boundaryKindBox_.addItemList(allBoundaryKindNames(), 1);
        boundaryKindBox_.setSelectedId(1, juce::dontSendNotification);
        boundaryKindBox_.onChange = [this]
        {
            editor_.setSelectedBoundaryKind(boundaryKindFromString(boundaryKindBox_.getText()));
        };
        addAndMakeVisible(boundaryKindBox_);

        configureTextEditor(selectionTextEditor_, {}, [this] { applySelectionText(); });
        selectionTextEditor_.setTextToShowWhenEmpty("Selected lyric / boundary text", juce::Colour(0xff6f7887));

        statusLabel_.setJustificationType(juce::Justification::topLeft);
        statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd8dee8));
        statusLabel_.setText("Open an audio file to create a draft annotation.", juce::dontSendNotification);
        addAndMakeVisible(statusLabel_);

        infoLabel_.setJustificationType(juce::Justification::topLeft);
        infoLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffaeb6c4));
        addAndMakeVisible(infoLabel_);

        addAndMakeVisible(editor_);
        setWantsKeyboardFocus(true);
        setSize(1280, 720);
        resetHistory();
        updatePlaybackNotes();
        startTimerHz(30);
        setAudioChannels(0, 2);
    }

    ~MainComponent() override
    {
        stopTimer();
        transport_.stop();
        instrumentalTransport_.stop();
        transport_.setSource(nullptr);
        instrumentalTransport_.setSource(nullptr);
        activeNoteState_.store(nullptr);
        readerSource_.reset();
        instrumentalReaderSource_.reset();
        shutdownAudio();
    }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        audioSampleRate_.store(sampleRate);
        instrumentalMixBuffer_.setSize(2, samplesPerBlockExpected, false, false, true);
        transport_.prepareToPlay(samplesPerBlockExpected, sampleRate);
        instrumentalTransport_.prepareToPlay(samplesPerBlockExpected, sampleRate);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        const auto playheadStart = transport_.getCurrentPosition();

        if (readerSource_ != nullptr)
            transport_.getNextAudioBlock(bufferToFill);
        else
            bufferToFill.clearActiveBufferRegion();

        const auto mode = static_cast<PlaybackMode>(playbackMode_.load());
        if (mode == PlaybackMode::notesOnly)
            bufferToFill.clearActiveBufferRegion();

        if (transport_.isPlaying() && (mode == PlaybackMode::notesOnly || mode == PlaybackMode::notesAndSound))
            renderTimelineNotes(bufferToFill, playheadStart);

        if (mode == PlaybackMode::notesAndSound)
            mixInstrumentalTrack(bufferToFill);

        renderClickedNoteTone(bufferToFill);
    }

    void releaseResources() override
    {
        transport_.releaseResources();
        instrumentalTransport_.releaseResources();
    }

    void mixInstrumentalTrack(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        if (instrumentalReaderSource_ == nullptr || ! instrumentalTransport_.isPlaying() || bufferToFill.buffer == nullptr)
            return;

        const auto channels = bufferToFill.buffer->getNumChannels();
        if (instrumentalMixBuffer_.getNumChannels() < channels || instrumentalMixBuffer_.getNumSamples() < bufferToFill.numSamples)
            return;

        instrumentalMixBuffer_.clear();
        juce::AudioSourceChannelInfo instrumentalInfo(&instrumentalMixBuffer_, 0, bufferToFill.numSamples);
        instrumentalTransport_.getNextAudioBlock(instrumentalInfo);

        for (int channel = 0; channel < channels; ++channel)
            bufferToFill.buffer->addFrom(channel,
                                         bufferToFill.startSample,
                                         instrumentalMixBuffer_,
                                         channel,
                                         0,
                                         bufferToFill.numSamples,
                                         0.7f);
    }

    void renderTimelineNotes(const juce::AudioSourceChannelInfo& bufferToFill, double playheadStart)
    {
        auto* buffer = bufferToFill.buffer;
        if (buffer == nullptr)
            return;

        const auto* state = activeNoteState_.load();
        if (state == nullptr || state->count <= 0)
            return;

        const auto sampleRate = audioSampleRate_.load();
        if (sampleRate <= 0.0)
            return;

        const auto channels = buffer->getNumChannels();
        const auto startSample = bufferToFill.startSample;
        const auto numSamples = bufferToFill.numSamples;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto time = playheadStart + static_cast<double>(sample) / sampleRate;
            const PlaybackNote* activeNote = nullptr;
            for (int i = 0; i < state->count; ++i)
            {
                const auto& note = state->notes[static_cast<size_t>(i)];
                if (time >= note.start && time < note.end)
                {
                    activeNote = &note;
                    break;
                }
            }

            if (activeNote == nullptr)
            {
                timelineTonePhase_ = 0.0;
                continue;
            }

            const auto fadeSeconds = 0.01;
            const auto attack = activeNote->legatoFromPrevious
                ? 1.0
                : juce::jlimit(0.0, 1.0, (time - activeNote->start) / fadeSeconds);
            const auto release = activeNote->legatoToNext
                ? 1.0
                : juce::jlimit(0.0, 1.0, (activeNote->end - time) / fadeSeconds);
            const auto gain = 0.15f * static_cast<float>(juce::jmin(attack, release));
            const auto frequency = midiNoteToFrequency(playbackMidiAtTime(*activeNote, time));
            const auto value = std::sin(timelineTonePhase_) * gain;
            timelineTonePhase_ += kTwoPi * frequency / sampleRate;
            if (timelineTonePhase_ >= kTwoPi)
                timelineTonePhase_ = std::fmod(timelineTonePhase_, kTwoPi);

            for (int channel = 0; channel < channels; ++channel)
                buffer->addSample(channel, startSample + sample, static_cast<float>(value));
        }
    }

    void renderClickedNoteTone(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        auto* buffer = bufferToFill.buffer;
        if (buffer == nullptr)
            return;

        const auto serial = clickedToneSerial_.load();
        if (serial != renderedClickedToneSerial_)
        {
            renderedClickedToneSerial_ = serial;
            clickedTonePhase_ = 0.0;
            clickedToneElapsedSamples_ = 0;
        }

        auto remaining = clickedToneSamplesRemaining_.load();
        if (remaining <= 0)
            return;

        const auto totalSamples = juce::jmax(1, clickedToneTotalSamples_.load());
        const auto sampleRate = audioSampleRate_.load();
        if (sampleRate <= 0.0)
            return;

        const auto frequency = clickedToneFrequency_.load();
        const auto channels = buffer->getNumChannels();

        for (int sample = 0; sample < bufferToFill.numSamples && remaining > 0; ++sample)
        {
            const auto attack = juce::jlimit(0.0, 1.0, static_cast<double>(clickedToneElapsedSamples_) / (sampleRate * 0.01));
            const auto release = juce::jlimit(0.0, 1.0, static_cast<double>(remaining) / (sampleRate * 0.04));
            const auto taper = 1.0 - static_cast<double>(clickedToneElapsedSamples_) / static_cast<double>(totalSamples);
            const auto gain = 0.22f * static_cast<float>(juce::jmin(attack, release) * juce::jmax(0.0, taper));
            const auto value = static_cast<float>(std::sin(clickedTonePhase_) * gain);

            clickedTonePhase_ += kTwoPi * frequency / sampleRate;
            if (clickedTonePhase_ >= kTwoPi)
                clickedTonePhase_ = std::fmod(clickedTonePhase_, kTwoPi);

            for (int channel = 0; channel < channels; ++channel)
                buffer->addSample(channel, bufferToFill.startSample + sample, value);

            --remaining;
            ++clickedToneElapsedSamples_;
        }

        clickedToneSamplesRemaining_.store(remaining);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto toolbar = bounds.removeFromTop(kToolbarHeight).reduced(6, 5);
        constexpr int kToolbarControlsWidth = 352;
        const auto buttonWidth = juce::jlimit(
            64,
            82,
            (toolbar.getWidth() - kToolbarControlsWidth)
                / juce::jmax(1, static_cast<int>(toolbarButtons_.size())));

        for (auto* button : toolbarButtons_)
            button->setBounds(toolbar.removeFromLeft(buttonWidth).reduced(2, 0));

        toolbar.removeFromLeft(10);
        horizontalZoomSlider_.setBounds(toolbar.removeFromLeft(110).reduced(4, 0));
        pitchZoomSlider_.setBounds(toolbar.removeFromLeft(110).reduced(4, 0));
        playbackModeBox_.setBounds(toolbar.removeFromLeft(122).reduced(4, 0));

        auto inspector = bounds.removeFromRight(kInspectorWidth).reduced(8);
        infoLabel_.setBounds(inspector.removeFromTop(96));
        inspector.removeFromTop(8);
        bpmEditor_.setBounds(inspector.removeFromTop(28));
        inspector.removeFromTop(8);
        keyEditor_.setBounds(inspector.removeFromTop(28));
        inspector.removeFromTop(8);
        boundaryKindBox_.setBounds(inspector.removeFromTop(28));
        inspector.removeFromTop(8);
        selectionTextEditor_.setBounds(inspector.removeFromTop(58));
        inspector.removeFromTop(12);
        statusLabel_.setBounds(inspector);

        editor_.setBounds(bounds);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0))
        {
            saveJson();
            return true;
        }

        if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0))
        {
            undo();
            return true;
        }

        if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
            || key == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0))
        {
            redo();
            return true;
        }

        if (key == juce::KeyPress::spaceKey)
        {
            togglePlayback();
            return true;
        }

        if (editor_.keyPressed(key))
            return true;

        return false;
    }

private:
    void addToolbarButton(juce::TextButton& button, const juce::String& text, std::function<void()> onClick)
    {
        button.setButtonText(text);
        button.onClick = std::move(onClick);
        toolbarButtons_.push_back(&button);
        addAndMakeVisible(button);
    }

    void configureSlider(juce::Slider& slider, double min, double max, double value, std::function<void()> onChange)
    {
        slider.setRange(min, max, 0.01);
        slider.setValue(value, juce::dontSendNotification);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 20);
        slider.onValueChange = std::move(onChange);
        addAndMakeVisible(slider);
    }

    void configureTextEditor(juce::TextEditor& editor, const juce::String& text, std::function<void()> onChange)
    {
        editor.setText(text, false);
        editor.setSelectAllWhenFocused(true);
        editor.onReturnKey = onChange;
        editor.onFocusLost = std::move(onChange);
        addAndMakeVisible(editor);
    }

    void resetHistory()
    {
        undoStack_.clear();
        redoStack_.clear();
        undoButton_.setEnabled(false);
        redoButton_.setEnabled(false);
    }

    void updatePlaybackNotes()
    {
        auto& state = noteStates_[static_cast<size_t>(writeNoteStateIndex_)];
        state.count = 0;
        for (const auto& note : document_.notes)
        {
            if (note.end <= note.start || state.count >= kMaxPlaybackNotes)
                continue;

            auto& playbackNote = state.notes[static_cast<size_t>(state.count++)];
            playbackNote.start = note.start;
            playbackNote.end = note.end;
            playbackNote.frequency = midiNoteToFrequency(note.pitchExact);
            playbackNote.legatoFromPrevious = note.flags.contains("legato_from_previous");
            playbackNote.legatoToNext = note.flags.contains("legato_to_next");
            playbackNote.curveCount = 0;

            if (note.curve.empty())
            {
                playbackNote.curve[0] = { note.start, note.pitchExact, 1.0 };
                playbackNote.curve[1] = { note.end, note.pitchExact, 1.0 };
                playbackNote.curveCount = 2;
            }
            else
            {
                for (const auto& point : note.curve)
                {
                    if (point.time < note.start || point.time > note.end)
                        continue;

                    if (playbackNote.curveCount >= kMaxPlaybackCurvePoints)
                        break;

                    playbackNote.curve[static_cast<size_t>(playbackNote.curveCount++)] = point;
                }

                if (playbackNote.curveCount == 0)
                {
                    playbackNote.curve[0] = { note.start, note.pitchExact, 1.0 };
                    playbackNote.curve[1] = { note.end, note.pitchExact, 1.0 };
                    playbackNote.curveCount = 2;
                }
            }

            std::sort(playbackNote.curve.begin(),
                      playbackNote.curve.begin() + playbackNote.curveCount,
                      [](const auto& a, const auto& b) { return a.time < b.time; });
        }

        activeNoteState_.store(&state);
        writeNoteStateIndex_ = 1 - writeNoteStateIndex_;
    }

    void beginUndoableAction() override
    {
        if (restoringHistory_)
            return;

        undoStack_.push_back(document_);
        if (undoStack_.size() > kMaxUndoSnapshots)
            undoStack_.erase(undoStack_.begin());

        redoStack_.clear();
        undoButton_.setEnabled(true);
        redoButton_.setEnabled(false);
    }

    void undo()
    {
        if (undoStack_.empty())
            return;

        restoringHistory_ = true;
        redoStack_.push_back(document_);
        document_ = undoStack_.back();
        undoStack_.pop_back();
        restoringHistory_ = false;

        finishHistoryRestore();
    }

    void redo()
    {
        if (redoStack_.empty())
            return;

        restoringHistory_ = true;
        undoStack_.push_back(document_);
        document_ = redoStack_.back();
        redoStack_.pop_back();
        restoringHistory_ = false;

        finishHistoryRestore();
    }

    void finishHistoryRestore()
    {
        ++documentRevision_;
        undoButton_.setEnabled(! undoStack_.empty());
        redoButton_.setEnabled(! redoStack_.empty());
        dirty_ = true;
        editor_.clearSelection();
        if (document_.audioFile.existsAsFile())
            editor_.setAudioFile(document_.audioFile);
        if (document_.instrumentalFile.existsAsFile())
        {
            editor_.setInstrumentalFile(document_.instrumentalFile);
            configureInstrumentalTransportForFile(document_.instrumentalFile);
            if (document_.tempoSegments.empty() && document_.timeSignatures.empty() && document_.chords.empty())
                runMusicalContextAnalysis();
        }
        else
        {
            clearInstrumentalTrack();
        }
        updatePlaybackNotes();
        syncInspector();
        setStatus("History restored.");
    }

    void clearInstrumentalTrack()
    {
        instrumentalTransport_.stop();
        instrumentalTransport_.setSource(nullptr);
        instrumentalReaderSource_.reset();
        editor_.setInstrumentalFile({});
    }

    void configureTransportForFile(const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
        if (reader == nullptr)
        {
            deviceManager.closeAudioDevice();
            transport_.stop();
            transport_.setSource(nullptr);
            readerSource_.reset();
            deviceManager.restartLastAudioDevice();
            return;
        }

        const auto sampleRate = reader->sampleRate;
        auto nextReaderSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);

        deviceManager.closeAudioDevice();
        transport_.stop();
        transport_.setSource(nullptr);
        readerSource_ = std::move(nextReaderSource);
        transport_.setSource(readerSource_.get(), 0, nullptr, sampleRate);
        deviceManager.restartLastAudioDevice();
        playButton_.setButtonText("Play");
    }

    void configureInstrumentalTransportForFile(const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
        if (reader == nullptr)
        {
            deviceManager.closeAudioDevice();
            instrumentalTransport_.stop();
            instrumentalTransport_.setSource(nullptr);
            instrumentalReaderSource_.reset();
            deviceManager.restartLastAudioDevice();
            return;
        }

        const auto sampleRate = reader->sampleRate;
        auto nextReaderSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);

        deviceManager.closeAudioDevice();
        instrumentalTransport_.stop();
        instrumentalTransport_.setSource(nullptr);
        instrumentalReaderSource_ = std::move(nextReaderSource);
        instrumentalTransport_.setSource(instrumentalReaderSource_.get(), 0, nullptr, sampleRate);
        deviceManager.restartLastAudioDevice();
    }

    void togglePlayback()
    {
        if (readerSource_ == nullptr && instrumentalReaderSource_ == nullptr)
        {
            setStatus("Open an audio file before playback.");
            return;
        }

        if (transport_.isPlaying() || instrumentalTransport_.isPlaying())
        {
            transport_.stop();
            instrumentalTransport_.stop();
            loopAuditionActive_ = false;
            playButton_.setButtonText("Play");
        }
        else
        {
            loopAuditionActive_ = false;
            if (transport_.getCurrentPosition() >= juce::jmax(0.0, document_.duration - 0.01))
                transport_.setPosition(0.0);
            instrumentalTransport_.setPosition(transport_.getCurrentPosition());
            if (readerSource_ != nullptr)
                transport_.start();
            if (instrumentalReaderSource_ != nullptr)
                instrumentalTransport_.start();
            playButton_.setButtonText("Stop");
        }
    }

    void auditionNote(const NoteBlock& note)
    {
        loopAuditionActive_ = false;
        if (transport_.isPlaying())
            transport_.stop();
        if (instrumentalTransport_.isPlaying())
            instrumentalTransport_.stop();

        clickedToneFrequency_.store(midiNoteToFrequency(note.pitchExact));
        const auto totalSamples = juce::jmax(1, static_cast<int>(audioSampleRate_.load() * 0.55));
        clickedToneTotalSamples_.store(totalSamples);
        clickedToneSamplesRemaining_.store(totalSamples);
        clickedToneSerial_.fetch_add(1);
        playButton_.setButtonText("Play");
        setStatus("Auditioning note " + note.id + " at MIDI " + juce::String(note.pitchExact, 2) + ".");
    }

    void auditionWaveformLoop(double startTime, double endTime)
    {
        if (readerSource_ == nullptr)
        {
            setStatus("Open an audio file before auditioning waveform parts.");
            return;
        }

        loopStartTime_ = juce::jlimit(0.0, document_.duration, startTime);
        loopEndTime_ = juce::jlimit(loopStartTime_, document_.duration, endTime);
        if (loopEndTime_ - loopStartTime_ < 0.02)
            loopEndTime_ = juce::jmin(document_.duration, loopStartTime_ + 0.02);
        loopAuditionActive_ = true;
        transport_.setPosition(loopStartTime_);
        instrumentalTransport_.setPosition(loopStartTime_);
        if (readerSource_ != nullptr)
            transport_.start();
        if (instrumentalReaderSource_ != nullptr)
            instrumentalTransport_.start();
        playButton_.setButtonText("Stop");
        setStatus("Looping selected part from " + juce::String(loopStartTime_, 3) + "s to " + juce::String(loopEndTime_, 3) + "s.");
    }

    void timerCallback() override
    {
        const auto transportPosition = readerSource_ != nullptr ? transport_.getCurrentPosition() : instrumentalTransport_.getCurrentPosition();
        const auto playhead = juce::jlimit(0.0, juce::jmax(0.0, document_.duration), transportPosition);
        editor_.setPlayheadTime(playhead);

        if (transport_.isPlaying() && loopAuditionActive_)
        {
            if (playhead >= loopEndTime_ - 0.004)
            {
                transport_.setPosition(loopStartTime_);
                instrumentalTransport_.setPosition(loopStartTime_);
            }
        }
        else if (transport_.isPlaying() && document_.duration > 0.0 && playhead >= document_.duration - 0.005)
        {
            transport_.stop();
            instrumentalTransport_.stop();
            playButton_.setButtonText("Play");
        }

        maybeAutosave();
    }

    void maybeAutosave()
    {
        if (! dirty_)
            return;

        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastDirtyTimeMs_ < kAutosaveDelayMs)
            return;

        const auto baseFile = currentJsonFile_ != juce::File()
            ? currentJsonFile_
            : (document_.audioFile.existsAsFile() ? document_.audioFile.withFileExtension(".annotation.json") : juce::File());
        if (baseFile == juce::File())
            return;

        const auto autosaveFile = baseFile.getSiblingFile(baseFile.getFileNameWithoutExtension() + ".autosave.json");
        const auto result = AnnotationJson::save(document_, autosaveFile);
        lastDirtyTimeMs_ = now + kAutosaveDelayMs;
        if (result.failed())
            setStatus(resultMessage("Autosave", result));
    }

    void chooseAudioFile()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Open vocal audio", juce::File(), "*.wav;*.aiff;*.aif;*.flac");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (file.existsAsFile())
                                      loadAudio(file);
                              });
    }

    void chooseInstrumentalFile()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Open instrumental audio", juce::File(), "*.wav;*.aiff;*.aif;*.flac");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (file.existsAsFile())
                                      loadInstrumental(file);
                              });
    }

    void chooseJsonFile()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Open annotation JSON", juce::File(), "*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (file.existsAsFile())
                                      loadJson(file);
                              });
    }

    void chooseLyricsFile()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Open lyrics text", juce::File(), "*.txt;*.text;*.md");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (file.existsAsFile())
                                      loadLyrics(file);
                              });
    }

    void loadAudio(const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
        if (reader == nullptr)
        {
            setStatus("Could not read audio file.");
            return;
        }

        const auto previousInstrumentalFile = document_.instrumentalFile;
        auto previousTempoSegments = std::move(document_.tempoSegments);
        auto previousTimeSignatures = std::move(document_.timeSignatures);
        auto previousChords = std::move(document_.chords);

        document_.clear();
        document_.audioFile = file;
        document_.instrumentalFile = previousInstrumentalFile;
        document_.tempoSegments = std::move(previousTempoSegments);
        document_.timeSignatures = std::move(previousTimeSignatures);
        document_.chords = std::move(previousChords);
        document_.sampleRate = static_cast<int>(reader->sampleRate);
        document_.duration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        currentJsonFile_ = file.withFileExtension(".annotation.json");
        ++documentRevision_;
        configureTransportForFile(file);

        editor_.setAudioFile(file);
        if (document_.instrumentalFile.existsAsFile())
            editor_.setInstrumentalFile(document_.instrumentalFile);
        updatePlaybackNotes();
        syncInspector();
        markChanged();
        resetHistory();
        setStatus("Loaded audio. Run AI Parts or Analyze to create annotation.");
    }

    void loadInstrumental(const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
        if (reader == nullptr)
        {
            setStatus("Could not read instrumental file.");
            return;
        }

        beginUndoableAction();
        document_.instrumentalFile = file;
        editor_.setInstrumentalFile(file);
        configureInstrumentalTransportForFile(file);
        markChanged();
        setStatus("Loaded instrumental track: " + file.getFileName());
        runMusicalContextAnalysis();
    }

    void loadLyrics(const juce::File& file)
    {
        if (! document_.audioFile.existsAsFile())
        {
            setStatus("Open audio first, then load lyrics for AI alignment.");
            return;
        }

        if (lyricsAlignmentRunning_)
            return;

        const auto text = file.loadFileAsString();
        if (lyricSyllablesFromText(text).empty())
        {
            setStatus("Lyrics file did not contain words to align.");
            return;
        }

        lyricsAlignmentRunning_ = true;
        setStatus("Running AI lyrics/audio forced alignment...");

        juce::WeakReference<MainComponent> safeThis(this);
        const auto audioFile = document_.audioFile;
        juce::Thread::launch([safeThis, audioFile, file]
        {
            const auto script = juce::File(SYNTHETIC_OBSIDIAN_ROOT)
                                    .getChildFile("tools")
                                    .getChildFile("vocal_annotation_tool")
                                    .getChildFile("align_lyrics_torchaudio.py");
            juce::String output;
            juce::String error;

            juce::ChildProcess process;
            juce::StringArray args;
            args.add(analysisPythonExecutable());
            args.add(script.getFullPathName());
            args.add(audioFile.getFullPathName());
            args.add(file.getFullPathName());

            if (! script.existsAsFile())
            {
                error = "Lyrics alignment script not found: " + script.getFullPathName();
            }
            else if (! process.start(args))
            {
                error = "Could not start python3 lyrics alignment subprocess.";
            }
            else
            {
                while (process.isRunning())
                {
                    output += process.readAllProcessOutput();
                    juce::Thread::sleep(80);
                }

                output += process.readAllProcessOutput();
                const auto exitCode = process.getExitCode();
                if (exitCode != 0)
                    error = "Lyrics alignment failed with exit code " + juce::String(exitCode) + ": " + output;
            }

            juce::MessageManager::callAsync([safeThis, output, error, file]
            {
                if (safeThis != nullptr)
                    safeThis->applyLyricsAlignment(output, error, file);
            });
        });
    }

    void applyLyricsAlignment(const juce::String& output, const juce::String& error, const juce::File& lyricsFile)
    {
        lyricsAlignmentRunning_ = false;

        if (error.isNotEmpty())
        {
            setStatus(error);
            return;
        }

        const auto jsonStart = output.indexOfChar('{');
        const auto jsonEnd = output.lastIndexOfChar('}');
        if (jsonStart < 0 || jsonEnd <= jsonStart)
        {
            setStatus("Lyrics alignment returned no JSON.");
            return;
        }

        auto parsed = juce::JSON::parse(output.substring(jsonStart, jsonEnd + 1));
        auto* root = parsed.getDynamicObject();
        auto* words = root != nullptr ? root->getProperty("words").getArray() : nullptr;
        if (words == nullptr)
        {
            setStatus("Lyrics alignment returned invalid JSON.");
            return;
        }

        std::vector<TimedLyricSyllable> timedSyllables;
        for (const auto& value : *words)
        {
            auto* object = value.getDynamicObject();
            if (object == nullptr)
                continue;

            const auto word = object->getProperty("text").toString();
            const auto start = static_cast<double>(object->getProperty("start"));
            const auto end = static_cast<double>(object->getProperty("end"));
            const auto confidence = juce::jlimit(0.0, 1.0, static_cast<double>(object->getProperty("confidence")));
            if (word.isEmpty() || end <= start)
                continue;

            const auto syllables = lyricSyllablesFromText(word);
            if (syllables.empty())
                continue;

            for (int i = 0; i < static_cast<int>(syllables.size()); ++i)
            {
                const auto proportionStart = static_cast<double>(i) / static_cast<double>(syllables.size());
                const auto proportionEnd = static_cast<double>(i + 1) / static_cast<double>(syllables.size());
                TimedLyricSyllable syllable;
                syllable.text = syllables[static_cast<size_t>(i)].text;
                syllable.start = start + (end - start) * proportionStart;
                syllable.end = start + (end - start) * proportionEnd;
                syllable.kind = BoundaryKind::syllable;
                syllable.confidence = juce::jmax(0.35, confidence);
                timedSyllables.push_back(std::move(syllable));
            }
        }

        if (timedSyllables.empty())
        {
            setStatus("Lyrics alignment did not produce usable word timings.");
            return;
        }

        beginUndoableAction();
        applyTimedLyricSyllables(timedSyllables);
        markChanged();
        editor_.repaint();
        setStatus("AI-aligned " + juce::String(static_cast<int>(timedSyllables.size())) + " lyric syllables from " + lyricsFile.getFileName() + ".");
    }

    std::vector<int> lyricBoundaryIndices() const
    {
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(document_.boundaries.size()); ++i)
        {
            const auto kind = document_.boundaries[static_cast<size_t>(i)].kind;
            if (kind == BoundaryKind::syllable || kind == BoundaryKind::rearticulation || kind == BoundaryKind::legato)
                indices.push_back(i);
        }

        std::sort(indices.begin(), indices.end(), [this](int a, int b)
        {
            return document_.boundaries[static_cast<size_t>(a)].time < document_.boundaries[static_cast<size_t>(b)].time;
        });

        return indices;
    }

    void splitNoteForLyricAt(int noteIndex, double splitTime)
    {
        auto& note = document_.notes[static_cast<size_t>(noteIndex)];
        if (splitTime <= note.start + 0.04 || splitTime >= note.end - 0.04)
            return;

        auto right = note;
        right.id = document_.nextNoteId();
        right.start = splitTime;
        right.voicedStart = juce::jmax(splitTime, right.voicedStart);
        right.flags.addIfNotAlreadyThere("lyric_split");

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
        note.flags.addIfNotAlreadyThere("lyric_split");
        note.curve = std::move(leftCurve);
        right.curve = std::move(rightCurve);

        if (note.curve.empty())
            note.curve.push_back({ note.start, static_cast<double>(note.pitch), 0.45 });
        if (right.curve.empty())
            right.curve.push_back({ right.start, static_cast<double>(right.pitch), 0.45 });

        document_.notes.push_back(std::move(right));
        std::sort(document_.notes.begin(), document_.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    }

    void splitNoteContainingLyricTime(double time)
    {
        for (int i = 0; i < static_cast<int>(document_.notes.size()); ++i)
        {
            const auto& note = document_.notes[static_cast<size_t>(i)];
            if (time > note.start + 0.04 && time < note.end - 0.04)
            {
                splitNoteForLyricAt(i, time);
                return;
            }
        }
    }

    std::optional<int> findNoteForLyricTime(double time) const
    {
        for (int i = 0; i < static_cast<int>(document_.notes.size()); ++i)
        {
            const auto& note = document_.notes[static_cast<size_t>(i)];
            if (time >= note.start && time <= note.end)
                return i;
        }

        return std::nullopt;
    }

    void snapTimedLyricSyllablesToDetectedNotes(std::vector<TimedLyricSyllable>& syllables) const
    {
        for (int i = 0; i < static_cast<int>(syllables.size()); ++i)
        {
            auto& syllable = syllables[static_cast<size_t>(i)];
            const auto startsWord = ! syllable.text.startsWithChar('-');
            if (! startsWord)
                continue;

            const auto noteIndex = findNoteForLyricTime(syllable.start);
            if (! noteIndex)
                continue;

            const auto& note = document_.notes[static_cast<size_t>(*noteIndex)];
            const auto acousticStart = juce::jlimit(note.start, note.end, note.voicedStart);
            const auto onsetLead = syllable.start - acousticStart;
            if (onsetLead < 0.025 || onsetLead > 0.16)
                continue;

            if (i > 0 && acousticStart < syllables[static_cast<size_t>(i - 1)].start + 0.05)
                continue;

            const auto originalLength = juce::jmax(0.05, syllable.end - syllable.start);
            syllable.start = acousticStart;
            syllable.end = juce::jmax(syllable.start + 0.05, syllable.end);

            if (i > 0)
            {
                auto& previous = syllables[static_cast<size_t>(i - 1)];
                previous.end = juce::jmin(previous.end, syllable.start - 0.01);
                if (previous.end <= previous.start)
                    previous.end = previous.start + 0.03;
            }

            if (syllable.end - syllable.start < originalLength * 0.45)
                syllable.end = syllable.start + originalLength * 0.45;
        }
    }

    std::vector<AcousticBoundaryCandidate> detectAcousticBoundaryCandidates()
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(document_.audioFile));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > static_cast<juce::int64>(std::numeric_limits<int>::max()))
            return {};

        const int channels = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
        const int sampleCount = static_cast<int>(reader->lengthInSamples);
        const int windowSamples = juce::jmax(1, static_cast<int>(reader->sampleRate * 0.01));
        juce::AudioBuffer<float> buffer(channels, sampleCount);
        reader->read(&buffer, 0, sampleCount, 0, true, channels > 1);

        std::vector<float> rms;
        rms.reserve(static_cast<size_t>(sampleCount / windowSamples + 1));
        auto maxRms = 0.0f;
        for (int start = 0; start < sampleCount; start += windowSamples)
        {
            const auto count = juce::jmin(windowSamples, sampleCount - start);
            double sum = 0.0;
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto* data = buffer.getReadPointer(channel, start);
                for (int i = 0; i < count; ++i)
                    sum += static_cast<double>(data[i]) * data[i];
            }

            const auto value = static_cast<float>(std::sqrt(sum / static_cast<double>(count * channels)));
            rms.push_back(value);
            maxRms = juce::jmax(maxRms, value);
        }

        if (rms.empty() || maxRms <= 0.00001f)
            return {};

        std::vector<float> envelope(rms.size(), 0.0f);
        for (int i = 0; i < static_cast<int>(rms.size()); ++i)
        {
            double sum = 0.0;
            int count = 0;
            for (int j = juce::jmax(0, i - 2); j <= juce::jmin(static_cast<int>(rms.size()) - 1, i + 2); ++j)
            {
                sum += rms[static_cast<size_t>(j)];
                ++count;
            }

            envelope[static_cast<size_t>(i)] = static_cast<float>(sum / static_cast<double>(count));
        }

        const auto activeThreshold = juce::jmax(0.006f, maxRms * 0.10f);
        const auto searchFrames = juce::jmax(3, static_cast<int>(0.08 * reader->sampleRate / static_cast<double>(windowSamples)));
        const auto minGapFrames = juce::jmax(8, static_cast<int>(0.08 * reader->sampleRate / static_cast<double>(windowSamples)));

        const auto maxEnvelopeInRange = [&envelope](int start, int end)
        {
            auto value = 0.0f;
            for (int i = juce::jmax(0, start); i < juce::jmin(static_cast<int>(envelope.size()), end); ++i)
                value = juce::jmax(value, envelope[static_cast<size_t>(i)]);
            return value;
        };

        std::vector<AcousticBoundaryCandidate> candidates;

        int lastCandidateFrame = -minGapFrames;
        for (int i = searchFrames; i < static_cast<int>(envelope.size()) - searchFrames; ++i)
        {
            if (i - lastCandidateFrame < minGapFrames)
                continue;

            const auto current = envelope[static_cast<size_t>(i)];
            const auto previousPeak = maxEnvelopeInRange(i - searchFrames, i);
            const auto nextPeak = maxEnvelopeInRange(i + 1, i + searchFrames);
            const auto surroundingPeak = juce::jmin(previousPeak, nextPeak);
            const auto localValley = current <= envelope[static_cast<size_t>(i - 1)]
                && current <= envelope[static_cast<size_t>(i + 1)]
                && surroundingPeak >= activeThreshold * 1.35f
                && current <= surroundingPeak * 0.64f;

            const auto before = envelope[static_cast<size_t>(juce::jmax(0, i - 4))];
            const auto after = envelope[static_cast<size_t>(juce::jmin(static_cast<int>(envelope.size()) - 1, i + 4))];
            const auto reattack = after - before > maxRms * 0.075f
                && before <= after * 0.68f
                && after >= activeThreshold * 1.15f;

            if (! localValley && ! reattack)
                continue;

            auto bestFrame = i;
            auto bestValue = current;
            if (localValley)
            {
                for (int candidate = i - 4; candidate <= i + 4; ++candidate)
                {
                    const auto value = envelope[static_cast<size_t>(candidate)];
                    if (value < bestValue)
                    {
                        bestValue = value;
                        bestFrame = candidate;
                    }
                }
            }

            const auto time = static_cast<double>(bestFrame * windowSamples) / reader->sampleRate;
            const auto strength = juce::jlimit(0.0, 1.0, static_cast<double>((surroundingPeak - bestValue) / juce::jmax(0.00001f, surroundingPeak)));
            candidates.push_back({ time, strength });
            lastCandidateFrame = bestFrame;
        }

        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        return candidates;
    }

    static std::optional<AcousticBoundaryCandidate> nearestCandidate(const std::vector<AcousticBoundaryCandidate>& candidates,
                                                                     double windowStart,
                                                                     double windowEnd,
                                                                     double target,
                                                                     double minStrength)
    {
        std::optional<AcousticBoundaryCandidate> best;
        auto bestScore = std::numeric_limits<double>::max();
        for (const auto& candidate : candidates)
        {
            if (candidate.time < windowStart || candidate.time > windowEnd || candidate.strength < minStrength)
                continue;

            const auto score = std::abs(candidate.time - target) / juce::jmax(0.02, windowEnd - windowStart)
                - candidate.strength * 0.20;
            if (score < bestScore)
            {
                bestScore = score;
                best = candidate;
            }
        }

        return best;
    }

    static double acousticCandidateStrengthNear(const std::vector<AcousticBoundaryCandidate>& candidates, double time, double window)
    {
        auto strength = 0.0;
        for (const auto& candidate : candidates)
            if (std::abs(candidate.time - time) <= window)
                strength = juce::jmax(strength, candidate.strength);

        return strength;
    }

    void refineTimedLyricSyllablesWithAudio(std::vector<TimedLyricSyllable>& syllables)
    {
        auto candidates = detectAcousticBoundaryCandidates();
        if (candidates.empty())
            return;

        std::sort(syllables.begin(), syllables.end(), [](const auto& a, const auto& b) { return a.start < b.start; });

        for (int i = 0; i < static_cast<int>(syllables.size()); ++i)
        {
            auto& syllable = syllables[static_cast<size_t>(i)];
            const auto startsWord = ! syllable.text.startsWithChar('-');
            const auto searchBefore = startsWord ? 0.45 : 0.22;
            const auto searchAfter = startsWord ? 0.20 : 0.22;
            const auto minAllowed = i > 0 ? syllables[static_cast<size_t>(i - 1)].start + 0.05 : 0.0;

            if (auto candidate = nearestCandidate(candidates,
                                                  juce::jmax(minAllowed, syllable.start - searchBefore),
                                                  juce::jmin(document_.duration, syllable.start + searchAfter),
                                                  syllable.start,
                                                  startsWord ? 0.25 : 0.20))
            {
                syllable.start = juce::jlimit(minAllowed, document_.duration, candidate->time);
                syllable.confidence = juce::jmax(syllable.confidence, candidate->strength);
            }

            if (! startsWord)
            {
                const auto attackStrength = acousticCandidateStrengthNear(candidates, syllable.start, 0.08);
                syllable.kind = attackStrength >= 0.30 ? BoundaryKind::syllable : BoundaryKind::legato;
            }
        }

        std::sort(syllables.begin(), syllables.end(), [](const auto& a, const auto& b) { return a.start < b.start; });

        for (int i = 0; i < static_cast<int>(syllables.size()); ++i)
        {
            auto& syllable = syllables[static_cast<size_t>(i)];
            const auto nextStart = i + 1 < static_cast<int>(syllables.size())
                ? syllables[static_cast<size_t>(i + 1)].start
                : juce::jmin(document_.duration, syllable.end);
            syllable.end = juce::jlimit(syllable.start + 0.03, document_.duration, juce::jmax(syllable.end, nextStart - 0.01));
        }
    }

    void applyTimedLyricSyllables(std::vector<TimedLyricSyllable> syllables)
    {
        std::sort(syllables.begin(), syllables.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        refineTimedLyricSyllablesWithAudio(syllables);
        snapTimedLyricSyllablesToDetectedNotes(syllables);

        document_.boundaries.erase(std::remove_if(document_.boundaries.begin(),
                                                  document_.boundaries.end(),
                                                  [](const BoundaryMarker& boundary)
                                                  {
                                                      return boundary.kind == BoundaryKind::syllable
                                                          || boundary.kind == BoundaryKind::rearticulation
                                                          || boundary.kind == BoundaryKind::legato;
                                                  }),
                                   document_.boundaries.end());

        for (auto& note : document_.notes)
        {
            note.lyric.clear();
            note.syllableId.clear();
            note.flags.removeString("legato_from_previous");
            note.flags.removeString("legato_to_next");
            note.flags.removeString("melisma_continuation");
        }

        for (const auto& syllable : syllables)
            splitNoteContainingLyricTime(syllable.start);

        for (const auto& syllable : syllables)
        {
            BoundaryMarker boundary;
            boundary.id = document_.nextBoundaryId();
            boundary.time = juce::jlimit(0.0, document_.duration, syllable.start);
            boundary.kind = syllable.kind;
            boundary.text = syllable.text;
            boundary.confidence = syllable.confidence;
            const auto boundaryId = boundary.id;
            document_.boundaries.push_back(std::move(boundary));

            if (auto noteIndex = findNoteForLyricTime((syllable.start + syllable.end) * 0.5))
            {
                document_.notes[static_cast<size_t>(*noteIndex)].lyric = syllable.text;
                document_.notes[static_cast<size_t>(*noteIndex)].syllableId = boundaryId;
            }
        }

        std::sort(document_.boundaries.begin(), document_.boundaries.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        std::sort(document_.notes.begin(), document_.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });

        for (const auto& syllable : syllables)
        {
            if (syllable.kind != BoundaryKind::legato)
                continue;

            for (size_t i = 1; i < document_.notes.size(); ++i)
            {
                auto& previous = document_.notes[i - 1];
                auto& current = document_.notes[i];
                if (std::abs(current.start - syllable.start) <= 0.015
                    && std::abs(previous.end - current.start) <= 0.015)
                {
                    previous.flags.addIfNotAlreadyThere("legato_to_next");
                    current.flags.addIfNotAlreadyThere("legato_from_previous");
                    break;
                }
            }
        }
    }

    bool addLyricRearticulationSplit()
    {
        int bestNoteIndex = -1;
        double bestLength = 0.0;
        for (int i = 0; i < static_cast<int>(document_.notes.size()); ++i)
        {
            const auto& note = document_.notes[static_cast<size_t>(i)];
            const auto length = note.end - note.start;
            if (length > bestLength && length >= 0.18)
            {
                bestLength = length;
                bestNoteIndex = i;
            }
        }

        if (bestNoteIndex < 0)
            return false;

        const auto& note = document_.notes[static_cast<size_t>(bestNoteIndex)];
        const auto splitTime = (note.start + note.end) * 0.5;

        BoundaryMarker boundary;
        boundary.id = document_.nextBoundaryId();
        boundary.time = splitTime;
        boundary.kind = BoundaryKind::rearticulation;
        boundary.text = "rearticulation";
        boundary.confidence = 0.42;
        document_.boundaries.push_back(std::move(boundary));
        splitNoteForLyricAt(bestNoteIndex, splitTime);
        return true;
    }

    void alignLyricsToParts(const std::vector<LyricSyllable>& syllables)
    {
        for (auto& boundary : document_.boundaries)
            if (boundary.kind == BoundaryKind::syllable || boundary.kind == BoundaryKind::rearticulation || boundary.kind == BoundaryKind::legato)
                boundary.text.clear();

        while (lyricBoundaryIndices().size() < syllables.size())
        {
            if (! addLyricRearticulationSplit())
                break;
        }

        auto slots = lyricBoundaryIndices();
        const auto count = juce::jmin(static_cast<int>(slots.size()), static_cast<int>(syllables.size()));
        for (int i = 0; i < count; ++i)
        {
            auto& boundary = document_.boundaries[static_cast<size_t>(slots[static_cast<size_t>(i)])];
            boundary.text = syllables[static_cast<size_t>(i)].text;
            boundary.confidence = juce::jmax(boundary.confidence, 0.62);
        }
    }

    bool hasLyricBoundaryStructure() const
    {
        for (const auto& boundary : document_.boundaries)
        {
            if (boundary.kind != BoundaryKind::syllable && boundary.kind != BoundaryKind::rearticulation && boundary.kind != BoundaryKind::legato)
                continue;

            const auto text = boundary.text.trim();
            if (text.isNotEmpty() && text != toString(boundary.kind))
                return true;
        }

        for (const auto& note : document_.notes)
            if (note.lyric.trim().isNotEmpty())
                return true;

        return false;
    }

    bool hasAiPartsStructure() const
    {
        return std::any_of(document_.boundaries.begin(), document_.boundaries.end(),
                           [](const auto& boundary)
                           {
                               return (boundary.source == "gtsinger_tcn"
                                       && boundary.kind == BoundaryKind::syllable)
                                   || (boundary.source == "gtsinger_tcn_snap"
                                       && (boundary.kind == BoundaryKind::syllable
                                           || boundary.kind == BoundaryKind::rearticulation
                                           || boundary.kind == BoundaryKind::legato));
                           });
    }

    bool hasConformingBoundaryStructure() const
    {
        return hasLyricBoundaryStructure() || hasAiPartsStructure();
    }

    static double overlapDuration(double startA, double endA, double startB, double endB)
    {
        return juce::jmax(0.0, juce::jmin(endA, endB) - juce::jmax(startA, startB));
    }

    static void updateRepresentativePitch(NoteBlock& note)
    {
        struct PitchSample
        {
            double time = 0.0;
            double midi = 60.0;
            double weight = 1.0;
        };

        std::vector<PitchSample> samples;
        samples.reserve(note.curve.size());
        for (const auto& point : note.curve)
        {
            if (point.time < note.start - 0.001 || point.time > note.end + 0.001)
                continue;
            if (! std::isfinite(point.midi) || point.midi < 0.0 || point.midi > 127.0)
                continue;

            samples.push_back({ point.time,
                                point.midi,
                                std::pow(juce::jlimit(0.05, 1.0, point.confidence), 1.4) });
        }

        if (samples.empty())
            return;

        const auto duration = note.end - note.start;
        const auto trim = juce::jmin(0.08, duration * 0.22);
        if (duration >= 0.18 && trim > 0.0)
        {
            std::vector<PitchSample> core;
            core.reserve(samples.size());
            for (const auto& sample : samples)
                if (sample.time >= note.start + trim && sample.time <= note.end - trim)
                    core.push_back(sample);

            if (core.size() >= 2)
                samples = std::move(core);
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

        auto cumulative = 0.0;
        auto representative = samples[samples.size() / 2].midi;
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

    std::vector<NoteBlock> pitchPlateausForInterval(const std::vector<NoteBlock>& notes,
                                                    double start,
                                                    double end) const
    {
        constexpr auto minPlateauDuration = 0.055;
        constexpr auto maxConnectedGap = 0.12;
        constexpr auto samePitchTolerance = 0.68;

        std::vector<NoteBlock> candidates;
        for (const auto& source : notes)
        {
            const auto clippedStart = juce::jmax(start, source.start);
            const auto clippedEnd = juce::jmin(end, source.end);
            if (clippedEnd - clippedStart < minPlateauDuration)
                continue;

            auto candidate = source;
            candidate.start = clippedStart;
            candidate.end = clippedEnd;
            candidate.voicedStart = juce::jlimit(clippedStart, clippedEnd, juce::jmax(clippedStart, source.voicedStart));
            candidate.voicedEnd = juce::jlimit(candidate.voicedStart, clippedEnd, juce::jmin(clippedEnd, source.voicedEnd));
            candidate.curve.clear();
            for (const auto& point : source.curve)
                if (point.time >= clippedStart && point.time <= clippedEnd)
                    candidate.curve.push_back(point);

            if (! candidate.curve.empty())
                updateRepresentativePitch(candidate);
            candidates.push_back(std::move(candidate));
        }

        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        if (candidates.empty())
            return {};

        const auto splitCandidateByCurve = [](const NoteBlock& source)
        {
            std::vector<NoteBlock> result;
            if (source.curve.size() < 6)
                return result;

            std::vector<PitchCurvePoint> usableCurve;
            usableCurve.reserve(source.curve.size());
            for (const auto& point : source.curve)
            {
                if (point.time < source.start || point.time > source.end || point.confidence < 0.08)
                    continue;
                usableCurve.push_back(point);
            }

            if (usableCurve.size() < 6)
                return result;

            std::vector<double> smoothedMidi;
            smoothedMidi.reserve(usableCurve.size());
            for (size_t pointIndex = 0; pointIndex < usableCurve.size(); ++pointIndex)
            {
                std::array<double, 5> neighbourhood {};
                auto count = 0;
                const auto first = static_cast<int>(juce::jmax<size_t>(0, pointIndex > 2 ? pointIndex - 2 : 0));
                const auto last = juce::jmin(static_cast<int>(usableCurve.size()) - 1, static_cast<int>(pointIndex) + 2);
                for (int i = first; i <= last; ++i)
                    neighbourhood[static_cast<size_t>(count++)] = usableCurve[static_cast<size_t>(i)].midi;

                std::sort(neighbourhood.begin(), neighbourhood.begin() + count);
                smoothedMidi.push_back(neighbourhood[static_cast<size_t>(count / 2)]);
            }

            struct CurveRun
            {
                int first = 0;
                int last = 0;
                int pitchBin = 0;
            };

            std::vector<CurveRun> runs;
            constexpr auto curvePitchBinWidth = 0.40;
            constexpr auto minIndependentCurvePlateauDuration = 0.13;
            for (int pointIndex = 0; pointIndex < static_cast<int>(usableCurve.size()); ++pointIndex)
            {
                const auto pitchBin = static_cast<int>(std::round(smoothedMidi[static_cast<size_t>(pointIndex)] / curvePitchBinWidth));
                const auto startsNewRun = runs.empty()
                    || std::abs(pitchBin - runs.back().pitchBin) >= 1
                    || usableCurve[static_cast<size_t>(pointIndex)].time - usableCurve[static_cast<size_t>(runs.back().last)].time > 0.09;
                if (startsNewRun)
                {
                    runs.push_back({ pointIndex, pointIndex, pitchBin });
                    continue;
                }

                runs.back().last = pointIndex;
            }

            for (size_t runIndex = 0; runIndex < runs.size();)
            {
                const auto& run = runs[runIndex];
                const auto runDuration = usableCurve[static_cast<size_t>(run.last)].time - usableCurve[static_cast<size_t>(run.first)].time;
                if (runDuration >= minIndependentCurvePlateauDuration || runs.size() <= 1)
                {
                    ++runIndex;
                    continue;
                }

                if (runIndex > 0 && runIndex + 1 < runs.size())
                {
                    const auto previousDelta = std::abs(runs[runIndex - 1].pitchBin - run.pitchBin);
                    const auto nextDelta = std::abs(runs[runIndex + 1].pitchBin - run.pitchBin);
                    if (runs[runIndex - 1].pitchBin == runs[runIndex + 1].pitchBin)
                    {
                        runs[runIndex - 1].last = runs[runIndex + 1].last;
                        runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(runIndex + 1));
                    }
                    else if (previousDelta <= nextDelta)
                    {
                        runs[runIndex - 1].last = run.last;
                    }
                    else
                    {
                        runs[runIndex + 1].first = run.first;
                    }
                }
                else if (runIndex > 0)
                {
                    runs[runIndex - 1].last = run.last;
                }
                else if (runs.size() > 1)
                {
                    runs[runIndex + 1].first = run.first;
                }

                runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(runIndex));
            }

            for (const auto& run : runs)
            {
                const auto plateauStart = usableCurve[static_cast<size_t>(run.first)].time;
                const auto plateauEnd = usableCurve[static_cast<size_t>(run.last)].time;
                if (plateauEnd - plateauStart < minIndependentCurvePlateauDuration)
                    continue;

                NoteBlock plateau = source;
                plateau.start = plateauStart;
                plateau.end = plateauEnd;
                plateau.voicedStart = plateauStart;
                plateau.voicedEnd = plateauEnd;
                plateau.curve.assign(usableCurve.begin() + run.first, usableCurve.begin() + run.last + 1);
                updateRepresentativePitch(plateau);
                result.push_back(std::move(plateau));
            }

            return result.size() >= 2 ? result : std::vector<NoteBlock> {};
        };

        std::vector<NoteBlock> curveExpandedCandidates;
        curveExpandedCandidates.reserve(candidates.size());
        auto splitAnyCandidate = false;
        for (const auto& candidate : candidates)
        {
            auto split = splitCandidateByCurve(candidate);
            if (split.empty())
            {
                curveExpandedCandidates.push_back(candidate);
                continue;
            }

            splitAnyCandidate = true;
            curveExpandedCandidates.insert(curveExpandedCandidates.end(),
                                           std::make_move_iterator(split.begin()),
                                           std::make_move_iterator(split.end()));
        }

        if (splitAnyCandidate)
            candidates = std::move(curveExpandedCandidates);

        if (candidates.size() == 1 && candidates.front().curve.size() >= 6)
        {
            const auto& source = candidates.front();
            std::vector<PitchCurvePoint> usableCurve;
            usableCurve.reserve(source.curve.size());
            for (const auto& point : source.curve)
            {
                if (point.time < start || point.time > end || point.confidence < 0.08)
                    continue;
                usableCurve.push_back(point);
            }

            if (usableCurve.size() >= 6)
            {
                std::vector<double> smoothedMidi;
                smoothedMidi.reserve(usableCurve.size());
                for (size_t pointIndex = 0; pointIndex < usableCurve.size(); ++pointIndex)
                {
                    std::array<double, 5> neighbourhood {};
                    auto count = 0;
                    const auto first = static_cast<int>(juce::jmax<size_t>(0, pointIndex > 2 ? pointIndex - 2 : 0));
                    const auto last = juce::jmin(static_cast<int>(usableCurve.size()) - 1, static_cast<int>(pointIndex) + 2);
                    for (int i = first; i <= last; ++i)
                        neighbourhood[static_cast<size_t>(count++)] = usableCurve[static_cast<size_t>(i)].midi;

                    std::sort(neighbourhood.begin(), neighbourhood.begin() + count);
                    smoothedMidi.push_back(neighbourhood[static_cast<size_t>(count / 2)]);
                }

                struct CurveRun
                {
                    int first = 0;
                    int last = 0;
                    int pitchBin = 0;
                };

                std::vector<CurveRun> runs;
                constexpr auto curvePitchBinWidth = 0.40;
                for (int pointIndex = 0; pointIndex < static_cast<int>(usableCurve.size()); ++pointIndex)
                {
                    const auto pitchBin = static_cast<int>(std::round(smoothedMidi[static_cast<size_t>(pointIndex)] / curvePitchBinWidth));
                    const auto startsNewRun = runs.empty()
                        || std::abs(pitchBin - runs.back().pitchBin) >= 1
                        || usableCurve[static_cast<size_t>(pointIndex)].time - usableCurve[static_cast<size_t>(runs.back().last)].time > 0.09;
                    if (startsNewRun)
                    {
                        runs.push_back({ pointIndex, pointIndex, pitchBin });
                        continue;
                    }

                    runs.back().last = pointIndex;
                }

                for (size_t runIndex = 0; runIndex < runs.size();)
                {
                    const auto& run = runs[runIndex];
                    const auto runDuration = usableCurve[static_cast<size_t>(run.last)].time - usableCurve[static_cast<size_t>(run.first)].time;
                    if (runDuration >= minPlateauDuration || runs.size() <= 1)
                    {
                        ++runIndex;
                        continue;
                    }

                    if (runIndex > 0 && runIndex + 1 < runs.size())
                    {
                        const auto previousDelta = std::abs(runs[runIndex - 1].pitchBin - run.pitchBin);
                        const auto nextDelta = std::abs(runs[runIndex + 1].pitchBin - run.pitchBin);
                        if (previousDelta <= nextDelta)
                            runs[runIndex - 1].last = run.last;
                        else
                            runs[runIndex + 1].first = run.first;
                    }
                    else if (runIndex > 0)
                    {
                        runs[runIndex - 1].last = run.last;
                    }
                    else if (runs.size() > 1)
                    {
                        runs[runIndex + 1].first = run.first;
                    }

                    runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(runIndex));
                }

                std::vector<NoteBlock> curvePlateaus;
                for (const auto& run : runs)
                {
                    const auto plateauStart = usableCurve[static_cast<size_t>(run.first)].time;
                    const auto plateauEnd = usableCurve[static_cast<size_t>(run.last)].time;
                    if (plateauEnd - plateauStart < minPlateauDuration)
                        continue;

                    NoteBlock plateau = source;
                    plateau.start = plateauStart;
                    plateau.end = plateauEnd;
                    plateau.voicedStart = plateauStart;
                    plateau.voicedEnd = plateauEnd;
                    plateau.curve.assign(usableCurve.begin() + run.first, usableCurve.begin() + run.last + 1);
                    updateRepresentativePitch(plateau);
                    curvePlateaus.push_back(std::move(plateau));
                }

                if (curvePlateaus.size() >= 2)
                    return curvePlateaus;
            }
        }

        std::vector<std::vector<NoteBlock>> chains(1);
        for (auto& candidate : candidates)
        {
            auto& chain = chains.back();
            if (! chain.empty() && candidate.start - chain.back().end > maxConnectedGap)
                chains.emplace_back();
            chains.back().push_back(std::move(candidate));
        }

        const auto bestChain = std::max_element(chains.begin(), chains.end(), [](const auto& a, const auto& b)
        {
            const auto duration = [](const auto& chain)
            {
                auto total = 0.0;
                for (const auto& note : chain)
                    total += note.end - note.start;
                return total;
            };
            return duration(a) < duration(b);
        });

        std::vector<NoteBlock> plateaus;
        for (const auto& candidate : *bestChain)
        {
            if (! plateaus.empty()
                && candidate.start - plateaus.back().end <= maxConnectedGap
                && std::abs(candidate.pitchExact - plateaus.back().pitchExact) < samePitchTolerance)
            {
                auto& previous = plateaus.back();
                previous.end = juce::jmax(previous.end, candidate.end);
                previous.voicedEnd = juce::jmax(previous.voicedEnd, candidate.voicedEnd);
                previous.curve.insert(previous.curve.end(), candidate.curve.begin(), candidate.curve.end());
                std::sort(previous.curve.begin(), previous.curve.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
                updateRepresentativePitch(previous);
                continue;
            }

            plateaus.push_back(candidate);
        }

        return plateaus;
    }

    void rebuildPyinNotesAroundBoundaries(const std::vector<NoteBlock>& pyinNotes)
    {
        std::vector<int> structureIndices;
        structureIndices.reserve(document_.boundaries.size());
        for (int i = 0; i < static_cast<int>(document_.boundaries.size()); ++i)
            structureIndices.push_back(i);

        std::sort(structureIndices.begin(), structureIndices.end(), [this](int a, int b)
        {
            return document_.boundaries[static_cast<size_t>(a)].time < document_.boundaries[static_cast<size_t>(b)].time;
        });

        std::vector<NoteBlock> rebuiltNotes;
        for (int i = 0; i < static_cast<int>(structureIndices.size()); ++i)
        {
            const auto boundaryIndex = structureIndices[static_cast<size_t>(i)];
            const auto& boundary = document_.boundaries[static_cast<size_t>(boundaryIndex)];
            if (boundary.kind != BoundaryKind::syllable
                && boundary.kind != BoundaryKind::rearticulation
                && boundary.kind != BoundaryKind::legato)
                continue;

            const auto start = boundary.time;
            const auto end = i + 1 < static_cast<int>(structureIndices.size())
                ? document_.boundaries[static_cast<size_t>(structureIndices[static_cast<size_t>(i + 1)])].time
                : juce::jmin(document_.duration, start + 0.5);
            if (end <= start + 0.04)
                continue;

            auto plateaus = pitchPlateausForInterval(pyinNotes, start, end);
            if (plateaus.empty())
            {
                NoteBlock fallback;
                fallback.start = start;
                fallback.end = end;
                fallback.voicedStart = start;
                fallback.voicedEnd = end;
                plateaus.push_back(std::move(fallback));
            }

            std::vector<NoteBlock> selectedPlateaus;
            selectedPlateaus.reserve(plateaus.size());
            for (const auto& plateau : plateaus)
            {
                if (selectedPlateaus.empty())
                {
                    selectedPlateaus.push_back(plateau);
                    continue;
                }

                if (selectedPlateaus.size() >= 12)
                    break;
                if (plateau.start - selectedPlateaus.back().start < 0.055)
                    continue;
                if (end - plateau.start < 0.055)
                    continue;
                if (std::abs(plateau.pitchExact - selectedPlateaus.back().pitchExact) < 0.28)
                    continue;

                selectedPlateaus.push_back(plateau);
            }

            for (size_t plateauIndex = 0; plateauIndex < selectedPlateaus.size(); ++plateauIndex)
            {
                const auto segmentStart = plateauIndex == 0 ? start : selectedPlateaus[plateauIndex].start;
                const auto segmentEnd = plateauIndex + 1 < selectedPlateaus.size()
                    ? selectedPlateaus[plateauIndex + 1].start
                    : end;
                if (segmentEnd <= segmentStart + 0.04)
                    continue;

                const auto& plateau = selectedPlateaus[plateauIndex];
                NoteBlock note;
                note.id = juce::String("n") + juce::String(static_cast<int>(rebuiltNotes.size()) + 1).paddedLeft('0', 3);
                note.start = segmentStart;
                note.end = segmentEnd;
                note.pitch = plateau.pitch;
                note.pitchExact = plateau.pitchExact;
                note.voicedStart = juce::jlimit(segmentStart, segmentEnd, juce::jmax(segmentStart, plateau.voicedStart));
                note.voicedEnd = juce::jlimit(note.voicedStart, segmentEnd, juce::jmin(segmentEnd, plateau.voicedEnd));
                note.lyric = plateauIndex == 0 ? boundary.text : juce::String();
                note.syllableId = boundary.id;
                note.flags.add("draft_pyin");
                note.flags.add(boundary.source.startsWith("gtsinger_tcn")
                                   ? "ai_parts_conformed"
                                   : "lyrics_conformed");

                for (const auto& source : pyinNotes)
                    for (const auto& point : source.curve)
                        if (point.time >= segmentStart && point.time <= segmentEnd)
                            note.curve.push_back(point);

                std::sort(note.curve.begin(), note.curve.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
                if (! note.curve.empty())
                {
                    note.voicedStart = juce::jlimit(segmentStart, segmentEnd, note.curve.front().time);
                    note.voicedEnd = juce::jlimit(note.voicedStart, segmentEnd, note.curve.back().time);
                    updateRepresentativePitch(note);
                }
                else
                {
                    note.curve.push_back({ note.start, note.pitchExact, 0.45 });
                    note.curve.push_back({ note.end, note.pitchExact, 0.45 });
                }

                const auto continuesPrevious = plateauIndex > 0
                    || (boundary.kind == BoundaryKind::legato && ! rebuiltNotes.empty());
                if (continuesPrevious && ! rebuiltNotes.empty())
                {
                    note.flags.addIfNotAlreadyThere("legato_from_previous");
                    rebuiltNotes.back().flags.addIfNotAlreadyThere("legato_to_next");
                }
                if (plateauIndex > 0)
                    note.flags.addIfNotAlreadyThere("melisma_continuation");

                rebuiltNotes.push_back(std::move(note));
            }
        }

        if (! rebuiltNotes.empty())
            document_.notes = std::move(rebuiltNotes);
    }

    void runCombinedAnalysis()
    {
        if (! document_.audioFile.existsAsFile())
        {
            setStatus("Open an audio file before running analysis.");
            return;
        }

        if (analysisRunning_ || aiPartsAnalysisRunning_)
            return;

        if (hasAiPartsStructure())
        {
            setStatus("Analyzing pitch using the current AI Parts boundaries...");
            runPyinAnalysis();
            return;
        }

        analyzeAfterAiParts_ = true;
        setStatus("Analyze: detecting AI Parts before pitch analysis...");
        runAiPartsAnalysis();
    }

    void runMusicalContextAnalysis()
    {
        if (! document_.instrumentalFile.existsAsFile())
            return;

        if (musicalContextAnalysisRunning_)
            return;

        musicalContextAnalysisRunning_ = true;
        setStatus("Analyzing instrumental tempo, signature, and chords...");

        juce::WeakReference<MainComponent> safeThis(this);
        const auto instrumentalFile = document_.instrumentalFile;
        juce::Thread::launch([safeThis, instrumentalFile]
        {
            const auto script = juce::File(SYNTHETIC_OBSIDIAN_ROOT)
                                    .getChildFile("tools")
                                    .getChildFile("vocal_annotation_tool")
                                    .getChildFile("analyze_musical_context.py");
            juce::String output;
            juce::String error;

            juce::ChildProcess process;
            juce::StringArray args;
            args.add(analysisPythonExecutable());
            args.add(script.getFullPathName());
            args.add(instrumentalFile.getFullPathName());

            if (! script.existsAsFile())
                error = "Musical context script not found: " + script.getFullPathName();
            else if (! process.start(args))
                error = "Could not start musical context subprocess.";
            else
            {
                while (process.isRunning())
                {
                    output += process.readAllProcessOutput();
                    juce::Thread::sleep(40);
                }

                output += process.readAllProcessOutput();
                const auto exitCode = process.getExitCode();
                if (exitCode != 0)
                    error = "Musical context analysis failed with exit code " + juce::String(exitCode) + ": " + output;
            }

            juce::MessageManager::callAsync([safeThis, output, error, instrumentalFile]
            {
                if (safeThis != nullptr)
                    safeThis->applyMusicalContextAnalysis(output, error, instrumentalFile);
            });
        });
    }

    void applyMusicalContextAnalysis(const juce::String& output,
                                     const juce::String& error,
                                     const juce::File& expectedInstrumentalFile)
    {
        musicalContextAnalysisRunning_ = false;
        if (document_.instrumentalFile != expectedInstrumentalFile)
        {
            setStatus("Discarded stale musical context result because the instrumental track changed.");
            return;
        }

        if (error.isNotEmpty())
        {
            setStatus(error);
            return;
        }

        auto parsed = juce::JSON::parse(output);
        auto* root = parsed.getDynamicObject();
        if (root == nullptr)
        {
            setStatus("Musical context analysis returned invalid JSON.");
            return;
        }

        const auto numberProperty = [](const juce::DynamicObject& object, const juce::Identifier& name, double fallback)
        {
            const auto value = object.getProperty(name);
            return value.isDouble() || value.isInt() ? static_cast<double>(value) : fallback;
        };
        const auto stringProperty = [](const juce::DynamicObject& object, const juce::Identifier& name, const juce::String& fallback = {})
        {
            const auto value = object.getProperty(name);
            return value.isString() ? value.toString() : fallback;
        };

        document_.tempoSegments.clear();
        document_.timeSignatures.clear();
        document_.chords.clear();

        document_.bpm = juce::jlimit(20.0, 300.0, numberProperty(*root, "bpm", document_.bpm));

        if (auto* tempos = root->getProperty("tempo_segments").getArray())
        {
            for (const auto& value : *tempos)
            {
                auto* object = value.getDynamicObject();
                if (object == nullptr)
                    continue;

                document_.tempoSegments.push_back({
                    numberProperty(*object, "start", 0.0),
                    numberProperty(*object, "end", document_.duration),
                    juce::jlimit(20.0, 300.0, numberProperty(*object, "bpm", document_.bpm)),
                    numberProperty(*object, "confidence", 0.0)
                });
            }
        }

        if (auto* signatures = root->getProperty("time_signatures").getArray())
        {
            for (const auto& value : *signatures)
            {
                auto* object = value.getDynamicObject();
                if (object == nullptr)
                    continue;

                document_.timeSignatures.push_back({
                    numberProperty(*object, "start", 0.0),
                    numberProperty(*object, "end", document_.duration),
                    juce::jlimit(1, 12, static_cast<int>(numberProperty(*object, "numerator", 4.0))),
                    juce::jlimit(1, 16, static_cast<int>(numberProperty(*object, "denominator", 4.0))),
                    numberProperty(*object, "confidence", 0.0)
                });
            }
        }

        if (auto* chords = root->getProperty("chords").getArray())
        {
            for (const auto& value : *chords)
            {
                auto* object = value.getDynamicObject();
                if (object == nullptr)
                    continue;

                const auto name = stringProperty(*object, "name", "N");
                if (name.isEmpty())
                    continue;

                document_.chords.push_back({
                    numberProperty(*object, "start", 0.0),
                    numberProperty(*object, "end", document_.duration),
                    name,
                    numberProperty(*object, "confidence", 0.0)
                });
            }
        }

        if (document_.tempoSegments.empty())
            document_.tempoSegments.push_back({ 0.0, document_.duration, document_.bpm, 0.2 });
        if (document_.timeSignatures.empty())
            document_.timeSignatures.push_back({ 0.0, document_.duration, 4, 4, 0.2 });

        markChanged();
        syncInspector();
        editor_.repaint();
        setStatus("Detected " + juce::String(static_cast<int>(document_.tempoSegments.size()))
            + " tempo segment"
            + (document_.tempoSegments.size() == 1 ? ", " : "s, ")
            + juce::String(static_cast<int>(document_.chords.size()))
            + " chord"
            + (document_.chords.size() == 1 ? "." : "s."));
    }

    void runPyinAnalysis()
    {
        if (! document_.audioFile.existsAsFile())
        {
            setStatus("Open an audio file before running analysis.");
            return;
        }

        if (analysisRunning_)
            return;

        analysisRunning_ = true;
        const auto analysisWindows = buildVocalPitchAnalysisWindows(document_);
        setStatus(analysisWindows.empty()
            ? "Running offline pYIN analysis..."
            : "Running offline pYIN analysis in "
                + juce::String(static_cast<int>(analysisWindows.size()))
                + " vocal windows...");

        juce::WeakReference<MainComponent> safeThis(this);
        const auto audioFile = document_.audioFile;
        juce::Thread::launch([safeThis, audioFile, analysisWindows]
        {
            const auto script = juce::File(SYNTHETIC_OBSIDIAN_ROOT).getChildFile("research/svc_pitch/analyze_notes_pyin.py");
            juce::String output;
            juce::String error;

            juce::ChildProcess process;
            juce::StringArray args;
            args.add(analysisPythonExecutable());
            args.add(script.getFullPathName());
            args.add(audioFile.getFullPathName());
            args.add("--sensitivity");
            args.add("0.72");
            args.add("--pitch-backend");
            args.add("pyin");
            for (const auto& window : analysisWindows)
            {
                args.add("--analysis-window");
                args.add(juce::String(window.start, 6) + ":" + juce::String(window.end, 6));
            }

            if (! script.existsAsFile())
            {
                error = "pYIN script not found: " + script.getFullPathName();
            }
            else if (! process.start(args))
            {
                error = "Could not start python3 pYIN analysis subprocess.";
            }
            else
            {
                while (process.isRunning())
                {
                    output += process.readAllProcessOutput();
                    juce::Thread::sleep(40);
                }

                output += process.readAllProcessOutput();
                const auto exitCode = process.getExitCode();
                if (exitCode != 0)
                    error = "pYIN analysis failed with exit code " + juce::String(exitCode) + ".";
            }

            juce::MessageManager::callAsync([safeThis, output, error]
            {
                if (safeThis != nullptr)
                    safeThis->applyPyinAnalysis(output, error);
            });
        });
    }

    void runAiPartsAnalysis()
    {
        if (! document_.audioFile.existsAsFile())
        {
            setStatus("Open an audio file before running AI parts analysis.");
            return;
        }

        if (aiPartsAnalysisRunning_)
            return;

        aiPartsAnalysisRunning_ = true;
        setStatus("Running offline GTSinger TCN parts analysis...");

        juce::WeakReference<MainComponent> safeThis(this);
        const auto audioFile = document_.audioFile;
        const auto revision = documentRevision_;
        juce::Thread::launch([safeThis, audioFile, revision]
        {
            const auto root = juce::File(SYNTHETIC_OBSIDIAN_ROOT);
            const auto script = root.getChildFile("tools")
                                    .getChildFile("vocal_annotation_tool")
                                    .getChildFile("infer_gtsinger_boundaries.py");
            const auto checkpoint = root.getChildFile("data")
                                        .getChildFile("gtsinger_alignment")
                                        .getChildFile("multilingual_mps_full_breath_diffcut_encoder_thr075_tcn.pt");
           #if JUCE_WINDOWS
            const auto venvPython = root.getChildFile("research")
                                        .getChildFile(".venv_seed_vc")
                                        .getChildFile("Scripts")
                                        .getChildFile("python.exe");
            const auto fallbackPython = juce::String("python");
           #else
            const auto venvPython = root.getChildFile("research")
                                        .getChildFile(".venv_seed_vc")
                                        .getChildFile("bin")
                                        .getChildFile("python");
            const auto fallbackPython = juce::String("python3");
           #endif
            juce::String output;
            juce::String error;

            juce::ChildProcess process;
            juce::StringArray args;
            args.add(venvPython.existsAsFile() ? venvPython.getFullPathName() : fallbackPython);
            args.add(script.getFullPathName());
            args.add(audioFile.getFullPathName());
            args.add("--checkpoint");
            args.add(checkpoint.getFullPathName());

            if (! script.existsAsFile())
                error = "AI parts script not found: " + script.getFullPathName();
            else if (! checkpoint.existsAsFile())
                error = "AI parts checkpoint not found: " + checkpoint.getFullPathName();
            else if (! process.start(args))
                error = "Could not start AI parts subprocess.";
            else
            {
                while (process.isRunning())
                {
                    output += process.readAllProcessOutput();
                    juce::Thread::sleep(40);
                }

                output += process.readAllProcessOutput();
                const auto exitCode = process.getExitCode();
                if (exitCode != 0)
                    error = "AI parts analysis failed with exit code " + juce::String(exitCode) + ": " + output;
            }

            juce::MessageManager::callAsync([safeThis, output, error, audioFile, revision]
            {
                if (safeThis != nullptr)
                    safeThis->applyAiPartsAnalysis(output, error, audioFile, revision);
            });
        });
    }

    void applyAiPartsAnalysis(const juce::String& output,
                              const juce::String& error,
                              const juce::File& expectedAudioFile,
                              unsigned int expectedRevision)
    {
        aiPartsAnalysisRunning_ = false;
        if (document_.audioFile != expectedAudioFile || documentRevision_ != expectedRevision)
        {
            analyzeAfterAiParts_ = false;
            setStatus("Discarded stale AI parts result because the document changed.");
            return;
        }

        if (error.isNotEmpty())
        {
            analyzeAfterAiParts_ = false;
            setStatus(error);
            return;
        }

        const auto jsonStart = output.indexOfChar('{');
        const auto jsonEnd = output.lastIndexOfChar('}');
        auto parsed = juce::JSON::parse(
            jsonStart >= 0 && jsonEnd >= jsonStart
                ? output.substring(jsonStart, jsonEnd + 1)
                : output);
        auto* root = parsed.getDynamicObject();
        auto* events = root != nullptr ? root->getProperty("events").getDynamicObject() : nullptr;
        auto* intervals = root != nullptr ? root->getProperty("intervals").getDynamicObject() : nullptr;
        auto* syllables = events != nullptr ? events->getProperty("syllable").getArray() : nullptr;
        if (syllables == nullptr || intervals == nullptr)
        {
            analyzeAfterAiParts_ = false;
            setStatus("AI parts analysis returned invalid JSON.");
            return;
        }

        struct Candidate
        {
            double time = 0.0;
            double confidence = 0.0;
        };

        std::vector<Candidate> syllableCandidates;
        for (const auto& value : *syllables)
        {
            if (auto* object = value.getDynamicObject())
                syllableCandidates.push_back({
                    static_cast<double>(object->getProperty("time")),
                    static_cast<double>(object->getProperty("confidence"))
                });
        }

        beginUndoableAction();
        const auto preserveLyrics = hasLyricBoundaryStructure();
        document_.boundaries.erase(
            std::remove_if(document_.boundaries.begin(), document_.boundaries.end(),
                           [](const auto& boundary)
                           {
                               return boundary.source == "gtsinger_tcn"
                                   || boundary.source == "energy_draft"
                                   || boundary.source == "pyin_draft";
                           }),
            document_.boundaries.end());
        document_.regions.erase(
            std::remove_if(document_.regions.begin(), document_.regions.end(),
                           [](const auto& region)
                           {
                               return region.kind == "breath"
                                   || region.kind == "noise"
                                   || region.kind == "pause";
                           }),
            document_.regions.end());

        if (preserveLyrics)
        {
            std::vector<BoundaryMarker*> lyricBoundaries;
            for (auto& boundary : document_.boundaries)
            {
                if (boundary.kind == BoundaryKind::syllable
                    || boundary.kind == BoundaryKind::rearticulation
                    || boundary.kind == BoundaryKind::legato)
                    lyricBoundaries.push_back(&boundary);
            }
            std::sort(lyricBoundaries.begin(), lyricBoundaries.end(),
                      [](const auto* a, const auto* b) { return a->time < b->time; });

            size_t candidateStart = 0;
            auto previousTime = -1.0;
            for (auto* boundary : lyricBoundaries)
            {
                size_t nearestIndex = syllableCandidates.size();
                auto nearestDistance = 0.13;
                for (size_t index = candidateStart; index < syllableCandidates.size(); ++index)
                {
                    const auto& candidate = syllableCandidates[index];
                    if (candidate.time <= previousTime + 0.03)
                        continue;

                    const auto distance = std::abs(candidate.time - boundary->time);
                    if (distance < nearestDistance)
                    {
                        nearestIndex = index;
                        nearestDistance = distance;
                    }
                }

                if (nearestIndex < syllableCandidates.size())
                {
                    const auto& nearest = syllableCandidates[nearestIndex];
                    boundary->time = juce::jlimit(0.0, document_.duration, nearest.time);
                    boundary->confidence = juce::jmax(boundary->confidence, nearest.confidence);
                    boundary->source = "gtsinger_tcn_snap";
                    previousTime = boundary->time;
                    candidateStart = nearestIndex + 1;
                }
            }
        }
        else
        {
            for (const auto& candidate : syllableCandidates)
            {
                BoundaryMarker boundary;
                boundary.id = document_.nextBoundaryId();
                boundary.time = juce::jlimit(0.0, document_.duration, candidate.time);
                boundary.kind = BoundaryKind::syllable;
                boundary.text = "syllable";
                boundary.confidence = candidate.confidence;
                boundary.source = "gtsinger_tcn";
                document_.boundaries.push_back(std::move(boundary));
            }
        }

        const auto vocalOverlapSeconds = [this](double start, double end)
        {
            auto overlap = 0.0;
            for (const auto& note : document_.notes)
            {
                const auto overlapStart = juce::jmax(start, note.start);
                const auto overlapEnd = juce::jmin(end, note.end);
                if (overlapEnd > overlapStart)
                    overlap += overlapEnd - overlapStart;
            }
            return overlap;
        };

        const auto containsSyllableCandidate = [&syllableCandidates](double start, double end)
        {
            return std::any_of(syllableCandidates.begin(), syllableCandidates.end(),
                               [start, end](const auto& candidate)
                               {
                                   return candidate.time >= start - 0.05
                                       && candidate.time <= end + 0.05;
                               });
        };

        const auto overlapsKnownVocal = [&](double start, double end)
        {
            const auto duration = end - start;
            if (duration <= 0.0)
                return false;

            if (containsSyllableCandidate(start, end))
                return true;

            return vocalOverlapSeconds(start, end) > juce::jmax(0.04, duration * 0.20);
        };

        const auto addIntervals = [this, intervals, &overlapsKnownVocal](const juce::Identifier& property,
                                                                         BoundaryKind kind)
        {
            auto* values = intervals->getProperty(property).getArray();
            if (values == nullptr)
                return 0;

            auto count = 0;
            for (const auto& value : *values)
            {
                auto* object = value.getDynamicObject();
                if (object == nullptr)
                    continue;

                const auto start = juce::jlimit(0.0, document_.duration,
                                                static_cast<double>(object->getProperty("start")));
                const auto end = juce::jlimit(start, document_.duration,
                                              static_cast<double>(object->getProperty("end")));
                const auto confidence = juce::jlimit(
                    0.0,
                    1.0,
                    static_cast<double>(object->getProperty("confidence")));
                if (end - start < 0.08)
                    continue;
                if (kind == BoundaryKind::breath && overlapsKnownVocal(start, end))
                    continue;

                BoundaryMarker boundary;
                boundary.id = document_.nextBoundaryId();
                boundary.time = start;
                boundary.kind = kind;
                boundary.text = toString(kind);
                boundary.confidence = confidence;
                boundary.source = "gtsinger_tcn";
                document_.boundaries.push_back(std::move(boundary));
                document_.regions.push_back({ start, end, toString(kind), {} });
                ++count;
            }
            return count;
        };

        const auto breathCount = addIntervals("breath", BoundaryKind::breath);
        const auto silenceCount = addIntervals("silence", BoundaryKind::pause);
        const auto mergedNonVocalRegions = normalizeNonVocalRegions(document_.regions, document_.boundaries, document_.duration);
        for (const auto& region : document_.regions)
        {
            if (! isNonVocalRegionKind(region.kind))
                continue;

            BoundaryMarker boundary;
            boundary.id = document_.nextBoundaryId();
            boundary.time = region.start;
            boundary.kind = boundaryKindFromString(region.kind);
            boundary.text = region.kind;
            boundary.confidence = 0.85;
            boundary.source = "gtsinger_tcn";
            document_.boundaries.push_back(std::move(boundary));
        }
        const auto removedMicroBoundaries = removeMicroNonVocalBoundaries(document_.boundaries, 0.08, 0.11);

        editor_.fitToClip();
        markChanged();
        setStatus("GTSinger TCN applied "
                  + juce::String(static_cast<int>(syllableCandidates.size()))
                  + " syllable candidates, "
                  + juce::String(breathCount) + " breaths, and "
                  + juce::String(silenceCount) + " pauses"
                  + (mergedNonVocalRegions > 0
                         ? "; merged " + juce::String(mergedNonVocalRegions) + " non-vocal splits"
                         : juce::String())
                  + (removedMicroBoundaries > 0
                         ? (mergedNonVocalRegions > 0 ? ", suppressed " : "; suppressed ")
                            + juce::String(removedMicroBoundaries) + " micro-segments"
                         : juce::String())
                  + (preserveLyrics ? " while preserving lyrics." : "."));

        if (analyzeAfterAiParts_)
        {
            analyzeAfterAiParts_ = false;
            runPyinAnalysis();
        }
    }

    void applyPyinAnalysis(const juce::String& output, const juce::String& error)
    {
        analysisRunning_ = false;

        if (error.isNotEmpty())
        {
            setStatus(error);
            return;
        }

        auto parsed = juce::JSON::parse(output);
        auto* root = parsed.getDynamicObject();
        auto* notes = root != nullptr ? root->getProperty("notes").getArray() : nullptr;
        if (notes == nullptr)
        {
            setStatus("pYIN analysis returned invalid JSON.");
            return;
        }

        std::vector<NoteBlock> pyinNotes;
        const auto conformToBoundaries = hasConformingBoundaryStructure();

        for (const auto& value : *notes)
        {
            auto* object = value.getDynamicObject();
            if (object == nullptr)
                continue;

            NoteBlock note;
            note.id = juce::String("n") + juce::String(static_cast<int>(pyinNotes.size()) + 1).paddedLeft('0', 3);
            note.start = static_cast<double>(object->getProperty("start"));
            note.end = note.start + static_cast<double>(object->getProperty("duration"));
            note.pitch = juce::jlimit(0, 127, static_cast<int>(object->getProperty("pitch")));
            const auto pitchExactValue = object->getProperty("pitch_exact");
            note.pitchExact = juce::jlimit(0.0, 127.0,
                                           pitchExactValue.isDouble() || pitchExactValue.isInt()
                                               ? static_cast<double>(pitchExactValue)
                                               : static_cast<double>(note.pitch));
            note.voicedStart = note.start;
            note.voicedEnd = note.end;
            note.lyric = object->getProperty("lyric").toString();
            note.flags.add("draft_pyin");

            const auto confidence = static_cast<double>(object->getProperty("confidence"));
            const auto curve = object->getProperty("curve").toString();
            for (const auto& token : juce::StringArray::fromTokens(curve, ";", ""))
            {
                const auto pair = juce::StringArray::fromTokens(token, ":", "");
                if (pair.size() >= 2)
                    note.curve.push_back({ note.start + pair[0].getDoubleValue(),
                                           pair[1].getDoubleValue(),
                                           pair.size() >= 3
                                               ? juce::jlimit(0.0, 1.0, pair[2].getDoubleValue())
                                               : juce::jlimit(0.0, 1.0, confidence) });
            }

            updateRepresentativePitch(note);
            pyinNotes.push_back(std::move(note));
        }

        if (pyinNotes.empty())
        {
            setStatus("pYIN analysis returned no note blocks.");
            return;
        }

        beginUndoableAction();
        document_.regions.erase(
            std::remove_if(document_.regions.begin(), document_.regions.end(),
                           [](const auto& region)
                           {
                               return region.kind != "breath"
                                   && region.kind != "noise"
                                   && region.kind != "pause";
                           }),
            document_.regions.end());
        auto collapsedNoteGaps = 0;

        if (conformToBoundaries)
        {
            document_.notes.clear();
            rebuildPyinNotesAroundBoundaries(pyinNotes);
            if (document_.notes.empty())
                document_.notes = std::move(pyinNotes);
            collapsedNoteGaps = collapseShortNoteGaps(document_.notes, document_.boundaries);
        }
        else
        {
            document_.notes = std::move(pyinNotes);
            document_.boundaries.erase(
                std::remove_if(document_.boundaries.begin(), document_.boundaries.end(),
                               [](const auto& boundary)
                               {
                                   return ! isNonVocalBoundary(boundary.kind);
                               }),
                document_.boundaries.end());

            for (const auto& note : document_.notes)
            {
                BoundaryMarker boundary;
                boundary.id = document_.nextBoundaryId();
                boundary.time = note.start;
                boundary.confidence = 0.55;
                boundary.source = "pyin_draft";
                document_.boundaries.push_back(std::move(boundary));
            }
        }

        for (const auto& note : document_.notes)
        {
            document_.regions.push_back({ note.start, juce::jmin(note.start + 0.12, note.end), "slide_in", note.id });
            document_.regions.push_back({ juce::jmax(note.start, note.end - 0.12), note.end, "slide_out", note.id });
        }

        std::sort(document_.boundaries.begin(), document_.boundaries.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        std::sort(document_.notes.begin(), document_.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });

        editor_.fitToClip();
        updatePlaybackNotes();
        markChanged();
        setStatus(conformToBoundaries
            ? "pYIN analysis conformed " + juce::String(static_cast<int>(document_.notes.size()))
                + " note blocks to AI Parts / lyric boundaries"
                + (collapsedNoteGaps > 0
                       ? "; collapsed " + juce::String(collapsedNoteGaps) + " short note gaps."
                       : ".")
            : "pYIN analysis imported " + juce::String(static_cast<int>(document_.notes.size())) + " note blocks.");
    }

    void runNoteRecalculation(const juce::StringArray& noteIds)
    {
        if (! document_.audioFile.existsAsFile())
        {
            setStatus("Open audio before recalculating notes.");
            return;
        }

        if (noteRecalculationRunning_)
            return;

        juce::StringArray regions;
        for (const auto& noteId : noteIds)
        {
            if (auto index = document_.findNoteIndex(noteId))
            {
                const auto& note = document_.notes[static_cast<size_t>(*index)];
                regions.add(note.id + ":" + juce::String(note.start, 6) + ":" + juce::String(note.end, 6) + ":" + juce::String(note.pitch));
            }
        }

        if (regions.isEmpty())
            return;

        noteRecalculationRunning_ = true;
        setStatus("Recalculating " + juce::String(regions.size()) + " note pitch" + (regions.size() == 1 ? "..." : "es..."));

        juce::WeakReference<MainComponent> safeThis(this);
        const auto audioFile = document_.audioFile;
        juce::Thread::launch([safeThis, audioFile, regions]
        {
            const auto script = juce::File(SYNTHETIC_OBSIDIAN_ROOT).getChildFile("research/svc_pitch/analyze_notes_pyin.py");
            juce::String output;
            juce::String error;

            juce::ChildProcess process;
            juce::StringArray args;
            args.add(analysisPythonExecutable());
            args.add(script.getFullPathName());
            args.add(audioFile.getFullPathName());
            for (const auto& region : regions)
            {
                args.add("--region");
                args.add(region);
            }

            if (! script.existsAsFile())
            {
                error = "pYIN script not found: " + script.getFullPathName();
            }
            else if (! process.start(args))
            {
                error = "Could not start python3 note recalculation subprocess.";
            }
            else
            {
                while (process.isRunning())
                {
                    output += process.readAllProcessOutput();
                    juce::Thread::sleep(40);
                }

                output += process.readAllProcessOutput();
                const auto exitCode = process.getExitCode();
                if (exitCode != 0)
                    error = "Note recalculation failed with exit code " + juce::String(exitCode) + ".";
            }

            juce::MessageManager::callAsync([safeThis, output, error]
            {
                if (safeThis != nullptr)
                    safeThis->applyNoteRecalculation(output, error);
            });
        });
    }

    void applyNoteRecalculation(const juce::String& output, const juce::String& error)
    {
        noteRecalculationRunning_ = false;

        if (error.isNotEmpty())
        {
            setStatus(error);
            return;
        }

        auto parsed = juce::JSON::parse(output);
        auto* root = parsed.getDynamicObject();
        auto* notes = root != nullptr ? root->getProperty("notes").getArray() : nullptr;
        if (notes == nullptr)
        {
            setStatus("Note recalculation returned invalid JSON.");
            return;
        }

        auto changed = 0;
        beginUndoableAction();
        for (const auto& value : *notes)
        {
            auto* object = value.getDynamicObject();
            if (object == nullptr)
                continue;

            const auto id = object->getProperty("id").toString();
            if (auto index = document_.findNoteIndex(id))
            {
                auto& note = document_.notes[static_cast<size_t>(*index)];
                note.pitch = juce::jlimit(0, 127, static_cast<int>(object->getProperty("pitch")));
                const auto pitchExactValue = object->getProperty("pitch_exact");
                note.pitchExact = juce::jlimit(0.0, 127.0,
                                               pitchExactValue.isDouble() || pitchExactValue.isInt()
                                                   ? static_cast<double>(pitchExactValue)
                                                   : static_cast<double>(note.pitch));
                note.voicedStart = note.start;
                note.voicedEnd = note.end;
                note.curve.clear();
                const auto confidence = juce::jlimit(0.0, 1.0, static_cast<double>(object->getProperty("confidence")));
                const auto curve = object->getProperty("curve").toString();
                for (const auto& token : juce::StringArray::fromTokens(curve, ";", ""))
                {
                    const auto pair = juce::StringArray::fromTokens(token, ":", "");
                    if (pair.size() >= 2)
                        note.curve.push_back({ note.start + pair[0].getDoubleValue(),
                                               pair[1].getDoubleValue(),
                                               pair.size() >= 3
                                                   ? juce::jlimit(0.0, 1.0, pair[2].getDoubleValue())
                                                   : confidence });
                }

                if (note.curve.empty())
                {
                    note.curve.push_back({ note.start, note.pitchExact, confidence });
                    note.curve.push_back({ note.end, note.pitchExact, confidence });
                }

                updateRepresentativePitch(note);
                note.flags.addIfNotAlreadyThere("recalculated_pyin");
                ++changed;
            }
        }

        if (changed == 0)
        {
            setStatus("Note recalculation returned no matching notes.");
            return;
        }

        updatePlaybackNotes();
        markChanged();
        editor_.repaint();
        setStatus("Recalculated " + juce::String(changed) + " note pitch" + (changed == 1 ? "." : "es."));
    }

    void loadJson(const juce::File& file)
    {
        const auto result = AnnotationJson::load(file, document_);
        if (result.failed())
        {
            setStatus(resultMessage("Load JSON", result));
            return;
        }

        ++documentRevision_;
        currentJsonFile_ = file;
        if (document_.audioFile.existsAsFile())
        {
            editor_.setAudioFile(document_.audioFile);
            configureTransportForFile(document_.audioFile);
        }
        if (document_.instrumentalFile.existsAsFile())
        {
            editor_.setInstrumentalFile(document_.instrumentalFile);
            configureInstrumentalTransportForFile(document_.instrumentalFile);
        }
        else
        {
            clearInstrumentalTrack();
        }
        updatePlaybackNotes();
        syncInspector();
        resetHistory();
        updateValidationStatus();
    }

    void saveJson()
    {
        if (! currentJsonFile_.existsAsFile() && currentJsonFile_ == juce::File())
        {
            if (document_.audioFile.existsAsFile())
                currentJsonFile_ = document_.audioFile.withFileExtension(".annotation.json");
            else
                currentJsonFile_ = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("annotation.json");
        }

        const auto result = AnnotationJson::save(document_, currentJsonFile_);
        dirty_ = result.failed();
        setStatus(resultMessage("Save JSON", result));
    }

    void exportMidi()
    {
        const auto midiFile = currentJsonFile_ != juce::File()
                                ? currentJsonFile_.withFileExtension(".mid")
                                : document_.audioFile.withFileExtension(".mid");
        const auto result = AnnotationJson::exportMidi(document_, midiFile);
        setStatus(resultMessage("Export MIDI", result));
    }

    void createDraftAnnotation(juce::AudioFormatReader& reader)
    {
        if (reader.lengthInSamples <= 0 || reader.lengthInSamples > static_cast<juce::int64>(std::numeric_limits<int>::max()))
            return;

        const int channels = juce::jlimit(1, 2, static_cast<int>(reader.numChannels));
        const int sampleCount = static_cast<int>(reader.lengthInSamples);
        juce::AudioBuffer<float> buffer(channels, sampleCount);
        reader.read(&buffer, 0, sampleCount, 0, true, channels > 1);

        const auto windowSamples = juce::jmax(1, static_cast<int>(reader.sampleRate * 0.01));
        std::vector<float> rms;
        std::vector<float> zeroCrossingRate;
        rms.reserve(static_cast<size_t>(sampleCount / windowSamples + 1));
        zeroCrossingRate.reserve(static_cast<size_t>(sampleCount / windowSamples + 1));

        float maxRms = 0.0f;
        for (int start = 0; start < sampleCount; start += windowSamples)
        {
            const auto count = juce::jmin(windowSamples, sampleCount - start);
            double sum = 0.0;
            int zeroCrossings = 0;
            int transitions = 0;
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto* data = buffer.getReadPointer(channel, start);
                for (int i = 0; i < count; ++i)
                {
                    sum += static_cast<double>(data[i]) * data[i];
                    if (i > 0)
                    {
                        const auto previous = data[i - 1];
                        const auto current = data[i];
                        if ((previous < 0.0f && current >= 0.0f) || (previous >= 0.0f && current < 0.0f))
                            ++zeroCrossings;
                        ++transitions;
                    }
                }
            }

            const auto value = std::sqrt(sum / static_cast<double>(count * channels));
            rms.push_back(static_cast<float>(value));
            zeroCrossingRate.push_back(transitions > 0 ? static_cast<float>(zeroCrossings) / static_cast<float>(transitions) : 0.0f);
            maxRms = juce::jmax(maxRms, static_cast<float>(value));
        }

        if (rms.empty() || maxRms <= 0.00001f)
            return;

        std::vector<float> envelope(rms.size(), 0.0f);
        std::vector<float> zcrEnvelope(zeroCrossingRate.size(), 0.0f);
        for (int i = 0; i < static_cast<int>(rms.size()); ++i)
        {
            double sum = 0.0;
            double zcrSum = 0.0;
            int count = 0;
            for (int j = juce::jmax(0, i - 2); j <= juce::jmin(static_cast<int>(rms.size()) - 1, i + 2); ++j)
            {
                sum += rms[static_cast<size_t>(j)];
                zcrSum += zeroCrossingRate[static_cast<size_t>(j)];
                ++count;
            }
            envelope[static_cast<size_t>(i)] = static_cast<float>(sum / static_cast<double>(count));
            zcrEnvelope[static_cast<size_t>(i)] = static_cast<float>(zcrSum / static_cast<double>(count));
        }

        const auto activeThreshold = juce::jmax(0.006f, maxRms * 0.11f);
        const auto releaseThreshold = juce::jmax(0.004f, maxRms * 0.045f);
        const auto minPartFrames = juce::jmax(3, static_cast<int>(0.10 * reader.sampleRate / static_cast<double>(windowSamples)));
        const auto minGapFrames = juce::jmax(1, static_cast<int>(0.06 * reader.sampleRate / static_cast<double>(windowSamples)));
        const auto microGapFrames = juce::jmax(minGapFrames, static_cast<int>(0.10 * reader.sampleRate / static_cast<double>(windowSamples)));
        const auto searchFrames = juce::jmax(3, static_cast<int>(0.08 * reader.sampleRate / static_cast<double>(windowSamples)));

        std::vector<std::pair<int, int>> activeSpans;
        bool inRegion = false;
        int regionStart = 0;
        int quietFrames = 0;

        for (int i = 0; i <= static_cast<int>(envelope.size()); ++i)
        {
            const auto active = i < static_cast<int>(envelope.size()) && envelope[static_cast<size_t>(i)] >= activeThreshold;
            if (active && ! inRegion)
            {
                inRegion = true;
                regionStart = i;
                quietFrames = 0;
            }
            else if (inRegion)
            {
                const auto released = i >= static_cast<int>(envelope.size()) || envelope[static_cast<size_t>(i)] < releaseThreshold;
                quietFrames = released ? quietFrames + 1 : 0;
                if (quietFrames >= minGapFrames || i >= static_cast<int>(envelope.size()))
                {
                    const auto regionEnd = juce::jmax(regionStart + 1, i - quietFrames + 1);
                    if (regionEnd - regionStart >= minPartFrames)
                        activeSpans.push_back({ regionStart, regionEnd });
                    inRegion = false;
                    quietFrames = 0;
                }
            }
        }

        std::vector<std::pair<int, int>> mergedSpans;
        for (const auto& span : activeSpans)
        {
            if (! mergedSpans.empty() && span.first - mergedSpans.back().second <= microGapFrames)
                mergedSpans.back().second = span.second;
            else
                mergedSpans.push_back(span);
        }

        const auto maxEnvelopeInRange = [&envelope](int start, int end)
        {
            auto value = 0.0f;
            for (int i = juce::jmax(0, start); i < juce::jmin(static_cast<int>(envelope.size()), end); ++i)
                value = juce::jmax(value, envelope[static_cast<size_t>(i)]);
            return value;
        };

        const auto averageZcrInRange = [&zcrEnvelope](int start, int end)
        {
            double sum = 0.0;
            int count = 0;
            for (int i = juce::jmax(0, start); i < juce::jmin(static_cast<int>(zcrEnvelope.size()), end); ++i)
            {
                sum += zcrEnvelope[static_cast<size_t>(i)];
                ++count;
            }

            return count > 0 ? sum / static_cast<double>(count) : 0.0;
        };

        const auto frameToSeconds = [windowSamples, sampleRate = reader.sampleRate](int frame)
        {
            return static_cast<double>(frame * windowSamples) / sampleRate;
        };

        const auto addBoundary = [this](double time, BoundaryKind kind, double confidence)
        {
            BoundaryMarker boundary;
            boundary.id = document_.nextBoundaryId();
            boundary.time = juce::jlimit(0.0, document_.duration, time);
            boundary.kind = kind;
            boundary.text = toString(kind);
            boundary.confidence = juce::jlimit(0.0, 1.0, confidence);
            boundary.source = "energy_draft";
            document_.boundaries.push_back(std::move(boundary));
        };

        struct DraftCut
        {
            int frame = 0;
            BoundaryKind kind = BoundaryKind::syllable;
            double confidence = 0.5;
        };

        const auto addNonVocalInterval = [&](int gapStart, int gapEnd)
        {
            if (gapEnd - gapStart < microGapFrames)
                return;

            const auto gapPeak = maxEnvelopeInRange(gapStart, gapEnd);
            const auto gapDuration = frameToSeconds(gapEnd) - frameToSeconds(gapStart);
            const auto gapZcr = averageZcrInRange(gapStart, gapEnd);
            const auto hasAudibleNoise = gapPeak >= releaseThreshold * 0.85f;
            const auto isBreath = hasAudibleNoise && gapDuration >= 0.12 && gapZcr >= 0.055;
            const auto kind = isBreath
                ? BoundaryKind::breath
                : (! hasAudibleNoise || gapDuration >= 0.18 ? BoundaryKind::pause : BoundaryKind::noise);
            const auto confidence = juce::jlimit(0.25, 0.78,
                                                 static_cast<double>(gapPeak / juce::jmax(0.00001f, maxRms))
                                                     + (kind == BoundaryKind::breath ? 0.12
                                                        : kind == BoundaryKind::pause ? 0.08
                                                                                     : 0.04));
            addBoundary(frameToSeconds(gapStart), kind, confidence);
            document_.regions.push_back({ frameToSeconds(gapStart),
                                          juce::jmin(document_.duration, frameToSeconds(gapEnd)),
                                          toString(kind),
                                          {} });
        };

        if (! mergedSpans.empty())
        {
            addNonVocalInterval(0, mergedSpans.front().first);
            for (int i = 1; i < static_cast<int>(mergedSpans.size()); ++i)
                addNonVocalInterval(mergedSpans[static_cast<size_t>(i - 1)].second,
                                    mergedSpans[static_cast<size_t>(i)].first);
            addNonVocalInterval(mergedSpans.back().second, static_cast<int>(envelope.size()));
        }

        int notePitch = 60;
        for (const auto& span : mergedSpans)
        {
            std::vector<DraftCut> cuts;
            cuts.push_back({ span.first, BoundaryKind::syllable, 0.72 });

            for (int i = span.first + minPartFrames; i <= span.second - minPartFrames; ++i)
            {
                if (i - cuts.back().frame < minPartFrames)
                    continue;

                const auto current = envelope[static_cast<size_t>(i)];
                const auto previous = envelope[static_cast<size_t>(juce::jmax(span.first, i - 1))];
                const auto next = envelope[static_cast<size_t>(juce::jmin(span.second - 1, i + 1))];
                const auto localMin = current <= previous && current <= next;
                const auto previousPeak = maxEnvelopeInRange(juce::jmax(span.first, i - searchFrames), i);
                const auto nextPeak = maxEnvelopeInRange(i + 1, juce::jmin(span.second, i + searchFrames));
                const auto surroundingPeak = juce::jmin(previousPeak, nextPeak);
                const auto valleySplit = localMin
                    && surroundingPeak >= activeThreshold * 1.35f
                    && current <= surroundingPeak * 0.58f;

                const auto before = envelope[static_cast<size_t>(juce::jmax(span.first, i - 3))];
                const auto after = envelope[static_cast<size_t>(juce::jmin(span.second - 1, i + 3))];
                const auto reattackSplit = after - before > maxRms * 0.085f
                    && before <= after * 0.60f
                    && after >= activeThreshold * 1.25f;

                if (! valleySplit && ! reattackSplit)
                    continue;

                auto bestCut = i;
                if (valleySplit)
                {
                    auto bestValue = envelope[static_cast<size_t>(i)];
                    for (int candidate = juce::jmax(cuts.back().frame + minPartFrames, i - 3);
                         candidate <= juce::jmin(span.second - minPartFrames, i + 3);
                         ++candidate)
                    {
                        const auto candidateValue = envelope[static_cast<size_t>(candidate)];
                        if (candidateValue < bestValue)
                        {
                            bestValue = candidateValue;
                            bestCut = candidate;
                        }
                    }
                }

                const auto splitKind = reattackSplit && (! valleySplit || current > surroundingPeak * 0.42f)
                    ? BoundaryKind::rearticulation
                    : BoundaryKind::syllable;
                const auto splitConfidence = juce::jlimit(0.35, 0.86,
                                                          static_cast<double>((surroundingPeak - current) / juce::jmax(0.00001f, surroundingPeak))
                                                              + (splitKind == BoundaryKind::rearticulation ? 0.20 : 0.10));
                cuts.push_back({ bestCut, splitKind, splitConfidence });
                i = bestCut + minPartFrames - 1;
            }

            cuts.push_back({ span.second, BoundaryKind::ignore, 0.0 });

            for (int i = 0; i + 1 < static_cast<int>(cuts.size()); ++i)
            {
                const auto startFrame = cuts[static_cast<size_t>(i)].frame;
                const auto endFrame = cuts[static_cast<size_t>(i + 1)].frame;
                if (endFrame - startFrame < minPartFrames)
                    continue;

                auto voicedStartFrame = startFrame;
                while (voicedStartFrame < endFrame && envelope[static_cast<size_t>(voicedStartFrame)] < activeThreshold)
                    ++voicedStartFrame;

                auto voicedEndFrame = endFrame;
                while (voicedEndFrame > voicedStartFrame && envelope[static_cast<size_t>(voicedEndFrame - 1)] < activeThreshold)
                    --voicedEndFrame;

                const auto start = juce::jlimit(0.0, document_.duration, frameToSeconds(startFrame));
                const auto end = juce::jlimit(start, document_.duration, frameToSeconds(endFrame));
                if (end - start < 0.08)
                    continue;

                const auto voicedStart = juce::jlimit(start, end, frameToSeconds(voicedStartFrame));
                const auto voicedEnd = juce::jlimit(voicedStart, end, frameToSeconds(voicedEndFrame));
                const auto peak = maxEnvelopeInRange(startFrame, endFrame);
                const auto confidence = juce::jlimit(0.30, 0.82, static_cast<double>(peak / juce::jmax(0.00001f, maxRms)));

                NoteBlock note;
                note.id = document_.nextNoteId();
                note.start = start;
                note.end = end;
                note.pitch = notePitch;
                note.pitchExact = static_cast<double>(note.pitch);
                note.voicedStart = voicedStart;
                note.voicedEnd = voicedEnd;
                note.flags.add("draft_auto_part");
                note.curve.push_back({ start, static_cast<double>(note.pitch), 0.62 });
                note.curve.push_back({ (start + end) * 0.5, static_cast<double>(note.pitch), 0.62 });
                note.curve.push_back({ end, static_cast<double>(note.pitch), 0.62 });
                document_.notes.push_back(std::move(note));

                addBoundary(start, cuts[static_cast<size_t>(i)].kind, juce::jmax(confidence, cuts[static_cast<size_t>(i)].confidence));

                notePitch = notePitch == 64 ? 60 : notePitch + 1;
            }
        }

        if (document_.notes.empty() && document_.duration > 0.0)
        {
            NoteBlock note;
            note.id = document_.nextNoteId();
            note.start = 0.0;
            note.end = juce::jmin(0.5, document_.duration);
            note.pitch = 60;
            note.pitchExact = 60.0;
            note.voicedStart = note.start;
            note.voicedEnd = note.end;
            note.flags.add("manual_review");
            note.curve.push_back({ note.start, 60.0, 0.25 });
            note.curve.push_back({ note.end, 60.0, 0.25 });
            document_.notes.push_back(std::move(note));

            BoundaryMarker boundary;
            boundary.id = document_.nextBoundaryId();
            boundary.time = note.start;
            boundary.kind = BoundaryKind::syllable;
            boundary.text = "syllable";
            boundary.confidence = 0.25;
            boundary.source = "energy_draft";
            document_.boundaries.push_back(std::move(boundary));
        }

        for (const auto& note : document_.notes)
        {
            document_.regions.push_back({ note.start, juce::jmin(note.start + 0.12, note.end), "slide_in", note.id });
            document_.regions.push_back({ juce::jmax(note.start, note.end - 0.12), note.end, "slide_out", note.id });
        }
    }

    void annotationChanged() override
    {
        markChanged();
    }

    void selectionChanged(const juce::String& selectedNoteId, const juce::String& selectedBoundaryId) override
    {
        if (auto index = document_.findBoundaryIndex(selectedBoundaryId))
        {
            boundaryKindBox_.setSelectedItemIndex(static_cast<int>(document_.boundaries[static_cast<size_t>(*index)].kind), juce::dontSendNotification);
            selectionTextEditor_.setText(document_.boundaries[static_cast<size_t>(*index)].text, juce::dontSendNotification);
        }

        juce::String info;
        if (auto index = document_.findNoteIndex(selectedNoteId))
        {
            const auto& note = document_.notes[static_cast<size_t>(*index)];
            selectionTextEditor_.setText(note.lyric, juce::dontSendNotification);
            info << "Selected note: " << note.id << "\n"
                 << "Start: " << juce::String(note.start, 3) << "s\n"
                 << "End: " << juce::String(note.end, 3) << "s\n"
                 << "Pitch: " << note.pitch << " / " << juce::String(note.pitchExact, 2) << "\n"
                 << "Lyric: " << note.lyric;
        }
        else if (auto boundaryIndex = document_.findBoundaryIndex(selectedBoundaryId))
        {
            const auto& boundary = document_.boundaries[static_cast<size_t>(*boundaryIndex)];
            info << "Selected boundary: " << boundary.id << "\n"
                 << "Time: " << juce::String(boundary.time, 3) << "s\n"
                 << "Kind: " << toString(boundary.kind);
        }
        else
        {
            selectionTextEditor_.clear();
            info = "No selection.";
        }

        infoLabel_.setText(info, juce::dontSendNotification);
    }

    void noteAuditionRequested(const juce::String& noteId) override
    {
        if (auto index = document_.findNoteIndex(noteId))
            auditionNote(document_.notes[static_cast<size_t>(*index)]);
    }

    void notesRecalculationRequested(const juce::StringArray& noteIds) override
    {
        runNoteRecalculation(noteIds);
    }

    void waveformAuditionRequested(double startTime, double endTime) override
    {
        auditionWaveformLoop(startTime, endTime);
    }

    void applySelectionText()
    {
        const auto text = selectionTextEditor_.getText();
        if (auto index = document_.findNoteIndex(editor_.getSelectedNoteId()))
        {
            auto& note = document_.notes[static_cast<size_t>(*index)];
            if (note.lyric == text)
                return;

            beginUndoableAction();
            note.lyric = text;
            markChanged();
            editor_.repaint();
            return;
        }

        if (auto index = document_.findBoundaryIndex(editor_.getSelectedBoundaryId()))
        {
            auto& boundary = document_.boundaries[static_cast<size_t>(*index)];
            if (boundary.text == text)
                return;

            beginUndoableAction();
            boundary.text = text;
            markChanged();
            editor_.repaint();
        }
    }

    void markChanged()
    {
        dirty_ = true;
        ++documentRevision_;
        lastDirtyTimeMs_ = juce::Time::getMillisecondCounterHiRes();
        updatePlaybackNotes();
        updateInfoText();
        repaint();
    }

    void syncInspector()
    {
        bpmEditor_.setText(juce::String(document_.bpm, 0), false);
        keyEditor_.setText(document_.key, false);
        updateInfoText();
        repaint();
    }

    void updateInfoText()
    {
        juce::String info;
        info << (dirty_ ? "Unsaved changes\n" : "Saved\n")
             << "Audio: " << (document_.audioFile.existsAsFile() ? document_.audioFile.getFileName() : "none") << "\n"
             << "Instrumental: " << (document_.instrumentalFile.existsAsFile() ? document_.instrumentalFile.getFileName() : "none") << "\n"
             << "Duration: " << juce::String(document_.duration, 2) << "s\n"
             << "Notes: " << static_cast<int>(document_.notes.size()) << "\n"
             << "Boundaries: " << static_cast<int>(document_.boundaries.size()) << "\n"
             << "Tempo/Chords: " << static_cast<int>(document_.tempoSegments.size())
             << " / " << static_cast<int>(document_.chords.size());
        if (editor_.getSelectedNoteId().isEmpty() && editor_.getSelectedBoundaryId().isEmpty())
            infoLabel_.setText(info, juce::dontSendNotification);
    }

    void updateValidationStatus()
    {
        const auto issues = AnnotationValidator::validate(document_);
        setStatus(AnnotationValidator::summarize(issues));
    }

    void setStatus(const juce::String& text)
    {
        statusLabel_.setText(text, juce::dontSendNotification);
    }

    juce::AudioFormatManager formatManager_;
    AnnotationDocument document_;
    AnnotationEditorComponent editor_;
    juce::AudioTransportSource transport_;
    juce::AudioTransportSource instrumentalTransport_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    std::unique_ptr<juce::AudioFormatReaderSource> instrumentalReaderSource_;
    juce::AudioBuffer<float> instrumentalMixBuffer_;
    std::array<PlaybackNoteState, 2> noteStates_;
    std::atomic<PlaybackNoteState*> activeNoteState_ { nullptr };
    int writeNoteStateIndex_ = 0;
    std::atomic<int> playbackMode_ { static_cast<int>(PlaybackMode::notesAndSound) };
    std::atomic<double> audioSampleRate_ { 44100.0 };
    double timelineTonePhase_ = 0.0;
    std::atomic<double> clickedToneFrequency_ { 440.0 };
    std::atomic<int> clickedToneSamplesRemaining_ { 0 };
    std::atomic<int> clickedToneTotalSamples_ { 0 };
    std::atomic<unsigned int> clickedToneSerial_ { 0 };
    unsigned int renderedClickedToneSerial_ = 0;
    double clickedTonePhase_ = 0.0;
    int clickedToneElapsedSamples_ = 0;
    bool loopAuditionActive_ = false;
    double loopStartTime_ = 0.0;
    double loopEndTime_ = 0.0;
    juce::File currentJsonFile_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::vector<AnnotationDocument> undoStack_;
    std::vector<AnnotationDocument> redoStack_;
    bool dirty_ = false;
    bool analysisRunning_ = false;
    bool aiPartsAnalysisRunning_ = false;
    bool musicalContextAnalysisRunning_ = false;
    bool analyzeAfterAiParts_ = false;
    bool lyricsAlignmentRunning_ = false;
    bool noteRecalculationRunning_ = false;
    bool restoringHistory_ = false;
    unsigned int documentRevision_ = 0;
    double lastDirtyTimeMs_ = 0.0;

    juce::TextButton openAudioButton_;
    juce::TextButton openInstrumentalButton_;
    juce::TextButton loadJsonButton_;
    juce::TextButton loadLyricsButton_;
    juce::TextButton analyzeButton_;
    juce::TextButton aiPartsButton_;
    juce::TextButton playButton_;
    juce::TextButton saveJsonButton_;
    juce::TextButton undoButton_;
    juce::TextButton redoButton_;
    juce::TextButton exportMidiButton_;
    juce::TextButton validateButton_;
    juce::TextButton fitButton_;
    std::vector<juce::TextButton*> toolbarButtons_;

    juce::Slider horizontalZoomSlider_;
    juce::Slider pitchZoomSlider_;
    juce::ComboBox playbackModeBox_;
    juce::TextEditor bpmEditor_;
    juce::TextEditor keyEditor_;
    juce::TextEditor selectionTextEditor_;
    juce::ComboBox boundaryKindBox_;
    juce::Label infoLabel_;
    juce::Label statusLabel_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(MainComponent)
};

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow()
        : DocumentWindow("Vocal Annotation Tool",
                         juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                         DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
        centreWithSize(getWidth(), getHeight());
        setResizable(true, true);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class VocalAnnotationToolApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Vocal Annotation Tool"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        mainWindow_ = std::make_unique<MainWindow>();
    }

    void shutdown() override
    {
        mainWindow_.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override {}

private:
    std::unique_ptr<MainWindow> mainWindow_;
};

} // namespace vocal_annotation

START_JUCE_APPLICATION(vocal_annotation::VocalAnnotationToolApplication)
