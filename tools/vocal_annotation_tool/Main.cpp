#include "AnnotationEditorComponent.h"
#include "AnnotationJson.h"
#include "AnnotationValidator.h"
#include "AlgorithmicPitchPreview.h"
#include "web/SyntheticObsidianWebView.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <vector>

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
constexpr size_t kMaxBackingTracks = 36;
constexpr int kWaveformPointCount = 65536;
constexpr int kWaveformReadBlockSize = 32768;
constexpr double kMinimumWebNoteLengthSeconds = 0.04;

juce::String supportedAudioFileWildcard()
{
   #if JUCE_MAC || JUCE_IOS
    // CoreAudioFormat, registered by registerBasicFormats(), provides the
    // platform MP3 decoder without enabling JUCE's optional software decoder.
    return "*.wav;*.aiff;*.aif;*.flac;*.mp3";
   #else
    return "*.wav;*.aiff;*.aif;*.flac";
   #endif
}

std::optional<std::array<bool, 12>> chordPitchClasses(
    const juce::String& chordName)
{
    const auto name = chordName.trim();
    if (name.isEmpty())
        return std::nullopt;

    auto root = 0;
    switch (juce::CharacterFunctions::toUpperCase(name[0]))
    {
        case 'C': root = 0; break;
        case 'D': root = 2; break;
        case 'E': root = 4; break;
        case 'F': root = 5; break;
        case 'G': root = 7; break;
        case 'A': root = 9; break;
        case 'B': root = 11; break;
        default: return std::nullopt;
    }

    auto qualityStart = 1;
    if (name.length() > 1)
    {
        const auto accidental = name[1];
        if (accidental == '#' || accidental == 0x266f)
        {
            root = (root + 1) % 12;
            ++qualityStart;
        }
        else if (accidental == 'b' || accidental == 0x266d)
        {
            root = (root + 11) % 12;
            ++qualityStart;
        }
    }

    const auto quality = name.substring(qualityStart).trim().toLowerCase();
    const auto halfDiminished =
        quality.contains("m7b5") || quality.containsChar(0x00f8);
    const auto diminished = halfDiminished
        || quality.contains("dim")
        || quality.containsChar(0x00b0);
    const auto augmented =
        quality.contains("aug") || quality.startsWithChar('+');
    const auto power =
        quality == "5" || quality.startsWith("(5)");
    const auto suspended2 = quality.contains("sus2");
    const auto suspended4 =
        ! suspended2 && quality.contains("sus");
    const auto minor = ! diminished
        && ((quality.startsWithChar('m') && ! quality.startsWith("maj"))
            || quality.startsWith("min")
            || quality.contains("minor"));

    std::array<bool, 12> pitchClasses {};
    const auto addInterval = [&pitchClasses, root](int interval)
    {
        pitchClasses[static_cast<size_t>((root + interval) % 12)] = true;
    };
    if (power)
    {
        addInterval(0);
        addInterval(7);
    }
    else if (diminished)
    {
        addInterval(0);
        addInterval(3);
        addInterval(6);
    }
    else if (augmented)
    {
        addInterval(0);
        addInterval(4);
        addInterval(8);
    }
    else if (suspended2)
    {
        addInterval(0);
        addInterval(2);
        addInterval(7);
    }
    else if (suspended4)
    {
        addInterval(0);
        addInterval(5);
        addInterval(7);
    }
    else
    {
        addInterval(0);
        addInterval(minor ? 3 : 4);
        addInterval(7);
    }

    if (quality.contains("b5"))
    {
        pitchClasses[static_cast<size_t>((root + 7) % 12)] = false;
        addInterval(6);
    }
    else if (quality.contains("#5"))
    {
        pitchClasses[static_cast<size_t>((root + 7) % 12)] = false;
        addInterval(8);
    }

    const auto hasMajorSeventh =
        quality.contains("maj7")
        || quality.contains("major7")
        || quality.containsChar(0x25b3);
    if (hasMajorSeventh)
        addInterval(11);
    else if (halfDiminished
             || quality.containsChar('7')
             || quality.contains("9")
             || quality.contains("11")
             || quality.contains("13"))
        addInterval(10);

    if (quality.contains("6") || quality.contains("13"))
        addInterval(9);
    if (quality.contains("b9"))
        addInterval(1);
    else if (quality.contains("#9"))
        addInterval(3);
    else if (quality.contains("9"))
        addInterval(2);
    if (quality.contains("11"))
        addInterval(5);

    return pitchClasses;
}

double snapPitchToChord(
    double requestedPitch,
    double noteStart,
    const std::vector<ChordSegment>& chords)
{
    const auto chord = std::find_if(
        chords.begin(),
        chords.end(),
        [noteStart](const auto& candidate)
        {
            return noteStart >= candidate.start - 0.000001
                && noteStart < candidate.end - 0.000001;
        });
    if (chord == chords.end())
        return requestedPitch;

    const auto pitchClasses = chordPitchClasses(chord->name);
    if (! pitchClasses)
        return requestedPitch;

    auto bestPitch = requestedPitch;
    auto bestDistance = std::numeric_limits<double>::infinity();
    for (auto candidate = 0; candidate <= 127; ++candidate)
    {
        if (! (*pitchClasses)[static_cast<size_t>(candidate % 12)])
            continue;
        const auto distance =
            std::abs(static_cast<double>(candidate) - requestedPitch);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestPitch = static_cast<double>(candidate);
        }
    }
    return bestPitch;
}

void moveNoteToPitch(NoteBlock& note, double requestedPitch)
{
    const auto pitch = juce::jlimit(0.0, 127.0, requestedPitch);
    const auto pitchDelta = pitch - note.pitchExact;
    note.pitchExact = pitch;
    note.pitch = juce::jlimit(
        0,
        127,
        static_cast<int>(std::lround(pitch)));
    for (auto& point : note.curve)
        point.midi = juce::jlimit(
            0.0,
            127.0,
            point.midi + pitchDelta);
}

struct WaveformPoint
{
    float minimum = 0.0f;
    float maximum = 0.0f;
};

using WaveformData = std::vector<WaveformPoint>;

struct BackingWaveformCache
{
    juce::File file;
    WaveformData waveform;
    juce::String packedWaveform;
    double durationSeconds = 0.0;
    unsigned int requestRevision = 0;
    bool loading = false;
};

enum class WaveformTrack
{
    vocal,
    instrumental,
    backing
};

struct WaveformReadResult
{
    WaveformData waveform;
    juce::String packedWaveform;
    double durationSeconds = 0.0;
};

juce::String packWaveformData(const WaveformData& waveform)
{
    std::vector<std::uint8_t> bytes(waveform.size() * 4);
    for (size_t index = 0; index < waveform.size(); ++index)
    {
        const auto writeValue = [&bytes, index](float value, size_t valueOffset)
        {
            const auto quantized = static_cast<std::int16_t>(
                std::lround(juce::jlimit(-1.0f, 1.0f, value) * 32767.0f));
            const auto bits = static_cast<std::uint16_t>(quantized);
            const auto offset = index * 4 + valueOffset;
            bytes[offset] = static_cast<std::uint8_t>(bits & 0xffu);
            bytes[offset + 1] = static_cast<std::uint8_t>((bits >> 8u) & 0xffu);
        };
        writeValue(waveform[index].minimum, 0);
        writeValue(waveform[index].maximum, 2);
    }

    return bytes.empty()
        ? juce::String()
        : juce::Base64::toBase64(bytes.data(), bytes.size());
}

WaveformReadResult readWaveformData(const juce::File& file)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return {};

    const auto durationSeconds = reader->sampleRate > 0.0
        ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate
        : 0.0;
    const auto pointCount = static_cast<int>(
        juce::jmin<juce::int64>(kWaveformPointCount, reader->lengthInSamples));
    const auto channelsToRead = static_cast<int>(juce::jmin(2u, reader->numChannels));
    juce::AudioBuffer<float> buffer(channelsToRead, kWaveformReadBlockSize);
    WaveformData result(static_cast<size_t>(pointCount));

    for (juce::int64 readPosition = 0;
         readPosition < reader->lengthInSamples;
         readPosition += kWaveformReadBlockSize)
    {
        const auto samplesToRead = static_cast<int>(
            juce::jmin<juce::int64>(
                kWaveformReadBlockSize,
                reader->lengthInSamples - readPosition));
        if (! reader->read(&buffer, 0, samplesToRead, readPosition, true, true))
            break;

        const auto blockEnd = readPosition + samplesToRead;
        auto pointIndex = static_cast<int>(
            readPosition * pointCount / reader->lengthInSamples);
        while (pointIndex < pointCount)
        {
            const auto pointStart = reader->lengthInSamples * pointIndex / pointCount;
            const auto pointEnd = reader->lengthInSamples * (pointIndex + 1) / pointCount;
            const auto segmentStart = juce::jmax(readPosition, pointStart);
            const auto segmentEnd = juce::jmin(blockEnd, pointEnd);
            auto& point = result[static_cast<size_t>(pointIndex)];
            for (int channel = 0; channel < channelsToRead; ++channel)
            {
                const auto range = buffer.findMinMax(
                    channel,
                    static_cast<int>(segmentStart - readPosition),
                    static_cast<int>(segmentEnd - segmentStart));
                point.minimum = juce::jmin(point.minimum, range.getStart());
                point.maximum = juce::jmax(point.maximum, range.getEnd());
            }
            if (pointEnd >= blockEnd)
                break;
            ++pointIndex;
        }
    }

    auto packedWaveform = packWaveformData(result);
    return { std::move(result), std::move(packedWaveform), durationSeconds };
}

enum class PlaybackMode
{
    notesAndSound = 1,
    notesOnly = 2,
    soundOnly = 3
};

struct PlaybackNote
{
    double start = 0.0;
    double end = 0.0;
    double frequency = 440.0;
    float gain = 1.0f;
    bool isBacking = false;
    int backingTrackIndex = -1;
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

struct PitchTrackSnapshot
{
    std::vector<NoteBlock> notes;
    std::vector<AnnotationRegion> regions;
};

struct PitchTrackHistory
{
    std::vector<PitchTrackSnapshot> undo;
    std::vector<PitchTrackSnapshot> redo;
};

struct VoiceTrainingSource
{
    juce::File file;
    double durationSeconds = 0.0;
    double sampleRate = 0.0;
    int channels = 0;
    bool usable = false;
    juce::String status;
    juce::String message;
};

struct VoiceProfileOption
{
    juce::String id;
    juce::String name;
    juce::String quality;
    juce::File directory;
    juce::File adapterFile;
    std::vector<juce::File> sources;
    bool active = false;
};

struct BackingStyleDefinition
{
    const char* id = "";
    const char* name = "";
};

constexpr std::array<BackingStyleDefinition, kMaxBackingTracks> kBackingStyles {{
    { "UNISON", "Unison Double" },
    { "OCT_UP", "Octave Above" },
    { "OCT_DOWN", "Octave Below" },
    { "THIRD_UP", "Third Above" },
    { "THIRD_DOWN", "Third Below" },
    { "SIXTH_UP", "Sixth Above" },
    { "SIXTH_DOWN", "Sixth Below" },
    { "FIFTH_UP", "Fifth Above" },
    { "FIFTH_DOWN", "Fifth Below" },
    { "FOURTH_UP", "Fourth Above" },
    { "FOURTH_DOWN", "Fourth Below" },
    { "DRONE_ROOT", "Drone Root" },
    { "DRONE_FIFTH", "Drone Fifth" },
    { "DRONE_THIRD", "Drone Third" },
    { "PEDAL_ROOT", "Pedal Tone Root" },
    { "PEDAL_FIFTH", "Pedal Tone Fifth" },
    { "CONTRARY", "Contrary Motion Harmony" },
    { "OBLIQUE", "Oblique Motion Harmony" },
    { "PAR3", "Parallel Thirds" },
    { "PAR6", "Parallel Sixths" },
    { "CHOIR_SOP", "Choir Soprano" },
    { "CHOIR_ALT", "Choir Alto" },
    { "CHOIR_TEN", "Choir Tenor" },
    { "CHOIR_BASS", "Choir Bass" },
    { "HARMONY_2P", "Two-Part Harmony" },
    { "HARMONY_3P", "Three-Part Harmony" },
    { "HARMONY_4P", "Four-Part Harmony" },
    { "STYLE_POP", "Pop Harmony" },
    { "STYLE_FOLK", "Folk Harmony" },
    { "STYLE_GOSPEL", "Gospel Harmony" },
    { "STYLE_CLASSICAL", "Classical Choral Harmony" },
    { "STYLE_BSHOP", "Barbershop Harmony" },
    { "SUSP", "Suspension Harmony" },
    { "PASSING", "Passing Tone Harmony" },
    { "TENSION_RES", "Tension-Resolution Harmony" },
    { "DYNAMIC_CP", "Dynamic Counterpoint" },
}};

int backingStyleIndex(const juce::String& styleId, const juce::String& styleName)
{
    for (size_t i = 0; i < kBackingStyles.size(); ++i)
    {
        if ((styleId.isNotEmpty() && styleId == kBackingStyles[i].id)
            || (styleName.isNotEmpty() && styleName == kBackingStyles[i].name))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::optional<BackingStyleDefinition> backingStyleForComboId(int comboId)
{
    const auto index = comboId - 1;
    if (index < 0 || index >= static_cast<int>(kBackingStyles.size()))
        return std::nullopt;

    return kBackingStyles[static_cast<size_t>(index)];
}

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

bool canMergeLeadNotesForBacking(const NoteBlock& previous,
                                 const NoteBlock& current,
                                 double maximumGapSeconds,
                                 double maximumPitchDeltaSemitones)
{
    if (previous.end <= previous.start || current.end <= current.start)
        return false;

    const auto gap = current.start - previous.end;
    if (gap > maximumGapSeconds)
        return false;

    if (previous.syllableId.isNotEmpty() && current.syllableId.isNotEmpty())
        return previous.syllableId == current.syllableId;

    if (std::abs(current.pitchExact - previous.pitchExact) >= maximumPitchDeltaSemitones)
        return false;

    // Some pitch plateaus at the edge of a detected syllable are intentionally
    // left without text/id. Treat an adjacent unlabelled plateau as a continuation,
    // but never merge two explicitly different syllables.
    if (previous.syllableId.isNotEmpty() || current.syllableId.isNotEmpty())
    {
        const auto& unlabelled = previous.syllableId.isEmpty() ? previous : current;
        return unlabelled.lyric.trim().isEmpty() && gap <= 0.03;
    }

    if (previous.lyric.trim().isNotEmpty() || current.lyric.trim().isNotEmpty())
        return previous.lyric.trim() == current.lyric.trim();

    return gap <= 0.03;
}

void mergeLeadNoteForBacking(NoteBlock& target, const NoteBlock& source)
{
    target.end = juce::jmax(target.end, source.end);
    target.voicedStart = juce::jmin(target.voicedStart, source.voicedStart);
    target.voicedEnd = juce::jmax(target.voicedEnd, source.voicedEnd);

    if (target.lyric.trim().isEmpty())
        target.lyric = source.lyric;
    if (target.syllableId.isEmpty())
        target.syllableId = source.syllableId;

    // Keep the articulation at the beginning of the merged note, but inherit
    // the outgoing legato state from its final absorbed plateau.
    target.flags.removeString("legato_to_next");
    if (source.flags.contains("legato_to_next"))
        target.flags.addIfNotAlreadyThere("legato_to_next");
    if (source.flags.contains("melisma_continuation"))
        target.flags.addIfNotAlreadyThere("melisma_continuation");

    target.curve.insert(target.curve.end(), source.curve.begin(), source.curve.end());
}

std::vector<NoteBlock> leadNotesForBackingGeneration(const std::vector<NoteBlock>& sourceNotes,
                                                     double maximumGapSeconds = 0.08,
                                                     double maximumPitchDeltaSemitones = 0.80)
{
    std::vector<NoteBlock> notes;
    std::vector<double> anchorDurations;
    notes.reserve(sourceNotes.size());
    anchorDurations.reserve(sourceNotes.size());

    for (const auto& note : sourceNotes)
    {
        if (note.end <= note.start)
            continue;

        if (! notes.empty()
            && canMergeLeadNotesForBacking(notes.back(), note, maximumGapSeconds, maximumPitchDeltaSemitones))
        {
            const auto sourceDuration = juce::jmax(0.001, note.voicedEnd - note.voicedStart);
            if (sourceDuration > anchorDurations.back())
            {
                notes.back().pitch = note.pitch;
                notes.back().pitchExact = static_cast<double>(note.pitch);
                anchorDurations.back() = sourceDuration;
            }
            mergeLeadNoteForBacking(notes.back(), note);
            continue;
        }

        auto canonical = note;
        canonical.pitchExact = static_cast<double>(canonical.pitch);
        canonical.curve.clear();
        notes.push_back(std::move(canonical));
        anchorDurations.push_back(juce::jmax(0.001, note.voicedEnd - note.voicedStart));
    }

    return notes;
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

juce::String backingModelPythonExecutable()
{
    const auto root = juce::File(SYNTHETIC_OBSIDIAN_ROOT);
   #if JUCE_WINDOWS
    const auto venvPython = root.getChildFile(".venv-llm")
                                .getChildFile("Scripts")
                                .getChildFile("python.exe");
    if (venvPython.existsAsFile())
        return venvPython.getFullPathName();
    return analysisPythonExecutable();
   #else
    const auto venvPython = root.getChildFile(".venv-llm")
                                .getChildFile("bin")
                                .getChildFile("python");
    if (venvPython.existsAsFile())
        return venvPython.getFullPathName();
    return analysisPythonExecutable();
   #endif
}

juce::String soulXPythonExecutable()
{
    const auto runtime = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                             .getChildFile(".synthetic_obsidian")
                             .getChildFile("runtime")
                             .getChildFile("soulx-singer")
                             .getChildFile(".venv");
   #if JUCE_WINDOWS
    const auto venvPython = runtime.getChildFile("Scripts").getChildFile("python.exe");
   #else
    const auto venvPython = runtime.getChildFile("bin").getChildFile("python");
   #endif
    if (venvPython.existsAsFile())
        return venvPython.getFullPathName();
    return analysisPythonExecutable();
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

double interpolatedNoteMidi(const NoteBlock& note, double time)
{
    if (note.curve.empty())
        return note.pitchExact;

    auto curve = note.curve;
    std::sort(curve.begin(), curve.end(), [](const auto& left, const auto& right)
    {
        return left.time < right.time;
    });
    if (time <= curve.front().time)
        return curve.front().midi;
    if (time >= curve.back().time)
        return curve.back().midi;

    for (size_t index = 1; index < curve.size(); ++index)
    {
        const auto& previous = curve[index - 1];
        const auto& next = curve[index];
        if (time <= next.time)
        {
            const auto proportion = juce::jlimit(
                0.0,
                1.0,
                (time - previous.time) / juce::jmax(0.001, next.time - previous.time));
            return previous.midi + (next.midi - previous.midi) * proportion;
        }
    }

    return curve.back().midi;
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
        : editor_(formatManager_, document_),
          webView_([this](const juce::var& command) { handleWebCommand(command); })
    {
        formatManager_.registerBasicFormats();
        loadActiveVoiceProfile();
        editor_.setListener(this);

        addToolbarButton(openAudioButton_, "Open Audio", [this] { chooseAudioFile(); });
        openAudioButton_.setEnabled(false);
        addToolbarButton(openInstrumentalButton_, "Open Inst", [this] { chooseInstrumentalFile(); });
        addToolbarButton(loadJsonButton_, "Open Project", [this] { chooseProject(); });
        addToolbarButton(loadLyricsButton_, "Lyrics", [this] { chooseLyricsFile(); });
        addToolbarButton(analyzeButton_, "Analyze", [this] { runCombinedAnalysis(); });
        addToolbarButton(aiPartsButton_, "AI Parts", [this] { runAiPartsAnalysis(); });
        addToolbarButton(addBackingVocalButton_, "Add BV", [this] { runBackingVocalGeneration(); });
        addToolbarButton(renderBackingAudioButton_, "Render BV", [this] { runBackingAudioRender(); });
        addToolbarButton(playButton_, "Play", [this] { togglePlayback(); });
        addToolbarButton(saveJsonButton_, "Save Project", [this] { saveProject(); });
        addToolbarButton(undoButton_, "Undo", [this] { undo(); });
        addToolbarButton(redoButton_, "Redo", [this] { redo(); });
        addToolbarButton(exportMidiButton_, "Export MIDI", [this] { exportMidi(); });
        addToolbarButton(validateButton_, "Validate", [this] { updateValidationStatus(); });
        addToolbarButton(fitButton_, "Fit", [this] { editor_.fitToClip(); });

        configureSlider(horizontalZoomSlider_, 1.0, 32.0, 1.0, [this] { editor_.setHorizontalZoom(horizontalZoomSlider_.getValue()); });
        configureSlider(pitchZoomSlider_, 1.0, 6.0, 3.0, [this] { editor_.setPitchZoom(pitchZoomSlider_.getValue()); });

        playbackModeBox_.addItem("Notes + Sound", static_cast<int>(PlaybackMode::notesAndSound));
        playbackModeBox_.addItem("Notes Only", static_cast<int>(PlaybackMode::notesOnly));
        playbackModeBox_.addItem("Sound Only", static_cast<int>(PlaybackMode::soundOnly));
        playbackModeBox_.setSelectedId(static_cast<int>(PlaybackMode::notesAndSound), juce::dontSendNotification);
        playbackModeBox_.onChange = [this]
        {
            const auto mode = static_cast<PlaybackMode>(playbackModeBox_.getSelectedId());
            const auto audioMuted = mode == PlaybackMode::notesOnly;
            const auto notesMuted = mode == PlaybackMode::soundOnly;
            leadAudioMuted_.store(audioMuted);
            leadNotesMuted_.store(notesMuted);
            backingAudioMuted_.store(audioMuted);
            backingNotesMuted_.store(notesMuted);
            for (size_t i = 0; i < kMaxBackingTracks; ++i)
            {
                backingTrackAudioMuted_[i].store(audioMuted);
                backingTrackNotesMuted_[i].store(notesMuted);
            }
            sendWebTrackLayerState("Voice Main", audioMuted, notesMuted);
            for (const auto& track : document_.backingTracks)
                sendWebTrackLayerState(track.styleName, audioMuted, notesMuted);
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

        for (int i = 0; i < static_cast<int>(kBackingStyles.size()); ++i)
            backingStyleBox_.addItem(juce::String(kBackingStyles[static_cast<size_t>(i)].id)
                                         + " — "
                                         + kBackingStyles[static_cast<size_t>(i)].name,
                                     i + 1);
        backingStyleBox_.setSelectedId(4, juce::dontSendNotification);
        addAndMakeVisible(backingStyleBox_);

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
        addAndMakeVisible(webView_);
        setWantsKeyboardFocus(true);
        setSize(1280, 720);
        resetHistory();
        updatePlaybackNotes();
        startTimerHz(30);
        setAudioChannels(0, 2);
        startBackingVocalWorker();
    }

    ~MainComponent() override
    {
        if (voiceProfileCancellation_ != nullptr)
            voiceProfileCancellation_->store(true, std::memory_order_release);
        stopTimer();
        stopBackingVocalWorker();
        stopBackingAudioWorker();
        transport_.stop();
        instrumentalTransport_.stop();
        backingAudioTransport_.stop();
        transport_.setSource(nullptr);
        instrumentalTransport_.setSource(nullptr);
        backingAudioTransport_.setSource(nullptr);
        activeNoteState_.store(nullptr);
        readerSource_.reset();
        instrumentalReaderSource_.reset();
        backingAudioReaderSource_.reset();
        if (leadPitchPreviewFile_.existsAsFile())
            leadPitchPreviewFile_.deleteFile();
        for (const auto& [trackName, previewFile] : backingPitchPreviewFiles_)
        {
            juce::ignoreUnused(trackName);
            previewFile.deleteFile();
        }
        shutdownAudio();
    }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        audioSampleRate_.store(sampleRate);
        for (auto& peak : outputPeakLevels_)
            peak.store(0.0f, std::memory_order_relaxed);
        leadAudioMixBuffer_.setSize(2, samplesPerBlockExpected, false, false, true);
        instrumentalMixBuffer_.setSize(2, samplesPerBlockExpected, false, false, true);
        backingAudioMixBuffer_.setSize(2, samplesPerBlockExpected, false, false, true);
        transport_.prepareToPlay(samplesPerBlockExpected, sampleRate);
        instrumentalTransport_.prepareToPlay(samplesPerBlockExpected, sampleRate);
        backingAudioTransport_.prepareToPlay(samplesPerBlockExpected, sampleRate);
    }

    void accumulateOutputPeak(size_t channel, float peak) noexcept
    {
        auto& outputPeak = outputPeakLevels_[channel];
        auto currentPeak = outputPeak.load(std::memory_order_relaxed);
        while (peak > currentPeak
               && ! outputPeak.compare_exchange_weak(
                   currentPeak,
                   peak,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed))
        {
        }
    }

    void captureOutputPeaks(const juce::AudioSourceChannelInfo& bufferToFill) noexcept
    {
        auto* buffer = bufferToFill.buffer;
        if (buffer == nullptr || bufferToFill.numSamples <= 0 || buffer->getNumChannels() <= 0)
            return;

        const auto leftPeak = buffer->getMagnitude(
            0,
            bufferToFill.startSample,
            bufferToFill.numSamples);
        const auto rightPeak = buffer->getNumChannels() > 1
            ? buffer->getMagnitude(
                1,
                bufferToFill.startSample,
                bufferToFill.numSamples)
            : leftPeak;
        accumulateOutputPeak(0, leftPeak);
        accumulateOutputPeak(1, rightPeak);
    }

    void applyLeadClipGains(juce::AudioBuffer<float>& buffer,
                            int numSamples,
                            double playheadStart) noexcept
    {
        const auto* state = activeNoteState_.load(std::memory_order_acquire);
        const auto sampleRate = audioSampleRate_.load(std::memory_order_relaxed);
        if (state == nullptr || state->count <= 0 || sampleRate <= 0.0)
            return;

        constexpr auto transitionSeconds = 0.005;
        const auto blockEnd = playheadStart + static_cast<double>(numSamples) / sampleRate;
        for (int noteIndex = 0; noteIndex < state->count; ++noteIndex)
        {
            const auto& note = state->notes[static_cast<size_t>(noteIndex)];
            if (note.start >= blockEnd)
                break;
            if (note.isBacking
                || note.end <= playheadStart
                || std::abs(note.gain - 1.0f) <= 0.0001f)
            {
                continue;
            }

            const auto firstSample = juce::jlimit(
                0,
                numSamples,
                static_cast<int>(std::floor((note.start - playheadStart) * sampleRate)));
            const auto lastSample = juce::jlimit(
                firstSample,
                numSamples,
                static_cast<int>(std::ceil((note.end - playheadStart) * sampleRate)));
            for (int sample = firstSample; sample < lastSample; ++sample)
            {
                const auto time = playheadStart + static_cast<double>(sample) / sampleRate;
                const auto edgeBlend = juce::jlimit(
                    0.0,
                    1.0,
                    juce::jmin(time - note.start, note.end - time) / transitionSeconds);
                const auto gain =
                    1.0f + (note.gain - 1.0f) * static_cast<float>(edgeBlend);
                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    buffer.setSample(channel, sample, buffer.getSample(channel, sample) * gain);
            }
        }
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        const auto playheadStart = currentTransportPosition();
        bufferToFill.clearActiveBufferRegion();

        if (readerSource_ != nullptr)
        {
            const auto channels = bufferToFill.buffer != nullptr ? bufferToFill.buffer->getNumChannels() : 0;
            if (leadAudioMixBuffer_.getNumChannels() >= channels && leadAudioMixBuffer_.getNumSamples() >= bufferToFill.numSamples)
            {
                leadAudioMixBuffer_.clear();
                juce::AudioSourceChannelInfo leadInfo(&leadAudioMixBuffer_, 0, bufferToFill.numSamples);
                transport_.getNextAudioBlock(leadInfo);
                applyLeadClipGains(leadAudioMixBuffer_, bufferToFill.numSamples, playheadStart);

                const auto anySolo = instrumentalSolo_.load()
                    || leadSolo_.load()
                    || anyBackingTrackSoloed();
                const auto leadAudible = ! leadAudioMuted_.load()
                    && (anySolo ? leadSolo_.load() : ! leadMuted_.load());

                if (leadAudible && bufferToFill.buffer != nullptr)
                {
                    for (int channel = 0; channel < channels; ++channel)
                        bufferToFill.buffer->addFrom(channel,
                                                     bufferToFill.startSample,
                                                     leadAudioMixBuffer_,
                                                     channel,
                                                     0,
                                                     bufferToFill.numSamples);
                }
            }
            else
            {
                transport_.getNextAudioBlock(bufferToFill);
                bufferToFill.clearActiveBufferRegion();
            }
        }

        if (isPlaybackRunning())
            renderTimelineNotes(bufferToFill, playheadStart);

        mixInstrumentalTrack(bufferToFill);
        mixBackingAudioTrack(bufferToFill);

        renderClickedNoteTone(bufferToFill);

        if (bufferToFill.buffer != nullptr)
        {
            bufferToFill.buffer->applyGain(
                bufferToFill.startSample,
                bufferToFill.numSamples,
                masterVolume_.load());
            captureOutputPeaks(bufferToFill);
        }
    }

    void releaseResources() override
    {
        transport_.releaseResources();
        instrumentalTransport_.releaseResources();
        backingAudioTransport_.releaseResources();
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

        const auto anySolo = instrumentalSolo_.load()
            || leadSolo_.load()
            || anyBackingTrackSoloed();
        if (anySolo ? ! instrumentalSolo_.load() : instrumentalMuted_.load())
            return;

        for (int channel = 0; channel < channels; ++channel)
            bufferToFill.buffer->addFrom(channel,
                                         bufferToFill.startSample,
                                         instrumentalMixBuffer_,
                                         channel,
                                         0,
                                         bufferToFill.numSamples,
                                         0.7f);
    }

    void mixBackingAudioTrack(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        if (backingAudioReaderSource_ == nullptr || ! backingAudioTransport_.isPlaying() || bufferToFill.buffer == nullptr)
            return;

        const auto channels = bufferToFill.buffer->getNumChannels();
        if (backingAudioMixBuffer_.getNumChannels() < channels || backingAudioMixBuffer_.getNumSamples() < bufferToFill.numSamples)
            return;

        backingAudioMixBuffer_.clear();
        juce::AudioSourceChannelInfo backingInfo(&backingAudioMixBuffer_, 0, bufferToFill.numSamples);
        backingAudioTransport_.getNextAudioBlock(backingInfo);

        const auto activeTrackIndex = activeBackingTrackIndex_.load(std::memory_order_relaxed);
        const auto hasActiveTrack = activeTrackIndex >= 0
            && activeTrackIndex < static_cast<int>(kMaxBackingTracks);
        const auto audioMuted = hasActiveTrack
            ? backingTrackAudioMuted_[static_cast<size_t>(activeTrackIndex)].load(std::memory_order_relaxed)
            : backingAudioMuted_.load();
        if (audioMuted)
            return;

        const auto activeTrackSoloed = hasActiveTrack
            ? backingTrackSoloed_[static_cast<size_t>(activeTrackIndex)].load(std::memory_order_relaxed)
            : backingSolo_.load();
        const auto activeTrackMuted = hasActiveTrack
            ? backingTrackMuted_[static_cast<size_t>(activeTrackIndex)].load(std::memory_order_relaxed)
            : backingMuted_.load();
        const auto anySolo = instrumentalSolo_.load()
            || leadSolo_.load()
            || anyBackingTrackSoloed();
        if (anySolo ? ! activeTrackSoloed : activeTrackMuted)
            return;

        for (int channel = 0; channel < channels; ++channel)
            bufferToFill.buffer->addFrom(channel,
                                         bufferToFill.startSample,
                                         backingAudioMixBuffer_,
                                         channel,
                                         0,
                                         bufferToFill.numSamples,
                                         0.85f);
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
        const auto anyBackingSolo = anyBackingTrackSoloed();
        const auto anySolo = instrumentalSolo_.load() || leadSolo_.load() || anyBackingSolo;
        const auto leadAudible = ! leadNotesMuted_.load()
            && (anySolo ? leadSolo_.load() : ! leadMuted_.load());
        std::array<bool, kMaxBackingTracks> backingAudible {};
        auto anyBackingAudible = false;
        for (size_t i = 0; i < kMaxBackingTracks; ++i)
        {
            const auto notesMuted = backingTrackNotesMuted_[i].load(std::memory_order_relaxed);
            const auto trackAudible = ! notesMuted
                && (anySolo
                        ? backingTrackSoloed_[i].load(std::memory_order_relaxed)
                        : ! backingTrackMuted_[i].load(std::memory_order_relaxed));
            backingAudible[i] = trackAudible;
            anyBackingAudible = anyBackingAudible || trackAudible;
        }
        if (! leadAudible && ! anyBackingAudible)
            return;

        if (state != renderedNoteState_ || playheadStart + 0.02 < lastTimelineRenderEnd_)
            timelineNotePhases_.fill(0.0);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto time = playheadStart + static_cast<double>(sample) / sampleRate;
            auto mixed = 0.0;
            auto activeCount = 0;
            for (int i = 0; i < state->count; ++i)
            {
                const auto& note = state->notes[static_cast<size_t>(i)];
                if (note.start > time)
                    break;
                if (note.isBacking)
                {
                    if (note.backingTrackIndex < 0
                        || note.backingTrackIndex >= static_cast<int>(kMaxBackingTracks)
                        || ! backingAudible[static_cast<size_t>(note.backingTrackIndex)])
                    {
                        continue;
                    }
                }
                else if (! leadAudible)
                {
                    continue;
                }
                if (time < note.start || time >= note.end)
                    continue;

                const auto fadeSeconds = 0.01;
                const auto attack = note.legatoFromPrevious
                    ? 1.0
                    : juce::jlimit(0.0, 1.0, (time - note.start) / fadeSeconds);
                const auto release = note.legatoToNext
                    ? 1.0
                    : juce::jlimit(0.0, 1.0, (note.end - time) / fadeSeconds);
                const auto frequency = midiNoteToFrequency(playbackMidiAtTime(note, time));
                auto& phase = timelineNotePhases_[static_cast<size_t>(i)];
                mixed += std::sin(phase) * juce::jmin(attack, release) * note.gain;
                phase += kTwoPi * frequency / sampleRate;
                if (phase >= kTwoPi)
                    phase = std::fmod(phase, kTwoPi);
                ++activeCount;
            }

            if (activeCount <= 0)
                continue;

            const auto gain = 0.13f / std::sqrt(static_cast<float>(activeCount));
            const auto value = static_cast<float>(mixed) * gain;

            for (int channel = 0; channel < channels; ++channel)
                buffer->addSample(channel, startSample + sample, value);
        }

        renderedNoteState_ = state;
        lastTimelineRenderEnd_ = playheadStart + static_cast<double>(numSamples) / sampleRate;
    }

    void renderClickedNoteTone(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        auto* buffer = bufferToFill.buffer;
        if (buffer == nullptr)
            return;

        const auto serial = clickedToneSerial_.load(std::memory_order_acquire);
        if (serial != renderedClickedToneSerial_)
        {
            renderedClickedToneSerial_ = serial;
            clickedTonePhase_ = 0.0;
            clickedToneDetunedPhase_ = 0.0;
            clickedToneElapsedSamples_ = 0;
            clickedToneCurrentFrequency_ =
                clickedToneFrequency_.load(std::memory_order_relaxed);
            clickedToneWasHeld_ =
                clickedToneHeld_.load(std::memory_order_relaxed);
        }

        auto remaining =
            clickedToneSamplesRemaining_.load(std::memory_order_relaxed);
        const auto sampleRate =
            audioSampleRate_.load(std::memory_order_relaxed);
        if (sampleRate <= 0.0)
            return;

        const auto holdRequested =
            clickedToneHeld_.load(std::memory_order_acquire);
        const auto leaseAvailable =
            clickedToneLeaseSamples_.load(std::memory_order_acquire);
        const auto leaseBeforeBlock =
            holdRequested && leaseAvailable > 0
            ? clickedToneLeaseSamples_.fetch_sub(
                bufferToFill.numSamples,
                std::memory_order_acq_rel)
            : 0;
        const auto held = holdRequested && leaseBeforeBlock > 0;
        const auto releaseSamples =
            juce::jmax(1, static_cast<int>(sampleRate * 0.055));
        if (clickedToneWasHeld_ && ! held)
            remaining = releaseSamples;
        clickedToneWasHeld_ = held;
        if (! held && remaining <= 0)
            return;

        const auto targetFrequency =
            clickedToneFrequency_.load(std::memory_order_relaxed);
        const auto frequencySmoothing =
            1.0 - std::exp(-1.0 / (sampleRate * 0.006));
        const auto channels = buffer->getNumChannels();

        for (int sample = 0;
             sample < bufferToFill.numSamples && (held || remaining > 0);
             ++sample)
        {
            const auto elapsedSeconds =
                static_cast<double>(clickedToneElapsedSamples_) / sampleRate;
            const auto attack =
                juce::jlimit(0.0, 1.0, elapsedSeconds / 0.004);
            const auto release = held
                ? 1.0
                : juce::jlimit(
                    0.0,
                    1.0,
                    static_cast<double>(remaining)
                        / static_cast<double>(releaseSamples));
            const auto decay = 0.38 + 0.62 * std::exp(-elapsedSeconds * 3.2);
            const auto brightness = std::exp(-elapsedSeconds * 4.8);
            const auto tone =
                0.70 * std::sin(clickedTonePhase_)
                + 0.20 * brightness * std::sin(2.0 * clickedTonePhase_ + 0.07)
                + 0.09 * brightness * std::sin(3.0 * clickedTonePhase_ + 0.13)
                + 0.04 * brightness * std::sin(4.0 * clickedTonePhase_ + 0.21)
                + 0.12 * std::sin(clickedToneDetunedPhase_);
            const auto gain =
                0.21f * static_cast<float>(attack * release * decay);
            const auto value = static_cast<float>(tone) * gain;

            clickedToneCurrentFrequency_ +=
                (targetFrequency - clickedToneCurrentFrequency_)
                * frequencySmoothing;
            clickedTonePhase_ +=
                kTwoPi * clickedToneCurrentFrequency_ / sampleRate;
            clickedToneDetunedPhase_ +=
                kTwoPi * clickedToneCurrentFrequency_ * 1.0025 / sampleRate;
            if (clickedTonePhase_ >= kTwoPi)
                clickedTonePhase_ = std::fmod(clickedTonePhase_, kTwoPi);
            if (clickedToneDetunedPhase_ >= kTwoPi)
                clickedToneDetunedPhase_ =
                    std::fmod(clickedToneDetunedPhase_, kTwoPi);

            for (int channel = 0; channel < channels; ++channel)
                buffer->addSample(channel, bufferToFill.startSample + sample, value);

            if (! held)
                --remaining;
            ++clickedToneElapsedSamples_;
        }

        clickedToneSamplesRemaining_.store(
            held ? 1 : remaining,
            std::memory_order_relaxed);
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
        backingStyleBox_.setBounds(inspector.removeFromTop(28));
        inspector.removeFromTop(8);
        boundaryKindBox_.setBounds(inspector.removeFromTop(28));
        inspector.removeFromTop(8);
        selectionTextEditor_.setBounds(inspector.removeFromTop(58));
        inspector.removeFromTop(12);
        statusLabel_.setBounds(inspector);

        editor_.setBounds(bounds);
        webView_.setBounds(getLocalBounds());
        webView_.toFront(false);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0))
        {
            saveProject();
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

    static juce::var makeTrackState(bool muted, bool soloed)
    {
        auto* state = new juce::DynamicObject();
        state->setProperty("mute", muted);
        state->setProperty("solo", soloed);
        return state;
    }

    static juce::var makeWaveformState(const WaveformData& waveform,
                                       const juce::String& packedWaveform)
    {
        auto* result = new juce::DynamicObject();
        result->setProperty("encoding", "i16le-base64");
        result->setProperty("pointCount", static_cast<int>(waveform.size()));
        result->setProperty("data", packedWaveform);
        return result;
    }

    void requestWaveformData(WaveformTrack track,
                             const juce::File& file,
                             int requestedBackingTrackIndex = -1)
    {
        const auto backingTrackIndex = track == WaveformTrack::backing
            ? (requestedBackingTrackIndex >= 0
                ? requestedBackingTrackIndex
                : activeBackingTrackIndex_.load(std::memory_order_relaxed))
            : -1;
        const auto hasBackingTrackCache = backingTrackIndex >= 0
            && backingTrackIndex < static_cast<int>(kMaxBackingTracks);
        auto* backingCache = hasBackingTrackCache
            ? &backingTrackWaveforms_[static_cast<size_t>(backingTrackIndex)]
            : nullptr;

        auto* sourceFile = backingCache != nullptr
            ? &backingCache->file
            : track == WaveformTrack::vocal
                ? &vocalWaveformFile_
                : track == WaveformTrack::instrumental
                    ? &instrumentalWaveformFile_
                    : &backingWaveformFile_;
        auto* waveform = backingCache != nullptr
            ? &backingCache->waveform
            : track == WaveformTrack::vocal
                ? &vocalWaveform_
                : track == WaveformTrack::instrumental
                    ? &instrumentalWaveform_
                    : &backingWaveform_;
        auto* packedWaveform = backingCache != nullptr
            ? &backingCache->packedWaveform
            : track == WaveformTrack::vocal
                ? &vocalPackedWaveform_
                : track == WaveformTrack::instrumental
                    ? &instrumentalPackedWaveform_
                    : &backingPackedWaveform_;
        auto* loading = backingCache != nullptr
            ? &backingCache->loading
            : track == WaveformTrack::vocal
                ? &vocalWaveformLoading_
                : track == WaveformTrack::instrumental
                    ? &instrumentalWaveformLoading_
                    : &backingWaveformLoading_;
        auto* requestRevision = backingCache != nullptr
            ? &backingCache->requestRevision
            : track == WaveformTrack::vocal
                ? &vocalWaveformRequestRevision_
                : track == WaveformTrack::instrumental
                    ? &instrumentalWaveformRequestRevision_
                    : &backingWaveformRequestRevision_;

        if (! file.existsAsFile())
        {
            ++*requestRevision;
            *sourceFile = juce::File();
            waveform->clear();
            packedWaveform->clear();
            *loading = false;
            if (backingCache != nullptr)
                backingCache->durationSeconds = 0.0;
            if (backingCache != nullptr
                && backingTrackIndex == activeBackingTrackIndex_.load(std::memory_order_relaxed))
            {
                backingWaveformFile_ = juce::File();
                backingWaveform_.clear();
                backingPackedWaveform_.clear();
                backingWaveformLoading_ = false;
            }
            return;
        }

        if (*sourceFile == file && (*loading || ! waveform->empty()))
        {
            if (backingCache != nullptr
                && backingTrackIndex == activeBackingTrackIndex_.load(std::memory_order_relaxed))
            {
                backingWaveformFile_ = backingCache->file;
                backingWaveform_ = backingCache->waveform;
                backingPackedWaveform_ = backingCache->packedWaveform;
                backingWaveformLoading_ = backingCache->loading;
            }
            return;
        }

        *sourceFile = file;
        waveform->clear();
        packedWaveform->clear();
        *loading = true;
        const auto expectedRevision = ++*requestRevision;
        juce::WeakReference<MainComponent> safeThis(this);
        juce::Thread::launch([safeThis, file, track, expectedRevision, backingTrackIndex]
        {
            auto loadedWaveform = readWaveformData(file);
            juce::MessageManager::callAsync(
                [safeThis,
                 file,
                 track,
                 expectedRevision,
                 backingTrackIndex,
                 waveformResult = std::move(loadedWaveform.waveform),
                 packedWaveformResult = std::move(loadedWaveform.packedWaveform),
                 durationSeconds = loadedWaveform.durationSeconds]() mutable
                {
                    if (safeThis == nullptr)
                        return;

                    if (track == WaveformTrack::backing
                        && backingTrackIndex >= 0
                        && backingTrackIndex < static_cast<int>(kMaxBackingTracks))
                    {
                        auto& cache = safeThis->backingTrackWaveforms_[
                            static_cast<size_t>(backingTrackIndex)];
                        if (cache.file != file || cache.requestRevision != expectedRevision)
                            return;

                        cache.waveform = std::move(waveformResult);
                        cache.packedWaveform = std::move(packedWaveformResult);
                        cache.durationSeconds = durationSeconds;
                        cache.loading = false;
                        if (backingTrackIndex
                            == safeThis->activeBackingTrackIndex_.load(std::memory_order_relaxed))
                        {
                            safeThis->backingWaveformFile_ = cache.file;
                            safeThis->backingWaveform_ = cache.waveform;
                            safeThis->backingPackedWaveform_ = cache.packedWaveform;
                            safeThis->backingWaveformLoading_ = false;
                        }
                        safeThis->sendWebProjectState();
                        return;
                    }

                    auto& currentFile = track == WaveformTrack::vocal
                        ? safeThis->document_.audioFile
                        : track == WaveformTrack::instrumental
                            ? safeThis->document_.instrumentalFile
                            : safeThis->document_.backingAudioFile;
                    auto& currentRevision = track == WaveformTrack::vocal
                        ? safeThis->vocalWaveformRequestRevision_
                        : track == WaveformTrack::instrumental
                            ? safeThis->instrumentalWaveformRequestRevision_
                            : safeThis->backingWaveformRequestRevision_;
                    if (currentFile != file || currentRevision != expectedRevision)
                        return;

                    auto& currentWaveform = track == WaveformTrack::vocal
                        ? safeThis->vocalWaveform_
                        : track == WaveformTrack::instrumental
                            ? safeThis->instrumentalWaveform_
                            : safeThis->backingWaveform_;
                    auto& currentLoading = track == WaveformTrack::vocal
                        ? safeThis->vocalWaveformLoading_
                        : track == WaveformTrack::instrumental
                            ? safeThis->instrumentalWaveformLoading_
                            : safeThis->backingWaveformLoading_;
                    auto& currentPackedWaveform = track == WaveformTrack::vocal
                        ? safeThis->vocalPackedWaveform_
                        : track == WaveformTrack::instrumental
                            ? safeThis->instrumentalPackedWaveform_
                            : safeThis->backingPackedWaveform_;
                    currentWaveform = std::move(waveformResult);
                    currentPackedWaveform = std::move(packedWaveformResult);
                    currentLoading = false;
                    safeThis->sendWebProjectState();
                });
        });
    }

    void refreshWaveformData()
    {
        requestWaveformData(WaveformTrack::vocal, document_.audioFile);
        requestWaveformData(WaveformTrack::instrumental, document_.instrumentalFile);
        for (const auto& track : document_.backingTracks)
        {
            const auto trackIndex = backingStyleIndex(track.styleId, track.styleName);
            if (trackIndex >= 0)
                requestWaveformData(WaveformTrack::backing, track.audioFile, trackIndex);
        }
        if (document_.backingTracks.empty())
            requestWaveformData(WaveformTrack::backing, document_.backingAudioFile);
    }

    bool isPlaybackRunning() const
    {
        return transport_.isPlaying()
            || instrumentalTransport_.isPlaying()
            || backingAudioTransport_.isPlaying();
    }

    double currentTransportPosition() const
    {
        if (readerSource_ != nullptr)
            return transport_.getCurrentPosition();
        if (instrumentalReaderSource_ != nullptr)
            return instrumentalTransport_.getCurrentPosition();
        return 0.0;
    }

    double timelineDuration() const
    {
        return juce::jmax(1.0, document_.duration);
    }

    double normalisedTimelinePosition(double seconds) const
    {
        return juce::jlimit(0.0, 100.0, seconds * 100.0 / timelineDuration());
    }

    void setTransportPosition(double seconds)
    {
        const auto clamped = juce::jlimit(0.0, juce::jmax(0.0, document_.duration), seconds);
        transport_.setPosition(clamped);
        instrumentalTransport_.setPosition(clamped);
        backingAudioTransport_.setPosition(clamped);
        editor_.setPlayheadTime(clamped);
    }

    void stopPlayback()
    {
        transport_.stop();
        instrumentalTransport_.stop();
        backingAudioTransport_.stop();
        loopAuditionActive_ = false;
        playButton_.setButtonText("Play");
    }

    void sendWebStatus()
    {
        if (! webFrontendReady_)
            return;

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "status-state");
        event->setProperty("message", statusLabel_.getText());
        event->setProperty(
            "vocalAnalysisRunning",
            analysisRunning_ || aiPartsAnalysisRunning_ || analyzeAfterAiParts_);
        event->setProperty(
            "instrumentalAnalysisRunning",
            musicalContextAnalysisRunning_);
        event->setProperty("backingGenerationRunning", backingGenerationRunning_);
        event->setProperty("backingAudioRenderRunning", backingAudioRenderRunning_);
        event->setProperty("backingGenerationTrack", backingGenerationTrackName_);
        event->setProperty("backingAudioRenderTrack", backingAudioRenderTrackName_);
        webView_.dispatch(event);
    }

    double voiceTrainingUsableDuration() const
    {
        double duration = 0.0;
        for (const auto& source : voiceTrainingSources_)
            if (source.usable)
                duration += source.durationSeconds;
        return duration;
    }

    static juce::File voiceProfilesRoot()
    {
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile(".synthetic_obsidian")
            .getChildFile("models")
            .getChildFile("voice_profiles");
    }

    static juce::File voiceTrainingJobsRoot()
    {
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile(".synthetic_obsidian")
            .getChildFile("training")
            .getChildFile("jobs");
    }

    std::vector<VoiceProfileOption> readVoiceProfileOptions() const
    {
        const auto profilesRoot = voiceProfilesRoot();
        juce::Array<juce::File> directories;
        profilesRoot.findChildFiles(
            directories,
            juce::File::findDirectories,
            false);

        std::vector<VoiceProfileOption> profiles;
        profiles.reserve(static_cast<size_t>(directories.size()));
        for (const auto& directory : directories)
        {
            const auto manifestValue =
                juce::JSON::parse(directory.getChildFile("manifest.json"));
            const auto* manifest = manifestValue.getDynamicObject();
            if (manifest == nullptr)
                continue;

            const auto adapterRelativePath =
                manifest->getProperty("adapterFile").toString();
            if (adapterRelativePath.isEmpty())
                continue;

            const auto directoryPrefix =
                directory.getFullPathName() + juce::File::getSeparatorString();
            const auto adapterFile = directory.getChildFile(adapterRelativePath);
            if (! adapterFile.existsAsFile()
                || ! adapterFile.getFullPathName().startsWith(directoryPrefix))
            {
                continue;
            }

            VoiceProfileOption profile;
            profile.id = directory.getFileName();
            profile.name = manifest->getProperty("presetName").toString();
            if (profile.name.isEmpty())
                profile.name = profile.id;
            profile.quality =
                manifest->getProperty("quality").toString() == "high"
                    ? "high"
                    : "balanced";
            profile.directory = directory;
            profile.adapterFile = adapterFile;
            profile.active = profile.id == activeVoiceProfileId_;

            if (const auto* sourceValues =
                    manifest->getProperty("sources").getArray())
            {
                for (const auto& sourceValue : *sourceValues)
                {
                    const auto* source = sourceValue.getDynamicObject();
                    if (source == nullptr)
                        continue;
                    const auto sourceFile = directory.getChildFile(
                        source->getProperty("file").toString());
                    if (sourceFile.existsAsFile()
                        && sourceFile.getFullPathName().startsWith(directoryPrefix))
                    {
                        profile.sources.push_back(sourceFile);
                    }
                }
            }
            profiles.push_back(std::move(profile));
        }

        std::sort(
            profiles.begin(),
            profiles.end(),
            [](const auto& left, const auto& right)
            {
                if (left.active != right.active)
                    return left.active;
                const auto nameComparison =
                    left.name.compareNatural(right.name);
                return nameComparison != 0
                    ? nameComparison < 0
                    : left.id < right.id;
            });
        return profiles;
    }

    void loadActiveVoiceProfile()
    {
        const auto profilesRoot = voiceProfilesRoot();
        const auto activeFile = profilesRoot.getChildFile("active.json");
        if (! activeFile.existsAsFile())
            return;

        const auto activeValue = juce::JSON::parse(activeFile);
        const auto* activeObject = activeValue.getDynamicObject();
        if (activeObject == nullptr)
            return;

        const auto directoryName =
            juce::File(activeObject->getProperty("directory").toString()).getFileName();
        const auto profileDirectory = profilesRoot.getChildFile(directoryName);
        const auto manifestFile = profileDirectory.getChildFile("manifest.json");
        if (! profileDirectory.isDirectory() || ! manifestFile.existsAsFile())
            return;

        const auto manifestValue = juce::JSON::parse(manifestFile);
        const auto* manifest = manifestValue.getDynamicObject();
        if (manifest == nullptr)
            return;

        std::vector<juce::File> sources;
        std::vector<VoiceTrainingSource> trainingSources;
        if (const auto* sourceValues = manifest->getProperty("sources").getArray())
        {
            for (const auto& sourceValue : *sourceValues)
            {
                const auto* source = sourceValue.getDynamicObject();
                if (source == nullptr)
                    continue;
                const auto relativePath = source->getProperty("file").toString();
                const auto file = profileDirectory.getChildFile(relativePath);
                if (file.existsAsFile()
                    && file.getFullPathName().startsWith(
                        profileDirectory.getFullPathName()
                            + juce::File::getSeparatorString()))
                {
                    sources.push_back(file);
                    trainingSources.push_back(inspectVoiceTrainingSource(file));
                }
            }
        }
        if (sources.empty())
            return;

        activeVoiceProfileName_ = manifest->getProperty("presetName").toString();
        activeVoiceProfileId_ = directoryName;
        activeVoiceProfileQuality_ =
            manifest->getProperty("quality").toString() == "high" ? "high" : "balanced";
        activeVoiceProfileDirectory_ = profileDirectory;
        activeVoiceProfileSources_ = std::move(sources);
        activeVoiceAdapterFile_ = juce::File();
        const auto adapterRelativePath = manifest->getProperty("adapterFile").toString();
        if (adapterRelativePath.isNotEmpty())
        {
            const auto adapterFile = profileDirectory.getChildFile(adapterRelativePath);
            if (adapterFile.existsAsFile()
                && adapterFile.getFullPathName().startsWith(
                    profileDirectory.getFullPathName()
                        + juce::File::getSeparatorString()))
            {
                activeVoiceAdapterFile_ = adapterFile;
            }
        }
        if (voiceTrainingSources_.empty())
            voiceTrainingSources_ = std::move(trainingSources);
        voiceTrainingPhase_ = "complete";
        voiceTrainingMessage_ =
            "The active local profile is ready for new backing-vocal renders.";
        voiceTrainingProgress_ = 1.0;
        lastActiveProfilePointerModificationMs_ =
            activeFile.getLastModificationTime().toMilliseconds();
    }

    VoiceTrainingSource inspectVoiceTrainingSource(const juce::File& file)
    {
        VoiceTrainingSource source;
        source.file = file;
        source.status = "rejected";
        source.message = "The audio file could not be read.";

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return source;

        source.durationSeconds =
            static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        source.sampleRate = reader->sampleRate;
        source.channels = static_cast<int>(reader->numChannels);

        if (source.durationSeconds < 3.0)
        {
            source.message = "Too short. Use recordings that are at least 3 seconds long.";
            return source;
        }

        source.usable = true;
        if (source.sampleRate < 32000.0)
        {
            source.status = "warning";
            source.message = "Usable, but 44.1 kHz or 48 kHz audio will preserve more detail.";
        }
        else if (source.channels > 1)
        {
            source.status = "warning";
            source.message = "Usable. Mono dry vocals are preferred; this file will be downmixed.";
        }
        else
        {
            source.status = "ready";
            source.message = "Ready for the local voice profile.";
        }
        return source;
    }

    void addVoiceTrainingSource(const juce::File& file)
    {
        if (! file.existsAsFile())
            return;

        const auto path = file.getFullPathName();
        const auto duplicate = std::any_of(
            voiceTrainingSources_.begin(),
            voiceTrainingSources_.end(),
            [&path](const auto& source)
            {
                return source.file.getFullPathName() == path;
            });
        if (! duplicate)
            voiceTrainingSources_.push_back(inspectVoiceTrainingSource(file));
    }

    std::vector<juce::var> readVoiceTrainingJobStates() const
    {
        juce::Array<juce::File> directories;
        voiceTrainingJobsRoot().findChildFiles(
            directories,
            juce::File::findDirectories,
            false);

        std::vector<juce::var> jobs;
        jobs.reserve(static_cast<size_t>(directories.size()));
        for (const auto& directory : directories)
        {
            const auto stateFile = directory.getChildFile("state.json");
            if (! stateFile.existsAsFile())
                continue;
            const auto state = juce::JSON::parse(stateFile);
            if (state.getDynamicObject() != nullptr)
                jobs.push_back(state);
        }
        std::sort(
            jobs.begin(),
            jobs.end(),
            [](const auto& left, const auto& right)
            {
                const auto* leftObject = left.getDynamicObject();
                const auto* rightObject = right.getDynamicObject();
                if (leftObject == nullptr || rightObject == nullptr)
                    return leftObject != nullptr;
                return leftObject->getProperty("createdAt").toString()
                    > rightObject->getProperty("createdAt").toString();
            });
        return jobs;
    }

    static bool isVoiceTrainingJobActive(const juce::var& state)
    {
        const auto* object = state.getDynamicObject();
        if (object == nullptr)
            return false;
        const auto status = object->getProperty("status").toString();
        return status != "complete"
            && status != "cancelled"
            && status != "error";
    }

    bool voiceFineTuningAvailable() const
    {
       #if JUCE_MAC && (defined(__arm64__) || defined(__aarch64__))
        const auto script = juce::File(SYNTHETIC_OBSIDIAN_ROOT)
                                .getChildFile("tools")
                                .getChildFile("vocal_annotation_tool")
                                .getChildFile("voice_lora_training_worker.py");
        const auto model = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                               .getChildFile(".synthetic_obsidian")
                               .getChildFile("models")
                               .getChildFile("SoulX-Singer")
                               .getChildFile("model-svc.pt");
        return script.existsAsFile()
            && model.existsAsFile()
            && juce::File(soulXPythonExecutable()).existsAsFile();
       #else
        return false;
       #endif
    }

    void sendWebVoiceTrainingState()
    {
        if (! webFrontendReady_)
            return;

        constexpr double minimumDurationSeconds = 30.0;
        constexpr double recommendedDurationSeconds = 180.0;
        const auto totalDuration = voiceTrainingUsableDuration();
        const auto jobs = readVoiceTrainingJobStates();
        const auto hasActiveJob = std::any_of(
            jobs.begin(),
            jobs.end(),
            [](const auto& job) { return isVoiceTrainingJobActive(job); });
        juce::Array<juce::var> sources;
        for (size_t index = 0; index < voiceTrainingSources_.size(); ++index)
        {
            const auto& source = voiceTrainingSources_[index];
            auto* item = new juce::DynamicObject();
            item->setProperty("id", juce::String(static_cast<int>(index)));
            item->setProperty("name", source.file.getFileName());
            item->setProperty("durationSeconds", source.durationSeconds);
            item->setProperty("sampleRate", source.sampleRate);
            item->setProperty("channels", source.channels);
            item->setProperty("status", source.status);
            item->setProperty("message", source.message);
            sources.add(item);
        }

        auto* state = new juce::DynamicObject();
        state->setProperty("phase", voiceTrainingPhase_);
        state->setProperty("message", voiceTrainingMessage_);
        state->setProperty("progress", voiceTrainingProgress_);
       #if JUCE_MAC && (defined(__arm64__) || defined(__aarch64__))
        state->setProperty("device", "Apple Silicon / Local");
        state->setProperty("isAppleSilicon", true);
       #else
        state->setProperty("device", "Local CPU");
        state->setProperty("isAppleSilicon", false);
       #endif
        state->setProperty("sources", sources);
        state->setProperty("totalDurationSeconds", totalDuration);
        state->setProperty("minimumDurationSeconds", minimumDurationSeconds);
        state->setProperty("recommendedDurationSeconds", recommendedDurationSeconds);
        state->setProperty(
            "canCreateProfile",
            ! voiceProfileCreationRunning_ && totalDuration >= minimumDurationSeconds);
        state->setProperty("activeProfileName", activeVoiceProfileName_);
        state->setProperty("outputDirectory", activeVoiceProfileDirectory_.getFullPathName());
        state->setProperty("fineTuningAvailable", voiceFineTuningAvailable());
        state->setProperty(
            "canStartTraining",
            voiceFineTuningAvailable()
                && ! hasActiveJob
                && totalDuration >= minimumDurationSeconds);
        juce::Array<juce::var> jobValues;
        for (const auto& job : jobs)
            jobValues.add(job);
        state->setProperty("jobs", jobValues);

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "voice-training-state");
        event->setProperty("state", state);
        webView_.dispatch(event);
    }

    void chooseVoiceTrainingFiles()
    {
        if (voiceProfileCreationRunning_)
            return;

        chooser_ = std::make_unique<juce::FileChooser>(
            "Add clean vocal recordings",
            juce::File(),
            supportedAudioFileWildcard());
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::canSelectMultipleItems,
            [this](const juce::FileChooser& chooser)
            {
                for (const auto& file : chooser.getResults())
                    addVoiceTrainingSource(file);

                if (! voiceTrainingSources_.empty())
                {
                    voiceTrainingPhase_ = "ready";
                    voiceTrainingMessage_ =
                        voiceTrainingUsableDuration() >= 30.0
                            ? "The recordings are ready to create a local voice profile."
                            : "Add at least 30 seconds of usable solo vocal.";
                    sendWebVoiceTrainingState();
                }
            });
    }

    void cancelVoiceProfileCreation()
    {
        if (! voiceProfileCreationRunning_ || voiceProfileCancellation_ == nullptr)
            return;

        voiceProfileCancellation_->store(true, std::memory_order_release);
        voiceTrainingPhase_ = "cancelling";
        voiceTrainingMessage_ = "Cancelling profile preparation...";
        sendWebVoiceTrainingState();
    }

    void startVoiceLoRATraining(
        const juce::String& requestedName,
        const juce::String& requestedQuality)
    {
        constexpr double minimumDurationSeconds = 30.0;
        if (! voiceFineTuningAvailable())
        {
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ = "The Apple Silicon LoRA training runtime is unavailable.";
            sendWebVoiceTrainingState();
            return;
        }
        const auto existingJobs = readVoiceTrainingJobStates();
        if (std::any_of(
                existingJobs.begin(),
                existingJobs.end(),
                [](const auto& job) { return isVoiceTrainingJobActive(job); }))
        {
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ =
                "Another voice training job is already running. Cancel it or wait for it to finish.";
            sendWebVoiceTrainingState();
            return;
        }
        if (voiceTrainingUsableDuration() < minimumDurationSeconds)
        {
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ = "Add at least 30 seconds of usable solo vocal.";
            sendWebVoiceTrainingState();
            return;
        }

        auto presetName = requestedName.trim().substring(0, 64);
        if (presetName.isEmpty())
            presetName = "My Voice";
        const juce::String quality =
            requestedQuality == "high" ? "high" : "balanced";
        const auto jobId =
            "lora_" + juce::String(juce::Time::currentTimeMillis())
                + "_" + juce::Uuid().toString().substring(0, 8);
        const auto jobsRoot = voiceTrainingJobsRoot();
        const auto jobDirectory = jobsRoot.getChildFile(jobId);
        if (jobDirectory.createDirectory().failed())
        {
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ = "Could not create the persistent training job.";
            sendWebVoiceTrainingState();
            return;
        }

        auto profileName = juce::File::createLegalFileName(presetName).trim();
        if (profileName.isEmpty())
            profileName = "voice";
        profileName << "-lora-" << juce::Uuid().toString().substring(0, 8);
        const auto profileDirectory = voiceProfilesRoot().getChildFile(profileName);
        const auto createdAt = juce::Time::getCurrentTime().toISO8601(true);

        juce::Array<juce::var> sources;
        double totalDurationSeconds = 0.0;
        for (const auto& source : voiceTrainingSources_)
        {
            if (! source.usable || ! source.file.existsAsFile())
                continue;
            auto* item = new juce::DynamicObject();
            item->setProperty("path", source.file.getFullPathName());
            item->setProperty("originalName", source.file.getFileName());
            item->setProperty("durationSeconds", source.durationSeconds);
            item->setProperty("sampleRate", source.sampleRate);
            item->setProperty("channels", source.channels);
            sources.add(item);
            totalDurationSeconds += source.durationSeconds;
        }

        auto* lora = new juce::DynamicObject();
        lora->setProperty("rank", quality == "high" ? 16 : 8);
        lora->setProperty("alpha", quality == "high" ? 16.0 : 8.0);
        lora->setProperty("dropout", 0.05);
        juce::Array<juce::var> targets;
        targets.add("q_proj");
        targets.add("v_proj");
        if (quality == "high")
        {
            targets.add("k_proj");
            targets.add("o_proj");
        }
        lora->setProperty("target_suffixes", targets);
        lora->setProperty(
            "module_prefix",
            "cfm_decoder.model.diff_estimator.layers.");

        auto* training = new juce::DynamicObject();
        training->setProperty("steps", quality == "high" ? 800 : 400);
        training->setProperty("segmentSeconds", quality == "high" ? 4.0 : 3.5);
        training->setProperty("maximumSegments", quality == "high" ? 64 : 40);
        training->setProperty("learningRate", quality == "high" ? 0.00005 : 0.0001);
        training->setProperty("gradientAccumulation", 2);
        training->setProperty("seed", 1337);
        training->setProperty("lora", lora);

        const auto sourceRoot = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                    .getChildFile(".synthetic_obsidian")
                                    .getChildFile("runtime")
                                    .getChildFile("soulx-singer")
                                    .getChildFile("source");
        const auto modelPath = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                   .getChildFile(".synthetic_obsidian")
                                   .getChildFile("models")
                                   .getChildFile("SoulX-Singer")
                                   .getChildFile("model-svc.pt");
        auto* request = new juce::DynamicObject();
        request->setProperty("schemaVersion", 1);
        request->setProperty("jobId", jobId);
        request->setProperty("presetName", presetName);
        request->setProperty("quality", quality);
        request->setProperty("createdAt", createdAt);
        request->setProperty("sources", sources);
        request->setProperty("totalDurationSeconds", totalDurationSeconds);
        request->setProperty("profileDirectory", profileDirectory.getFullPathName());
        request->setProperty("sourceRoot", sourceRoot.getFullPathName());
        request->setProperty("modelPath", modelPath.getFullPathName());
        request->setProperty(
            "configPath",
            sourceRoot.getChildFile("soulxsinger")
                .getChildFile("config")
                .getChildFile("soulxsinger.yaml")
                .getFullPathName());
        request->setProperty("training", training);

        const auto requestFile = jobDirectory.getChildFile("request.json");
        if (! requestFile.replaceWithText(
                juce::JSON::toString(juce::var(request), true)))
        {
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ = "Could not write the persistent training request.";
            sendWebVoiceTrainingState();
            return;
        }

        auto* state = new juce::DynamicObject();
        state->setProperty("schemaVersion", 1);
        state->setProperty("jobId", jobId);
        state->setProperty("presetName", presetName);
        state->setProperty("quality", quality);
        state->setProperty("status", "queued");
        state->setProperty("stage", "Queued");
        state->setProperty("message", "Starting the independent training process...");
        state->setProperty("progress", 0.0);
        state->setProperty("currentStep", 0);
        state->setProperty("totalSteps", quality == "high" ? 800 : 400);
        state->setProperty("createdAt", createdAt);
        state->setProperty("updatedAt", createdAt);
        state->setProperty("profileDirectory", profileDirectory.getFullPathName());
        state->setProperty("adapterFile", juce::String());
        state->setProperty("canCancel", true);
        const auto stateValue = juce::var(state);
        jobDirectory.getChildFile("state.json").replaceWithText(
            juce::JSON::toString(stateValue, true));

        const auto script = juce::File(SYNTHETIC_OBSIDIAN_ROOT)
                                .getChildFile("tools")
                                .getChildFile("vocal_annotation_tool")
                                .getChildFile("voice_lora_training_worker.py");
        auto process = std::make_unique<juce::ChildProcess>();
        juce::StringArray arguments;
        arguments.add(soulXPythonExecutable());
        arguments.add(script.getFullPathName());
        arguments.add("--job-dir");
        arguments.add(jobDirectory.getFullPathName());
        if (! process->start(arguments, 0))
        {
            state->setProperty("status", "error");
            state->setProperty("stage", "Failed");
            state->setProperty("message", "Could not launch the independent training process.");
            state->setProperty("canCancel", false);
            jobDirectory.getChildFile("state.json").replaceWithText(
                juce::JSON::toString(stateValue, true));
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ = "Could not launch the independent training process.";
            sendWebVoiceTrainingState();
            return;
        }

        detachedTrainingProcesses_.push_back(std::move(process));
        voiceTrainingPhase_ = "ready";
        voiceTrainingMessage_ =
            "LoRA training started. It will continue if the application closes.";
        setStatus("Independent LoRA voice training started: " + presetName + ".");
        sendWebVoiceTrainingState();
    }

    void cancelVoiceLoRATraining(const juce::String& requestedJobId)
    {
        const auto jobId = juce::File(requestedJobId).getFileName();
        if (jobId.isEmpty() || jobId != requestedJobId)
            return;
        const auto jobDirectory = voiceTrainingJobsRoot().getChildFile(jobId);
        const auto stateFile = jobDirectory.getChildFile("state.json");
        if (! stateFile.existsAsFile())
            return;
        const auto state = juce::JSON::parse(stateFile);
        if (! isVoiceTrainingJobActive(state))
            return;

        if (jobDirectory.getChildFile("cancel.request").replaceWithText(
                "cancel\n"))
        {
            setStatus("Cancelling LoRA training: " + jobId + "...");
            sendWebVoiceTrainingState();
        }
    }

    void revealVoiceTrainingJob(const juce::String& requestedJobId)
    {
        const auto jobId = juce::File(requestedJobId).getFileName();
        if (jobId.isEmpty() || jobId != requestedJobId)
            return;
        const auto jobDirectory = voiceTrainingJobsRoot().getChildFile(jobId);
        if (jobDirectory.isDirectory())
            jobDirectory.revealToUser();
    }

    void createVoiceProfile(const juce::String& requestedName, const juce::String& requestedQuality)
    {
        constexpr double minimumDurationSeconds = 30.0;
        if (voiceProfileCreationRunning_)
            return;
        if (voiceTrainingUsableDuration() < minimumDurationSeconds)
        {
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ = "Add at least 30 seconds of usable solo vocal.";
            sendWebVoiceTrainingState();
            return;
        }

        auto presetName = requestedName.trim().substring(0, 64);
        if (presetName.isEmpty())
            presetName = "My Voice";
        const auto quality = requestedQuality == "high" ? "high" : "balanced";

        std::vector<VoiceTrainingSource> sources;
        double totalDurationSeconds = 0.0;
        for (const auto& source : voiceTrainingSources_)
            if (source.usable)
            {
                sources.push_back(source);
                totalDurationSeconds += source.durationSeconds;
            }

        const auto modelsRoot = voiceProfilesRoot();
        auto directoryName = juce::File::createLegalFileName(presetName).trim();
        if (directoryName.isEmpty())
            directoryName = "voice";
        directoryName << "-" << juce::Uuid().toString().substring(0, 8);
        const auto profileDirectory = modelsRoot.getChildFile(directoryName);

        voiceProfileCancellation_ = std::make_shared<std::atomic<bool>>(false);
        const auto cancellation = voiceProfileCancellation_;
        voiceProfileCreationRunning_ = true;
        voiceTrainingPhase_ = "preparing";
        voiceTrainingMessage_ = "Copying and indexing vocal recordings...";
        voiceTrainingProgress_ = 0.0;
        sendWebVoiceTrainingState();
        setStatus("Preparing local voice profile: " + presetName + "...");

        juce::WeakReference<MainComponent> safeThis(this);
        const auto started = juce::Thread::launch(
            [safeThis,
             cancellation,
             sources,
             presetName,
             quality,
             totalDurationSeconds,
             profileDirectory]
            {
                const auto finishWithError =
                    [safeThis](const juce::String& message)
                    {
                        juce::MessageManager::callAsync(
                            [safeThis, message]
                            {
                                if (safeThis == nullptr)
                                    return;
                                safeThis->voiceProfileCreationRunning_ = false;
                                safeThis->voiceTrainingPhase_ = "error";
                                safeThis->voiceTrainingMessage_ = message;
                                safeThis->voiceTrainingProgress_ = 0.0;
                                safeThis->setStatus("Voice profile failed: " + message);
                                safeThis->sendWebVoiceTrainingState();
                            });
                    };

                const auto sourcesDirectory = profileDirectory.getChildFile("sources");
                if (sourcesDirectory.createDirectory().failed())
                {
                    finishWithError("Could not create the local voice profile directory.");
                    return;
                }

                std::vector<juce::File> copiedSources;
                copiedSources.reserve(sources.size());
                juce::Array<juce::var> manifestSources;
                for (size_t index = 0; index < sources.size(); ++index)
                {
                    if (cancellation->load(std::memory_order_acquire))
                    {
                        profileDirectory.deleteRecursively();
                        juce::MessageManager::callAsync(
                            [safeThis]
                            {
                                if (safeThis == nullptr)
                                    return;
                                safeThis->voiceProfileCreationRunning_ = false;
                                safeThis->voiceTrainingPhase_ = "cancelled";
                                safeThis->voiceTrainingMessage_ = "Profile preparation was cancelled.";
                                safeThis->voiceTrainingProgress_ = 0.0;
                                safeThis->setStatus("Voice profile preparation cancelled.");
                                safeThis->sendWebVoiceTrainingState();
                            });
                        return;
                    }

                    const auto& source = sources[index];
                    auto prefix = juce::String(static_cast<int>(index + 1)).paddedLeft('0', 2);
                    const auto target = sourcesDirectory.getChildFile(
                        prefix + "_" + juce::File::createLegalFileName(source.file.getFileName()));
                    if (! source.file.copyFileTo(target))
                    {
                        profileDirectory.deleteRecursively();
                        finishWithError("Could not copy " + source.file.getFileName() + ".");
                        return;
                    }
                    copiedSources.push_back(target);

                    auto* manifestSource = new juce::DynamicObject();
                    manifestSource->setProperty(
                        "file",
                        target.getRelativePathFrom(profileDirectory));
                    manifestSource->setProperty("originalName", source.file.getFileName());
                    manifestSource->setProperty("durationSeconds", source.durationSeconds);
                    manifestSource->setProperty("sampleRate", source.sampleRate);
                    manifestSource->setProperty("channels", source.channels);
                    manifestSources.add(manifestSource);

                    const auto progress =
                        0.08 + 0.82 * static_cast<double>(index + 1)
                            / static_cast<double>(sources.size());
                    juce::MessageManager::callAsync(
                        [safeThis, progress, fileName = source.file.getFileName()]
                        {
                            if (safeThis == nullptr)
                                return;
                            safeThis->voiceTrainingProgress_ = progress;
                            safeThis->voiceTrainingMessage_ = "Preparing " + fileName + "...";
                            safeThis->sendWebVoiceTrainingState();
                        });
                }

                if (cancellation->load(std::memory_order_acquire))
                {
                    profileDirectory.deleteRecursively();
                    juce::MessageManager::callAsync(
                        [safeThis]
                        {
                            if (safeThis == nullptr)
                                return;
                            safeThis->voiceProfileCreationRunning_ = false;
                            safeThis->voiceTrainingPhase_ = "cancelled";
                            safeThis->voiceTrainingMessage_ = "Profile preparation was cancelled.";
                            safeThis->voiceTrainingProgress_ = 0.0;
                            safeThis->setStatus("Voice profile preparation cancelled.");
                            safeThis->sendWebVoiceTrainingState();
                        });
                    return;
                }

                auto* manifest = new juce::DynamicObject();
                manifest->setProperty("schemaVersion", 1);
                manifest->setProperty("presetName", presetName);
                manifest->setProperty("kind", "reference_profile");
                manifest->setProperty("engine", "soulx_reference");
                manifest->setProperty("trainingStatus", "profile_ready");
                manifest->setProperty("fineTuned", false);
                manifest->setProperty("quality", quality);
                manifest->setProperty("totalDurationSeconds", totalDurationSeconds);
                manifest->setProperty(
                    "createdAt",
                    juce::Time::getCurrentTime().toISO8601(true));
                manifest->setProperty("sources", manifestSources);
                if (! copiedSources.empty())
                {
                    manifest->setProperty(
                        "primaryReference",
                        copiedSources.front().getRelativePathFrom(profileDirectory));
                }

                const auto manifestFile = profileDirectory.getChildFile("manifest.json");
                if (! manifestFile.replaceWithText(
                        juce::JSON::toString(juce::var(manifest), true)))
                {
                    profileDirectory.deleteRecursively();
                    finishWithError("Could not write the voice profile manifest.");
                    return;
                }

                auto* active = new juce::DynamicObject();
                active->setProperty("directory", profileDirectory.getFileName());
                active->setProperty("presetName", presetName);
                profileDirectory.getParentDirectory()
                    .getChildFile("active.json")
                    .replaceWithText(juce::JSON::toString(juce::var(active), true));

                juce::MessageManager::callAsync(
                    [safeThis, presetName, quality, profileDirectory, copiedSources]
                    {
                        if (safeThis == nullptr)
                            return;
                        safeThis->voiceProfileCreationRunning_ = false;
                        safeThis->voiceTrainingPhase_ = "complete";
                        safeThis->voiceTrainingMessage_ =
                            "The profile is active for new backing-vocal renders.";
                        safeThis->voiceTrainingProgress_ = 1.0;
                        safeThis->activeVoiceProfileName_ = presetName;
                        safeThis->activeVoiceProfileQuality_ = quality;
                        safeThis->activeVoiceProfileDirectory_ = profileDirectory;
                        safeThis->activeVoiceProfileSources_ = copiedSources;
                        safeThis->setStatus(
                            "Voice profile ready and active: " + presetName + ".");
                        safeThis->sendWebVoiceTrainingState();
                    });
            });

        if (! started)
        {
            voiceProfileCreationRunning_ = false;
            voiceTrainingPhase_ = "error";
            voiceTrainingMessage_ = "Could not start the background profile task.";
            voiceTrainingProgress_ = 0.0;
            sendWebVoiceTrainingState();
        }
    }

    void handleVoiceTrainingAction(juce::DynamicObject& root)
    {
        const auto action = root.getProperty("action").toString();
        if (action == "request-state")
        {
            sendWebVoiceTrainingState();
        }
        else if (action == "add-files")
        {
            chooseVoiceTrainingFiles();
        }
        else if (action == "add-current-track")
        {
            if (! document_.audioFile.existsAsFile())
            {
                voiceTrainingPhase_ = "error";
                voiceTrainingMessage_ = "Open a main vocal track before adding it.";
            }
            else
            {
                addVoiceTrainingSource(document_.audioFile);
                voiceTrainingPhase_ = "ready";
                voiceTrainingMessage_ =
                    voiceTrainingUsableDuration() >= 30.0
                        ? "The recordings are ready to create a local voice profile."
                        : "Add at least 30 seconds of usable solo vocal.";
            }
            sendWebVoiceTrainingState();
        }
        else if (action == "remove-file" && ! voiceProfileCreationRunning_)
        {
            const auto index = root.getProperty("sourceId").toString().getIntValue();
            if (index >= 0 && index < static_cast<int>(voiceTrainingSources_.size()))
                voiceTrainingSources_.erase(voiceTrainingSources_.begin() + index);
            voiceTrainingPhase_ = voiceTrainingSources_.empty() ? "idle" : "ready";
            voiceTrainingMessage_ = voiceTrainingSources_.empty()
                ? "Add clean, dry vocal recordings to build a reusable voice profile."
                : "Review the remaining recordings.";
            sendWebVoiceTrainingState();
        }
        else if (action == "clear-files" && ! voiceProfileCreationRunning_)
        {
            voiceTrainingSources_.clear();
            voiceTrainingPhase_ = "idle";
            voiceTrainingMessage_ =
                "Add clean, dry vocal recordings to build a reusable voice profile.";
            sendWebVoiceTrainingState();
        }
        else if (action == "create-profile")
        {
            createVoiceProfile(
                root.getProperty("presetName").toString(),
                root.getProperty("quality").toString());
        }
        else if (action == "start-training")
        {
            startVoiceLoRATraining(
                root.getProperty("presetName").toString(),
                root.getProperty("quality").toString());
        }
        else if (action == "cancel-training")
        {
            cancelVoiceLoRATraining(root.getProperty("jobId").toString());
        }
        else if (action == "reveal-training")
        {
            revealVoiceTrainingJob(root.getProperty("jobId").toString());
        }
        else if (action == "cancel")
        {
            cancelVoiceProfileCreation();
        }
        else if (action == "reveal-profile"
                 && activeVoiceProfileDirectory_.isDirectory())
        {
            activeVoiceProfileDirectory_.revealToUser();
        }
    }

    void completeExport(const juce::String& summary, const juce::File& directory)
    {
        setStatus(summary + " to " + directory.getFullPathName() + ".");

        if (webFrontendReady_)
        {
            auto* event = new juce::DynamicObject();
            event->setProperty("type", "export-complete");
            event->setProperty("message", summary + ".");
            event->setProperty("directory", directory.getFullPathName());
            webView_.dispatch(event);
        }

        if (! directory.startAsProcess())
            directory.revealToUser();
    }

    void sendWebTransportState()
    {
        if (! webFrontendReady_)
            return;

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "transport-state");
        event->setProperty("playing", isPlaybackRunning());
        event->setProperty("playhead", normalisedTimelinePosition(currentTransportPosition()));
        webView_.dispatch(event);
    }

    void sendWebLoopState()
    {
        if (! webFrontendReady_)
            return;

        auto* range = new juce::DynamicObject();
        range->setProperty("start", normalisedTimelinePosition(loopStartTime_));
        range->setProperty(
            "end",
            loopEndTime_ > loopStartTime_
                ? normalisedTimelinePosition(loopEndTime_)
                : 25.0);

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "loop-state");
        event->setProperty("active", cycleLoopActive_);
        event->setProperty("range", range);
        webView_.dispatch(event);
    }

    void sendWebVolumeState()
    {
        if (! webFrontendReady_)
            return;

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "volume-state");
        event->setProperty("value", masterVolume_.load() * 100.0f);
        webView_.dispatch(event);
    }

    void sendWebOutputMeterState()
    {
        constexpr float release = 0.88f;
        for (size_t channel = 0; channel < outputMeterLevels_.size(); ++channel)
        {
            const auto measuredPeak = outputPeakLevels_[channel].exchange(
                0.0f,
                std::memory_order_relaxed);
            outputMeterLevels_[channel] = juce::jmax(
                measuredPeak,
                outputMeterLevels_[channel] * release);
            if (outputMeterLevels_[channel] < 0.00001f)
                outputMeterLevels_[channel] = 0.0f;
        }

        if (! webFrontendReady_)
            return;

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "output-meter-state");
        event->setProperty("left", outputMeterLevels_[0]);
        event->setProperty("right", outputMeterLevels_[1]);
        webView_.dispatch(event);
    }

    void sendWebTrackState(const juce::String& track, bool muted, bool soloed)
    {
        if (! webFrontendReady_)
            return;

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "track-state");
        event->setProperty("track", track);
        event->setProperty("state", makeTrackState(muted, soloed));
        webView_.dispatch(event);
    }

    void sendWebTrackLayerState(const juce::String& track, bool audioMuted, bool notesMuted)
    {
        if (! webFrontendReady_)
            return;

        auto* state = new juce::DynamicObject();
        state->setProperty("audioMuted", audioMuted);
        state->setProperty("notesMuted", notesMuted);

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "track-layer-state");
        event->setProperty("track", track);
        event->setProperty("state", state);
        webView_.dispatch(event);
    }

    BackingVocalTrack* findBackingTrack(const juce::String& styleId,
                                        const juce::String& styleName)
    {
        const auto track = std::find_if(
            document_.backingTracks.begin(),
            document_.backingTracks.end(),
            [&styleId, &styleName](const auto& candidate)
            {
                return (styleId.isNotEmpty() && candidate.styleId == styleId)
                    || (styleName.isNotEmpty() && candidate.styleName == styleName);
            });
        return track != document_.backingTracks.end() ? &*track : nullptr;
    }

    const BackingVocalTrack* findBackingTrack(const juce::String& styleId,
                                              const juce::String& styleName) const
    {
        const auto track = std::find_if(
            document_.backingTracks.begin(),
            document_.backingTracks.end(),
            [&styleId, &styleName](const auto& candidate)
            {
                return (styleId.isNotEmpty() && candidate.styleId == styleId)
                    || (styleName.isNotEmpty() && candidate.styleName == styleName);
            });
        return track != document_.backingTracks.end() ? &*track : nullptr;
    }

    std::vector<NoteBlock>* notesForPitchTrack(const juce::String& trackName)
    {
        if (trackName == "Voice Main")
            return &document_.notes;
        if (auto* track = findBackingTrack({}, trackName))
            return &track->notes;
        return nullptr;
    }

    const std::vector<NoteBlock>* notesForPitchTrack(const juce::String& trackName) const
    {
        if (trackName == "Voice Main")
            return &document_.notes;
        if (const auto* track = findBackingTrack({}, trackName))
            return &track->notes;
        return nullptr;
    }

    juce::String normaliseWebClipId(const juce::String& trackName,
                                    const juce::String& clipId) const
    {
        if (trackName == "Voice Main")
            return clipId;
        if (const auto* track = findBackingTrack({}, trackName))
        {
            const auto prefix = track->styleId + ":";
            if (track->styleId.isNotEmpty() && clipId.startsWith(prefix))
                return clipId.substring(prefix.length());
        }
        return clipId;
    }

    static std::optional<size_t> findPitchNoteIndex(const std::vector<NoteBlock>& notes,
                                                    const juce::String& noteId)
    {
        const auto note = std::find_if(notes.begin(), notes.end(), [&noteId](const auto& candidate)
        {
            return candidate.id == noteId;
        });
        if (note == notes.end())
            return std::nullopt;
        return static_cast<size_t>(std::distance(notes.begin(), note));
    }

    static juce::String nextPitchNoteId(const std::vector<NoteBlock>& notes)
    {
        for (int suffix = static_cast<int>(notes.size()) + 1; ; ++suffix)
        {
            const auto candidate = "web-note-" + juce::String(suffix);
            if (! findPitchNoteIndex(notes, candidate))
                return candidate;
        }
    }

    PitchTrackSnapshot capturePitchTrackSnapshot(const juce::String& trackName) const
    {
        PitchTrackSnapshot snapshot;
        if (const auto* notes = notesForPitchTrack(trackName))
            snapshot.notes = *notes;
        if (trackName == "Voice Main")
            snapshot.regions = document_.regions;
        return snapshot;
    }

    void beginPitchTrackEdit(const juce::String& trackName)
    {
        auto& history = pitchTrackHistories_[trackName];
        history.undo.push_back(capturePitchTrackSnapshot(trackName));
        if (history.undo.size() > kMaxUndoSnapshots)
            history.undo.erase(history.undo.begin());
        history.redo.clear();
    }

    void syncActiveBackingTrackNotes(const juce::String& trackName)
    {
        if (trackName == "Voice Main")
            return;
        if (auto* track = findBackingTrack({}, trackName))
            if (track->styleId == document_.backingStyleId
                || track->styleName == document_.backingStyleName)
            {
                document_.backingNotes = track->notes;
            }
    }

    void applyPitchTrackSnapshot(const juce::String& trackName,
                                 const PitchTrackSnapshot& snapshot)
    {
        if (auto* notes = notesForPitchTrack(trackName))
            *notes = snapshot.notes;
        if (trackName == "Voice Main")
            document_.regions = snapshot.regions;
        syncActiveBackingTrackNotes(trackName);
    }

    void finishPitchTrackEdit(const juce::String& trackName)
    {
        syncActiveBackingTrackNotes(trackName);
        markChanged();
        if (trackName == "Voice Main")
            scheduleLeadPitchPreview();
        else
            scheduleBackingPitchPreview(trackName);
    }

    void undoPitchTrack(const juce::String& trackName)
    {
        auto history = pitchTrackHistories_.find(trackName);
        if (history == pitchTrackHistories_.end() || history->second.undo.empty())
            return;

        history->second.redo.push_back(capturePitchTrackSnapshot(trackName));
        applyPitchTrackSnapshot(trackName, history->second.undo.back());
        history->second.undo.pop_back();
        finishPitchTrackEdit(trackName);
    }

    void redoPitchTrack(const juce::String& trackName)
    {
        auto history = pitchTrackHistories_.find(trackName);
        if (history == pitchTrackHistories_.end() || history->second.redo.empty())
            return;

        history->second.undo.push_back(capturePitchTrackSnapshot(trackName));
        applyPitchTrackSnapshot(trackName, history->second.redo.back());
        history->second.redo.pop_back();
        finishPitchTrackEdit(trackName);
    }

    void clearPitchTrackHistory(const juce::String& trackName)
    {
        pitchTrackHistories_.erase(trackName);
    }

    bool hasPitchTrackUndo(const juce::String& trackName) const
    {
        const auto history = pitchTrackHistories_.find(trackName);
        return history != pitchTrackHistories_.end() && ! history->second.undo.empty();
    }

    void makeBackingTrackActive(const BackingVocalTrack& track)
    {
        document_.backingStyleId = track.styleId;
        document_.backingStyleName = track.styleName;
        document_.backingNotes = track.notes;
        document_.backingAudioFile = track.audioFile;
        const auto trackIndex = backingStyleIndex(track.styleId, track.styleName);
        activeBackingTrackIndex_.store(trackIndex, std::memory_order_relaxed);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(kMaxBackingTracks))
        {
            const auto& cache = backingTrackWaveforms_[static_cast<size_t>(trackIndex)];
            if (cache.file == track.audioFile)
            {
                backingWaveformFile_ = cache.file;
                backingWaveform_ = cache.waveform;
                backingPackedWaveform_ = cache.packedWaveform;
                backingWaveformLoading_ = cache.loading;
                return;
            }
        }

        backingWaveformFile_ = juce::File();
        backingWaveform_.clear();
        backingPackedWaveform_.clear();
        backingWaveformLoading_ = false;
    }

    void selectBackingTrackForEditing(const juce::String& trackName)
    {
        auto* track = findBackingTrack({}, trackName);
        if (track == nullptr)
            return;

        const auto transportPosition = currentTransportPosition();
        const auto shouldKeepPlaying = isPlaybackRunning();
        const auto preview = backingPitchPreviewFiles_.find(trackName);
        const auto auditionFile =
            hasPitchTrackUndo(trackName)
                && preview != backingPitchPreviewFiles_.end()
                && preview->second.existsAsFile()
            ? preview->second
            : track->audioFile;
        const auto sourceAlreadyConfigured =
            backingAudioReaderSource_ != nullptr && backingAudioTransportFile_ == auditionFile;
        makeBackingTrackActive(*track);

        if (auditionFile.existsAsFile())
        {
            editor_.setBackingAudioFile(auditionFile);
            if (! sourceAlreadyConfigured)
                configureBackingAudioTransportForFile(auditionFile);
            backingAudioTransport_.setPosition(transportPosition);
            if (shouldKeepPlaying)
                backingAudioTransport_.start();

            const auto trackIndex = backingStyleIndex(track->styleId, track->styleName);
            if (trackIndex >= 0)
                requestWaveformData(WaveformTrack::backing, track->audioFile, trackIndex);
        }
        else
        {
            clearBackingAudioTrack();
        }
    }

    std::vector<AlgorithmicPitchCorrection> buildPitchCorrections(
        const std::vector<NoteBlock>& currentNotes,
        const std::vector<NoteBlock>& sourceNotes) const
    {
        std::vector<AlgorithmicPitchCorrection> corrections;
        corrections.reserve(currentNotes.size());
        for (const auto& note : currentNotes)
        {
            if (note.end <= note.start)
                continue;

            const NoteBlock* renderedNote = nullptr;
            const auto sameId = std::find_if(
                sourceNotes.begin(),
                sourceNotes.end(),
                [&note](const auto& candidate) { return candidate.id == note.id; });
            if (sameId != sourceNotes.end())
            {
                renderedNote = &*sameId;
            }
            else
            {
                auto bestOverlap = 0.0;
                for (const auto& candidate : sourceNotes)
                {
                    const auto overlap = juce::jmax(
                        0.0,
                        juce::jmin(note.end, candidate.end)
                            - juce::jmax(note.start, candidate.start));
                    if (overlap > bestOverlap)
                    {
                        bestOverlap = overlap;
                        renderedNote = &candidate;
                    }
                }
            }
            if (renderedNote == nullptr || renderedNote->end <= renderedNote->start)
                continue;

            const auto noteProgress = 0.5;
            const auto targetTime = note.start + (note.end - note.start) * noteProgress;
            const auto renderedTime = renderedNote->start
                + (renderedNote->end - renderedNote->start) * noteProgress;
            const auto pitchDelta = interpolatedNoteMidi(note, targetTime)
                - interpolatedNoteMidi(*renderedNote, renderedTime);
            if (std::abs(pitchDelta) < 0.02)
                continue;

            corrections.push_back({
                note.start,
                note.end,
                juce::jlimit(
                    0.5f,
                    2.0f,
                    static_cast<float>(std::pow(2.0, pitchDelta / 12.0)))
            });
        }
        return corrections;
    }

    void restoreOriginalLeadAudio()
    {
        if (! leadPitchPreviewFile_.existsAsFile())
        {
            leadPitchPreviewFile_ = juce::File();
            return;
        }

        const auto previewFile = leadPitchPreviewFile_;
        leadPitchPreviewFile_ = juce::File();
        const auto position = currentTransportPosition();
        const auto wasPlaying = transport_.isPlaying();
        if (document_.audioFile.existsAsFile())
        {
            editor_.setAudioFile(document_.audioFile);
            configureTransportForFile(document_.audioFile);
            transport_.setPosition(position);
            if (wasPlaying)
            {
                transport_.start();
                playButton_.setButtonText("Stop");
            }
        }
        previewFile.deleteFile();
        setStatus("Restored original main vocal audio.");
    }

    void scheduleLeadPitchPreview()
    {
        const auto revision = ++leadPitchPreviewRevision_;
        const auto history = pitchTrackHistories_.find("Voice Main");
        if (history == pitchTrackHistories_.end()
            || history->second.undo.empty()
            || ! document_.audioFile.existsAsFile()
            || history->second.undo.front().notes.empty())
        {
            restoreOriginalLeadAudio();
            return;
        }

        const auto sourceFile = document_.audioFile;
        const auto outputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getNonexistentChildFile(
                                        "synthetic_obsidian_lead_pitch_preview",
                                        ".wav");
        const auto corrections = buildPitchCorrections(
            document_.notes,
            history->second.undo.front().notes);
        setStatus("Preparing algorithmic pitch preview for Voice Main...");

        juce::WeakReference<MainComponent> safeThis(this);
        juce::Thread::launch(
            [safeThis, sourceFile, outputFile, corrections, revision]
            {
                const auto result =
                    AlgorithmicPitchPreview::render(sourceFile, outputFile, corrections);
                const auto error = result.failed() ? result.getErrorMessage() : juce::String();
                juce::MessageManager::callAsync(
                    [safeThis, sourceFile, outputFile, revision, error]
                    {
                        if (safeThis != nullptr)
                            safeThis->applyLeadPitchPreview(
                                sourceFile,
                                outputFile,
                                revision,
                                error);
                        else
                            outputFile.deleteFile();
                    });
            });
    }

    void applyLeadPitchPreview(const juce::File& sourceFile,
                               const juce::File& outputFile,
                               unsigned int revision,
                               const juce::String& error)
    {
        if (revision != leadPitchPreviewRevision_
            || document_.audioFile != sourceFile
            || ! hasPitchTrackUndo("Voice Main"))
        {
            outputFile.deleteFile();
            return;
        }

        if (error.isNotEmpty() || ! outputFile.existsAsFile())
        {
            outputFile.deleteFile();
            setStatus(
                "Could not prepare algorithmic pitch preview for Voice Main: "
                + (error.isNotEmpty() ? error : "preview WAV was not created."));
            return;
        }

        const auto previousFile = leadPitchPreviewFile_;
        leadPitchPreviewFile_ = outputFile;
        const auto position = currentTransportPosition();
        const auto wasPlaying = transport_.isPlaying();
        editor_.setAudioFile(outputFile);
        configureTransportForFile(outputFile);
        transport_.setPosition(position);
        if (wasPlaying)
        {
            transport_.start();
            playButton_.setButtonText("Stop");
        }
        if (previousFile.existsAsFile() && previousFile != outputFile)
            previousFile.deleteFile();
        setStatus("Algorithmic pitch preview ready for Voice Main.");
    }

    void restoreRenderedBackingAudio(const juce::String& trackName)
    {
        auto preview = backingPitchPreviewFiles_.find(trackName);
        if (preview == backingPitchPreviewFiles_.end())
        {
            if (selectedWebTrack_ == trackName)
                if (const auto* track = findBackingTrack({}, trackName);
                    track != nullptr && track->audioFile.existsAsFile())
                {
                    setStatus("Using rendered backing audio for " + trackName + ".");
                }
            return;
        }

        const auto previewFile = preview->second;
        backingPitchPreviewFiles_.erase(preview);
        if (selectedWebTrack_ == trackName)
        {
            if (const auto* track = findBackingTrack({}, trackName))
            {
                const auto position = currentTransportPosition();
                const auto wasPlaying = isPlaybackRunning();
                editor_.setBackingAudioFile(track->audioFile);
                configureBackingAudioTransportForFile(track->audioFile);
                backingAudioTransport_.setPosition(position);
                if (wasPlaying)
                    backingAudioTransport_.start();
            }
        }
        previewFile.deleteFile();
        setStatus("Restored rendered backing audio for " + trackName + ".");
    }

    void scheduleBackingPitchPreview(const juce::String& trackName)
    {
        const auto revision = ++backingPitchPreviewRevisions_[trackName];
        const auto* track = findBackingTrack({}, trackName);
        if (track == nullptr
            || ! hasPitchTrackUndo(trackName)
            || ! track->audioFile.existsAsFile()
            || track->renderedNotes.empty())
        {
            restoreRenderedBackingAudio(trackName);
            return;
        }

        const auto sourceFile = track->audioFile;
        const auto outputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getNonexistentChildFile(
                                        "synthetic_obsidian_pitch_preview",
                                        ".wav");
        const auto corrections = buildPitchCorrections(track->notes, track->renderedNotes);
        setStatus("Preparing algorithmic pitch preview for " + trackName + "...");

        juce::WeakReference<MainComponent> safeThis(this);
        juce::Thread::launch(
            [safeThis, trackName, sourceFile, outputFile, corrections, revision]
            {
                const auto result =
                    AlgorithmicPitchPreview::render(sourceFile, outputFile, corrections);
                const auto error = result.failed() ? result.getErrorMessage() : juce::String();
                juce::MessageManager::callAsync(
                    [safeThis, trackName, sourceFile, outputFile, revision, error]
                    {
                        if (safeThis != nullptr)
                            safeThis->applyBackingPitchPreview(
                                trackName,
                                sourceFile,
                                outputFile,
                                revision,
                                error);
                        else
                            outputFile.deleteFile();
                    });
            });
    }

    void applyBackingPitchPreview(const juce::String& trackName,
                                  const juce::File& sourceFile,
                                  const juce::File& outputFile,
                                  unsigned int revision,
                                  const juce::String& error)
    {
        const auto latestRevision = backingPitchPreviewRevisions_.find(trackName);
        const auto* track = findBackingTrack({}, trackName);
        if (latestRevision == backingPitchPreviewRevisions_.end()
            || latestRevision->second != revision
            || track == nullptr
            || track->audioFile != sourceFile
            || ! hasPitchTrackUndo(trackName))
        {
            outputFile.deleteFile();
            return;
        }

        if (error.isNotEmpty() || ! outputFile.existsAsFile())
        {
            outputFile.deleteFile();
            setStatus(
                "Could not prepare algorithmic pitch preview for "
                + trackName
                + ": "
                + (error.isNotEmpty() ? error : "preview WAV was not created."));
            return;
        }

        const auto previousPreview = backingPitchPreviewFiles_.find(trackName);
        const auto previousFile = previousPreview != backingPitchPreviewFiles_.end()
            ? previousPreview->second
            : juce::File();
        backingPitchPreviewFiles_[trackName] = outputFile;

        if (selectedWebTrack_ == trackName)
        {
            const auto position = currentTransportPosition();
            const auto wasPlaying = isPlaybackRunning();
            editor_.setBackingAudioFile(outputFile);
            configureBackingAudioTransportForFile(outputFile);
            backingAudioTransport_.setPosition(position);
            if (wasPlaying)
                backingAudioTransport_.start();
        }
        if (previousFile.existsAsFile() && previousFile != outputFile)
            previousFile.deleteFile();
        setStatus("Algorithmic pitch preview ready for " + trackName + ".");
    }

    bool anyBackingTrackSoloed() const noexcept
    {
        for (const auto& soloed : backingTrackSoloed_)
            if (soloed.load(std::memory_order_relaxed))
                return true;
        return false;
    }

    void resetBackingTrackControls() noexcept
    {
        for (size_t i = 0; i < kMaxBackingTracks; ++i)
        {
            backingTrackMuted_[i].store(false, std::memory_order_relaxed);
            backingTrackSoloed_[i].store(false, std::memory_order_relaxed);
            backingTrackAudioMuted_[i].store(false, std::memory_order_relaxed);
            backingTrackNotesMuted_[i].store(false, std::memory_order_relaxed);
            auto& waveform = backingTrackWaveforms_[i];
            ++waveform.requestRevision;
            waveform.file = juce::File();
            waveform.waveform.clear();
            waveform.packedWaveform.clear();
            waveform.durationSeconds = 0.0;
            waveform.loading = false;
        }
        backingWaveformFile_ = juce::File();
        backingWaveform_.clear();
        backingPackedWaveform_.clear();
        backingWaveformLoading_ = false;
        activeBackingTrackIndex_.store(-1, std::memory_order_relaxed);
    }

    void sendWebProjectState()
    {
        if (! webFrontendReady_)
            return;

        const auto voiceProfiles = readVoiceProfileOptions();
        const auto effectiveVoiceProfileId =
            [this, &voiceProfiles](const juce::String& requestedId)
        {
            if (std::any_of(
                    voiceProfiles.begin(),
                    voiceProfiles.end(),
                    [&requestedId](const auto& profile)
                    {
                        return profile.id == requestedId;
                    }))
            {
                return requestedId;
            }
            if (std::any_of(
                    voiceProfiles.begin(),
                    voiceProfiles.end(),
                    [this](const auto& profile)
                    {
                        return profile.id == activeVoiceProfileId_;
                    }))
            {
                return activeVoiceProfileId_;
            }
            return voiceProfiles.empty()
                ? juce::String()
                : voiceProfiles.front().id;
        };

        juce::Array<juce::var> backingOptions;
        for (const auto& style : kBackingStyles)
            backingOptions.add(style.name);

        juce::Array<juce::var> backingTracks;
        for (const auto& track : document_.backingTracks)
            if (track.styleName.isNotEmpty())
                backingTracks.add(track.styleName);
        if (backingTracks.isEmpty()
            && ! document_.backingNotes.empty()
            && document_.backingStyleName.isNotEmpty())
        {
            backingTracks.add(document_.backingStyleName);
        }

        juce::Array<juce::var> trackStates;
        const auto addTrackState = [&trackStates](const juce::String& track, bool muted, bool soloed)
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty("track", track);
            entry->setProperty("state", makeTrackState(muted, soloed));
            trackStates.add(entry);
        };
        addTrackState("Instrumental", instrumentalMuted_.load(), instrumentalSolo_.load());
        addTrackState("Voice Main", leadMuted_.load(), leadSolo_.load());
        for (const auto& backing : backingTracks)
        {
            const auto index = backingStyleIndex({}, backing.toString());
            const auto hasIndex = index >= 0 && index < static_cast<int>(kMaxBackingTracks);
            addTrackState(
                backing.toString(),
                hasIndex
                    ? backingTrackMuted_[static_cast<size_t>(index)].load(std::memory_order_relaxed)
                    : backingMuted_.load(),
                hasIndex
                    ? backingTrackSoloed_[static_cast<size_t>(index)].load(std::memory_order_relaxed)
                    : backingSolo_.load());
        }

        juce::Array<juce::var> trackLayerState;
        const auto addTrackLayerState =
            [&trackLayerState](const juce::String& track, bool audioMuted, bool notesMuted)
        {
            auto* state = new juce::DynamicObject();
            state->setProperty("audioMuted", audioMuted);
            state->setProperty("notesMuted", notesMuted);

            auto* entry = new juce::DynamicObject();
            entry->setProperty("track", track);
            entry->setProperty("state", state);
            trackLayerState.add(entry);
        };
        addTrackLayerState(
            "Voice Main",
            leadAudioMuted_.load(),
            leadNotesMuted_.load());
        for (const auto& backing : backingTracks)
        {
            const auto index = backingStyleIndex({}, backing.toString());
            const auto hasIndex = index >= 0 && index < static_cast<int>(kMaxBackingTracks);
            addTrackLayerState(
                backing.toString(),
                hasIndex
                    ? backingTrackAudioMuted_[static_cast<size_t>(index)].load(std::memory_order_relaxed)
                    : backingAudioMuted_.load(),
                hasIndex
                    ? backingTrackNotesMuted_[static_cast<size_t>(index)].load(std::memory_order_relaxed)
                    : backingNotesMuted_.load());
        }

        static constexpr const char* clipColours[] {
            "violet", "cyan", "silver", "pink", "lime", "amber"
        };
        juce::Array<juce::var> clips;
        const auto duration = timelineDuration();
        for (size_t i = 0; i < document_.notes.size(); ++i)
        {
            const auto& note = document_.notes[i];
            auto* clip = new juce::DynamicObject();
            clip->setProperty("id", note.id);
            clip->setProperty("label", note.lyric.isNotEmpty() ? note.lyric : note.id);
            clip->setProperty("x", juce::jlimit(0.0, 100.0, note.start * 100.0 / duration));
            clip->setProperty("pitch", juce::jlimit(0.0, 127.0, note.pitchExact));
            clip->setProperty("gainDb", juce::jlimit(-24.0, 12.0, note.gainDb));
            clip->setProperty(
                "width",
                juce::jlimit(
                    0.0,
                    100.0,
                    juce::jmax(0.0, note.end - note.start) * 100.0 / duration));
            clip->setProperty("color", clipColours[i % std::size(clipColours)]);
            clip->setProperty(
                "legatoFromPrevious",
                note.flags.contains("legato_from_previous"));
            clip->setProperty("legatoToNext", note.flags.contains("legato_to_next"));

            juce::Array<juce::var> pitchCurve;
            constexpr size_t maxCurvePoints = 64;
            const auto curveStep = juce::jmax(
                static_cast<size_t>(1),
                note.curve.size() / maxCurvePoints);
            for (size_t pointIndex = 0; pointIndex < note.curve.size(); pointIndex += curveStep)
            {
                const auto& point = note.curve[pointIndex];
                if (point.time < note.start || point.time > note.end)
                    continue;

                auto* curvePoint = new juce::DynamicObject();
                curvePoint->setProperty(
                    "x",
                    juce::jlimit(0.0, 100.0, point.time * 100.0 / duration));
                curvePoint->setProperty("pitch", juce::jlimit(0.0, 127.0, point.midi));
                pitchCurve.add(curvePoint);
            }
            if (note.curve.size() > 1)
            {
                const auto& finalPoint = note.curve.back();
                const auto finalX = juce::jlimit(
                    0.0,
                    100.0,
                    finalPoint.time * 100.0 / duration);
                const auto lastX = pitchCurve.isEmpty()
                    ? -1.0
                    : static_cast<double>(
                        pitchCurve.getLast().getDynamicObject()->getProperty("x"));
                if (std::abs(finalX - lastX) > 0.0001)
                {
                    auto* curvePoint = new juce::DynamicObject();
                    curvePoint->setProperty("x", finalX);
                    curvePoint->setProperty(
                        "pitch",
                        juce::jlimit(0.0, 127.0, finalPoint.midi));
                    pitchCurve.add(curvePoint);
                }
            }
            clip->setProperty("pitchCurve", pitchCurve);
            clips.add(clip);
        }

        juce::Array<juce::var> backingClips;
        for (size_t i = 0; i < document_.backingNotes.size(); ++i)
        {
            const auto& note = document_.backingNotes[i];
            auto* clip = new juce::DynamicObject();
            clip->setProperty("id", note.id);
            clip->setProperty("label", note.lyric.isNotEmpty() ? note.lyric : note.id);
            clip->setProperty("x", juce::jlimit(0.0, 100.0, note.start * 100.0 / duration));
            clip->setProperty("pitch", juce::jlimit(0.0, 127.0, note.pitchExact));
            clip->setProperty("gainDb", juce::jlimit(-24.0, 12.0, note.gainDb));
            clip->setProperty(
                "width",
                juce::jlimit(
                    0.0,
                    100.0,
                    juce::jmax(0.0, note.end - note.start) * 100.0 / duration));
            clip->setProperty("color", clipColours[(i + 1) % std::size(clipColours)]);
            backingClips.add(clip);
        }

        juce::Array<juce::var> backingTrackContents;
        for (const auto& track : document_.backingTracks)
        {
            auto* trackContent = new juce::DynamicObject();
            trackContent->setProperty("track", track.styleName);
            trackContent->setProperty("hasAudio", track.audioFile.existsAsFile());
            trackContent->setProperty(
                "voiceProfileId",
                effectiveVoiceProfileId(track.voiceProfileId));
            const auto isActiveTrack = track.styleId == document_.backingStyleId
                || track.styleName == document_.backingStyleName;
            const auto trackIndex = backingStyleIndex(track.styleId, track.styleName);
            const auto hasTrackWaveform = trackIndex >= 0
                && trackIndex < static_cast<int>(kMaxBackingTracks);
            const auto& trackWaveform = hasTrackWaveform
                ? backingTrackWaveforms_[static_cast<size_t>(trackIndex)].waveform
                : backingWaveform_;
            const auto& trackPackedWaveform = hasTrackWaveform
                ? backingTrackWaveforms_[static_cast<size_t>(trackIndex)].packedWaveform
                : backingPackedWaveform_;
            const auto cachedDuration = hasTrackWaveform
                && backingTrackWaveforms_[static_cast<size_t>(trackIndex)].file == track.audioFile
                ? backingTrackWaveforms_[static_cast<size_t>(trackIndex)].durationSeconds
                : 0.0;
            trackContent->setProperty(
                "audioDurationSeconds",
                track.audioFile.existsAsFile()
                    ? (cachedDuration > 0.0
                        ? cachedDuration
                        : isActiveTrack && backingAudioReaderSource_ != nullptr
                        ? backingAudioTransport_.getLengthInSeconds()
                        : document_.duration)
                    : 0.0);
            trackContent->setProperty(
                "waveform",
                makeWaveformState(trackWaveform, trackPackedWaveform));

            juce::Array<juce::var> trackClips;
            for (size_t i = 0; i < track.notes.size(); ++i)
            {
                const auto& note = track.notes[i];
                auto* clip = new juce::DynamicObject();
                clip->setProperty("id", track.styleId + ":" + note.id);
                clip->setProperty("label", note.lyric.isNotEmpty() ? note.lyric : note.id);
                clip->setProperty("x", juce::jlimit(0.0, 100.0, note.start * 100.0 / duration));
                clip->setProperty("pitch", juce::jlimit(0.0, 127.0, note.pitchExact));
                clip->setProperty("gainDb", juce::jlimit(-24.0, 12.0, note.gainDb));
                clip->setProperty(
                    "width",
                    juce::jlimit(
                        0.0,
                        100.0,
                        juce::jmax(0.0, note.end - note.start) * 100.0 / duration));
                clip->setProperty("color", clipColours[(i + 1) % std::size(clipColours)]);
                trackClips.add(clip);
            }
            trackContent->setProperty("clips", trackClips);
            backingTrackContents.add(trackContent);
        }

        juce::Array<juce::var> chords;
        for (const auto& chord : document_.chords)
        {
            auto* item = new juce::DynamicObject();
            item->setProperty("label", chord.name);
            item->setProperty(
                "width",
                juce::jlimit(0.0, 100.0, (chord.end - chord.start) * 100.0 / duration));
            chords.add(item);
        }

        juce::Array<juce::var> timeSignatures;
        for (const auto& signature : document_.timeSignatures)
        {
            auto* item = new juce::DynamicObject();
            const auto start = juce::jlimit(0.0, duration, signature.start);
            item->setProperty("start", start);
            item->setProperty("end", juce::jlimit(start, duration, signature.end));
            item->setProperty("numerator", signature.numerator);
            item->setProperty("denominator", signature.denominator);
            timeSignatures.add(item);
        }
        if (timeSignatures.isEmpty())
        {
            auto* item = new juce::DynamicObject();
            item->setProperty("start", 0.0);
            item->setProperty("end", duration);
            item->setProperty("numerator", 4);
            item->setProperty("denominator", 4);
            timeSignatures.add(item);
        }

        juce::Array<juce::var> tempoSegments;
        for (const auto& tempo : document_.tempoSegments)
        {
            auto* item = new juce::DynamicObject();
            const auto start = juce::jlimit(0.0, duration, tempo.start);
            item->setProperty("start", start);
            item->setProperty("end", juce::jlimit(start, duration, tempo.end));
            item->setProperty("bpm", tempo.bpm);
            tempoSegments.add(item);
        }
        if (tempoSegments.isEmpty())
        {
            auto* item = new juce::DynamicObject();
            item->setProperty("start", 0.0);
            item->setProperty("end", duration);
            item->setProperty("bpm", document_.bpm);
            tempoSegments.add(item);
        }

        const auto meter = document_.timeSignatures.empty()
            ? juce::String("4 / 4")
            : juce::String(document_.timeSignatures.front().numerator)
                + " / "
                + juce::String(document_.timeSignatures.front().denominator);

        auto* cycleRange = new juce::DynamicObject();
        cycleRange->setProperty("start", normalisedTimelinePosition(loopStartTime_));
        cycleRange->setProperty(
            "end",
            loopEndTime_ > loopStartTime_
                ? normalisedTimelinePosition(loopEndTime_)
                : 25.0);

        juce::Array<juce::var> pitchEditorHistory;
        const auto addPitchHistoryState = [this, &pitchEditorHistory](const juce::String& trackName)
        {
            const auto history = pitchTrackHistories_.find(trackName);
            auto* state = new juce::DynamicObject();
            state->setProperty("track", trackName);
            state->setProperty(
                "canUndo",
                history != pitchTrackHistories_.end() && ! history->second.undo.empty());
            state->setProperty(
                "canRedo",
                history != pitchTrackHistories_.end() && ! history->second.redo.empty());
            pitchEditorHistory.add(state);
        };
        addPitchHistoryState("Voice Main");
        for (const auto& track : document_.backingTracks)
            addPitchHistoryState(track.styleName);

        juce::Array<juce::var> voiceProfileValues;
        for (const auto& profile : voiceProfiles)
        {
            auto* value = new juce::DynamicObject();
            value->setProperty("id", profile.id);
            value->setProperty("name", profile.name);
            value->setProperty("quality", profile.quality);
            value->setProperty("active", profile.active);
            voiceProfileValues.add(value);
        }

        auto* project = new juce::DynamicObject();
        project->setProperty("tempo", document_.bpm);
        project->setProperty("meter", meter);
        project->setProperty("hasInstrumental", document_.instrumentalFile.existsAsFile());
        project->setProperty("hasVocal", document_.audioFile.existsAsFile());
        project->setProperty("hasBackingAudio", document_.backingAudioFile.existsAsFile());
        project->setProperty(
            "instrumentalDurationSeconds",
            instrumentalReaderSource_ != nullptr ? instrumentalTransport_.getLengthInSeconds() : 0.0);
        project->setProperty(
            "vocalDurationSeconds",
            readerSource_ != nullptr ? transport_.getLengthInSeconds() : 0.0);
        project->setProperty(
            "backingAudioDurationSeconds",
            backingAudioReaderSource_ != nullptr ? backingAudioTransport_.getLengthInSeconds() : 0.0);
        project->setProperty("timelineStartSeconds", 0.0);
        project->setProperty("timelineDurationSeconds", duration);
        project->setProperty("initialPlayhead", normalisedTimelinePosition(currentTransportPosition()));
        project->setProperty("initialVolume", masterVolume_.load() * 100.0f);
        project->setProperty("initialLoopActive", cycleLoopActive_);
        project->setProperty("initialCycleRange", cycleRange);
        project->setProperty("backingTrackOptions", backingOptions);
        project->setProperty("voiceProfiles", voiceProfileValues);
        project->setProperty("initialBackingTracks", backingTracks);
        project->setProperty("initialTrackState", trackStates);
        project->setProperty("initialTrackLayerState", trackLayerState);
        project->setProperty("clips", clips);
        project->setProperty("backingClips", backingClips);
        project->setProperty("backingTrackContents", backingTrackContents);
        project->setProperty("pitchEditorHistory", pitchEditorHistory);
        project->setProperty(
            "vocalWaveform",
            makeWaveformState(vocalWaveform_, vocalPackedWaveform_));
        project->setProperty(
            "instrumentalWaveform",
            makeWaveformState(instrumentalWaveform_, instrumentalPackedWaveform_));
        project->setProperty(
            "backingWaveform",
            makeWaveformState(backingWaveform_, backingPackedWaveform_));
        project->setProperty("chords", chords);
        project->setProperty("timeSignatures", timeSignatures);
        project->setProperty("tempoSegments", tempoSegments);

        auto* event = new juce::DynamicObject();
        event->setProperty("type", "project-state");
        event->setProperty("project", project);
        webView_.dispatch(event);
    }

    void handleProjectAction(const juce::String& action)
    {
        if (action == "open-audio")              chooseAudioFile();
        else if (action == "open-instrumental") chooseInstrumentalFile();
        else if (action == "open-project")      chooseProject();
        else if (action == "save-project")      saveProject();
        else if (action == "load-json")         chooseJsonFile();
        else if (action == "load-lyrics")       chooseLyricsFile();
        else if (action == "analyze")           runCombinedAnalysis();
        else if (action == "ai-parts")          runAiPartsAnalysis();
        else if (action == "generate-backing")  runBackingVocalGeneration();
        else if (action == "render-backing")    runBackingAudioRender();
        else if (action == "save")              saveProject();
        else if (action == "undo")
        {
            if (selectedWebTrack_.isNotEmpty()
                && selectedWebTrack_ != "Instrumental"
                && hasPitchTrackUndo(selectedWebTrack_))
            {
                undoPitchTrack(selectedWebTrack_);
            }
            else
            {
                undo();
            }
        }
        else if (action == "redo")
        {
            const auto history = pitchTrackHistories_.find(selectedWebTrack_);
            if (selectedWebTrack_.isNotEmpty()
                && selectedWebTrack_ != "Instrumental"
                && history != pitchTrackHistories_.end()
                && ! history->second.redo.empty())
            {
                redoPitchTrack(selectedWebTrack_);
            }
            else
            {
                redo();
            }
        }
        else if (action == "export-all-tracks") exportAllTracks();
        else if (action == "export-midi")       exportMidi();
        else if (action == "validate")          updateValidationStatus();
    }

    void handleTransportCommand(const juce::String& action)
    {
        if (action == "return-to-start")
        {
            stopPlayback();
            setTransportPosition(0.0);
        }
        else if (action == "play")
        {
            if (! isPlaybackRunning())
                togglePlayback();
        }
        else if (action == "pause" || action == "stop")
        {
            if (isPlaybackRunning())
                stopPlayback();
        }

        sendWebTransportState();
    }

    bool handleWebPitchEdit(juce::DynamicObject& root, const juce::String& type)
    {
        const auto isPitchEdit = type == "add-clip"
            || type == "delete-clips"
            || type == "move-clips"
            || type == "set-clip-time"
            || type == "split-clip"
            || type == "join-clips"
            || type == "set-clip-vibrato"
            || type == "set-clip-gain";
        if (! isPitchEdit)
            return false;

        const auto trackName = root.getProperty("track").toString();
        auto* targetNotes = notesForPitchTrack(trackName);
        if (trackName.isEmpty() || targetNotes == nullptr)
            return true;
        const auto snapToChord =
            trackName != "Voice Main"
            && static_cast<bool>(root.getProperty("snapToChord"));
        const auto snappedPitch =
            [this, snapToChord](double pitch, double noteStart)
        {
            return snapToChord
                ? snapPitchToChord(pitch, noteStart, document_.chords)
                : pitch;
        };

        const auto normaliseId = [this, &trackName](const juce::String& id)
        {
            return normaliseWebClipId(trackName, id);
        };
        const auto readClipIds = [&normaliseId](const juce::var& value)
        {
            std::vector<juce::String> ids;
            if (auto* array = value.getArray())
                for (const auto& item : *array)
                {
                    const auto id = normaliseId(item.toString());
                    if (id.isNotEmpty()
                        && std::find(ids.begin(), ids.end(), id) == ids.end())
                    {
                        ids.push_back(id);
                    }
                }
            return ids;
        };
        const auto containsId = [](const std::vector<juce::String>& ids, const juce::String& id)
        {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        const auto findNoteIndex = [targetNotes](const juce::String& id)
        {
            return findPitchNoteIndex(*targetNotes, id);
        };
        const auto sortNotes = [targetNotes]
        {
            std::sort(targetNotes->begin(), targetNotes->end(), [](const auto& left, const auto& right)
            {
                return left.start < right.start;
            });
        };

        if (type == "add-clip")
        {
            if (document_.duration <= 0.0)
                return true;

            auto* clip = root.getProperty("clip").getDynamicObject();
            if (clip == nullptr)
                return true;

            auto id = normaliseId(clip->getProperty("id").toString());
            if (id.isEmpty() || findNoteIndex(id))
                id = nextPitchNoteId(*targetNotes);
            const auto duration = timelineDuration();
            const auto start = juce::jlimit(
                0.0,
                juce::jmax(0.0, document_.duration - kMinimumWebNoteLengthSeconds),
                static_cast<double>(clip->getProperty("x")) * duration / 100.0);
            const auto requestedLength =
                static_cast<double>(clip->getProperty("width")) * duration / 100.0;
            const auto end = juce::jlimit(
                start + kMinimumWebNoteLengthSeconds,
                document_.duration,
                start + juce::jmax(kMinimumWebNoteLengthSeconds, requestedLength));
            const auto pitch = snappedPitch(
                juce::jlimit(
                    0.0,
                    127.0,
                    static_cast<double>(clip->getProperty("pitch"))),
                start);
            if (end <= start)
                return true;

            beginPitchTrackEdit(trackName);
            NoteBlock note;
            note.id = id;
            note.start = start;
            note.end = end;
            note.pitch = juce::jlimit(0, 127, static_cast<int>(std::lround(pitch)));
            note.pitchExact = pitch;
            note.voicedStart = start;
            note.voicedEnd = end;
            note.flags.add("manual");
            note.curve.push_back({ start, pitch, 1.0 });
            note.curve.push_back({ end, pitch, 1.0 });
            targetNotes->push_back(std::move(note));
            sortNotes();
            finishPitchTrackEdit(trackName);
            return true;
        }

        if (type == "delete-clips")
        {
            const auto ids = readClipIds(root.getProperty("clipIds"));
            const auto hasMatchingNote = std::any_of(
                targetNotes->begin(),
                targetNotes->end(),
                [&ids, &containsId](const auto& note) { return containsId(ids, note.id); });
            if (! hasMatchingNote)
                return true;

            beginPitchTrackEdit(trackName);
            targetNotes->erase(
                std::remove_if(
                    targetNotes->begin(),
                    targetNotes->end(),
                    [&ids, &containsId](const auto& note) { return containsId(ids, note.id); }),
                targetNotes->end());
            if (trackName == "Voice Main")
            {
                document_.regions.erase(
                    std::remove_if(
                        document_.regions.begin(),
                        document_.regions.end(),
                        [&ids, &containsId](const auto& region)
                        {
                            return region.noteId.isNotEmpty() && containsId(ids, region.noteId);
                        }),
                    document_.regions.end());
            }
            finishPitchTrackEdit(trackName);
            return true;
        }

        if (type == "move-clips")
        {
            struct MoveEdit
            {
                juce::String id;
                double pitch = 60.0;
            };
            std::vector<MoveEdit> edits;
            if (auto* values = root.getProperty("clips").getArray())
                for (const auto& value : *values)
                    if (auto* edit = value.getDynamicObject())
                    {
                        const auto id = normaliseId(edit->getProperty("clipId").toString());
                        if (findNoteIndex(id))
                        {
                            edits.push_back({
                                id,
                                juce::jlimit(0.0, 127.0, static_cast<double>(edit->getProperty("pitch")))
                            });
                        }
                    }
            if (edits.empty())
                return true;

            beginPitchTrackEdit(trackName);
            for (const auto& edit : edits)
            {
                const auto index = findNoteIndex(edit.id);
                if (! index)
                    continue;
                auto& note = (*targetNotes)[*index];
                moveNoteToPitch(
                    note,
                    snappedPitch(edit.pitch, note.start));
            }
            sortNotes();
            finishPitchTrackEdit(trackName);
            return true;
        }

        if (type == "set-clip-time")
        {
            const auto id = normaliseId(root.getProperty("clipId").toString());
            const auto index = findNoteIndex(id);
            if (! index)
                return true;

            const auto duration = timelineDuration();
            const auto nextStart = juce::jlimit(
                0.0,
                juce::jmax(0.0, document_.duration - kMinimumWebNoteLengthSeconds),
                static_cast<double>(root.getProperty("x")) * duration / 100.0);
            const auto requestedLength =
                static_cast<double>(root.getProperty("width")) * duration / 100.0;
            const auto nextEnd = juce::jlimit(
                nextStart + kMinimumWebNoteLengthSeconds,
                document_.duration,
                nextStart + juce::jmax(kMinimumWebNoteLengthSeconds, requestedLength));
            if (nextEnd <= nextStart)
                return true;

            auto& note = (*targetNotes)[*index];
            if (std::abs(note.start - nextStart) < 0.0001
                && std::abs(note.end - nextEnd) < 0.0001)
            {
                return true;
            }

            beginPitchTrackEdit(trackName);
            const auto previousStart = note.start;
            const auto previousLength = juce::jmax(
                kMinimumWebNoteLengthSeconds,
                note.end - note.start);
            const auto nextLength = nextEnd - nextStart;
            const auto mapTime = [previousStart, previousLength, nextStart, nextLength](double time)
            {
                return nextStart + (time - previousStart) * nextLength / previousLength;
            };
            note.start = nextStart;
            note.end = nextEnd;
            note.voicedStart = juce::jlimit(note.start, note.end, mapTime(note.voicedStart));
            note.voicedEnd = juce::jlimit(note.voicedStart, note.end, mapTime(note.voicedEnd));
            for (auto& point : note.curve)
                point.time = juce::jlimit(note.start, note.end, mapTime(point.time));
            if (snapToChord)
                moveNoteToPitch(
                    note,
                    snappedPitch(note.pitchExact, note.start));
            if (trackName == "Voice Main")
                for (auto& region : document_.regions)
                    if (region.noteId == note.id)
                    {
                        region.start = juce::jlimit(note.start, note.end, mapTime(region.start));
                        region.end = juce::jlimit(region.start, note.end, mapTime(region.end));
                    }
            sortNotes();
            finishPitchTrackEdit(trackName);
            return true;
        }

        if (type == "split-clip")
        {
            const auto id = normaliseId(root.getProperty("clipId").toString());
            const auto index = findNoteIndex(id);
            if (! index)
                return true;

            const auto splitTime = juce::jlimit(
                0.0,
                document_.duration,
                static_cast<double>(root.getProperty("x")) * timelineDuration() / 100.0);
            auto& source = (*targetNotes)[*index];
            if (splitTime <= source.start + kMinimumWebNoteLengthSeconds
                || splitTime >= source.end - kMinimumWebNoteLengthSeconds)
            {
                return true;
            }

            beginPitchTrackEdit(trackName);
            const auto splitPitch = interpolatedNoteMidi(source, splitTime);
            auto right = source;
            auto rightId = normaliseId(root.getProperty("rightClipId").toString());
            if (rightId.isEmpty() || findNoteIndex(rightId))
                rightId = nextPitchNoteId(*targetNotes);
            right.id = rightId;
            right.start = splitTime;
            right.voicedStart = juce::jmax(splitTime, right.voicedStart);
            if (source.syllableId.isNotEmpty())
            {
                right.lyric.clear();
                right.flags.addIfNotAlreadyThere("melisma_continuation");
                right.flags.addIfNotAlreadyThere("legato_from_previous");
                source.flags.addIfNotAlreadyThere("legato_to_next");
            }

            std::vector<PitchCurvePoint> leftCurve;
            std::vector<PitchCurvePoint> rightCurve;
            for (const auto& point : source.curve)
                if (point.time < splitTime)
                    leftCurve.push_back(point);
                else if (point.time > splitTime)
                    rightCurve.push_back(point);
            leftCurve.push_back({ splitTime, splitPitch, 0.65 });
            rightCurve.insert(rightCurve.begin(), { splitTime, splitPitch, 0.65 });
            source.end = splitTime;
            source.voicedEnd = juce::jmin(splitTime, source.voicedEnd);
            source.curve = std::move(leftCurve);
            right.curve = std::move(rightCurve);
            updateRepresentativePitch(source);
            updateRepresentativePitch(right);
            if (snapToChord)
            {
                moveNoteToPitch(
                    source,
                    snappedPitch(source.pitchExact, source.start));
                moveNoteToPitch(
                    right,
                    snappedPitch(right.pitchExact, right.start));
            }

            if (trackName == "Voice Main")
            {
                std::vector<AnnotationRegion> rightRegions;
                for (auto& region : document_.regions)
                    if (region.noteId == id)
                    {
                        if (region.start >= splitTime)
                        {
                            region.noteId = rightId;
                        }
                        else if (region.end > splitTime)
                        {
                            auto rightRegion = region;
                            rightRegion.start = splitTime;
                            rightRegion.noteId = rightId;
                            region.end = splitTime;
                            rightRegions.push_back(std::move(rightRegion));
                        }
                    }
                document_.regions.insert(
                    document_.regions.end(),
                    std::make_move_iterator(rightRegions.begin()),
                    std::make_move_iterator(rightRegions.end()));
            }
            targetNotes->push_back(std::move(right));
            sortNotes();
            finishPitchTrackEdit(trackName);
            return true;
        }

        if (type == "join-clips")
        {
            const auto ids = readClipIds(root.getProperty("clipIds"));
            std::vector<NoteBlock> selected;
            for (const auto& note : *targetNotes)
                if (containsId(ids, note.id))
                    selected.push_back(note);
            std::sort(selected.begin(), selected.end(), [](const auto& left, const auto& right)
            {
                return left.start < right.start;
            });
            if (selected.size() < 2)
                return true;

            beginPitchTrackEdit(trackName);
            auto joined = selected.front();
            const auto joinedId = joined.id;
            auto weightedGain = 0.0;
            auto totalDuration = 0.0;
            juce::StringArray lyrics;
            std::vector<PitchCurvePoint> joinedCurve;
            for (const auto& note : selected)
            {
                const auto noteDuration = juce::jmax(0.0, note.end - note.start);
                weightedGain += note.gainDb * noteDuration;
                totalDuration += noteDuration;
                if (note.lyric.trim().isNotEmpty())
                    lyrics.addIfNotAlreadyThere(note.lyric.trim());
                joined.end = juce::jmax(joined.end, note.end);
                joined.voicedStart = juce::jmin(joined.voicedStart, note.voicedStart);
                joined.voicedEnd = juce::jmax(joined.voicedEnd, note.voicedEnd);
                if (joined.syllableId.isEmpty())
                    joined.syllableId = note.syllableId;
                joinedCurve.insert(joinedCurve.end(), note.curve.begin(), note.curve.end());
            }
            joined.lyric = lyrics.joinIntoString(" ");
            joined.gainDb = totalDuration > 0.0 ? weightedGain / totalDuration : joined.gainDb;
            joined.flags.removeString("legato_to_next");
            if (selected.back().flags.contains("legato_to_next"))
                joined.flags.addIfNotAlreadyThere("legato_to_next");
            joined.curve = std::move(joinedCurve);
            std::sort(joined.curve.begin(), joined.curve.end(), [](const auto& left, const auto& right)
            {
                return left.time < right.time;
            });
            updateRepresentativePitch(joined);
            if (snapToChord)
                moveNoteToPitch(
                    joined,
                    snappedPitch(joined.pitchExact, joined.start));

            targetNotes->erase(
                std::remove_if(
                    targetNotes->begin(),
                    targetNotes->end(),
                    [&ids, &containsId](const auto& note) { return containsId(ids, note.id); }),
                targetNotes->end());
            targetNotes->push_back(std::move(joined));
            if (trackName == "Voice Main")
                for (auto& region : document_.regions)
                    if (containsId(ids, region.noteId))
                        region.noteId = joinedId;
            sortNotes();
            finishPitchTrackEdit(trackName);
            return true;
        }

        if (type == "set-clip-vibrato")
        {
            const auto id = normaliseId(root.getProperty("clipId").toString());
            const auto index = findNoteIndex(id);
            if (! index)
                return true;
            const auto scale = juce::jlimit(
                0.0,
                4.0,
                static_cast<double>(root.getProperty("scale")));
            if (std::abs(scale - 1.0) < 0.0001)
                return true;

            beginPitchTrackEdit(trackName);
            auto& note = (*targetNotes)[*index];
            if (note.curve.empty())
            {
                note.curve.push_back({ note.start, note.pitchExact, 1.0 });
                note.curve.push_back({ note.end, note.pitchExact, 1.0 });
            }
            for (auto& point : note.curve)
                point.midi = juce::jlimit(
                    0.0,
                    127.0,
                    note.pitchExact + (point.midi - note.pitchExact) * scale);
            finishPitchTrackEdit(trackName);
            return true;
        }

        if (type == "set-clip-gain")
        {
            const auto id = normaliseId(root.getProperty("clipId").toString());
            const auto index = findNoteIndex(id);
            if (! index)
                return true;
            const auto gainDb = juce::jlimit(
                -24.0,
                12.0,
                static_cast<double>(root.getProperty("gainDb")));
            auto& note = (*targetNotes)[*index];
            if (std::abs(note.gainDb - gainDb) < 0.0001)
                return true;

            beginPitchTrackEdit(trackName);
            note.gainDb = gainDb;
            finishPitchTrackEdit(trackName);
            return true;
        }

        return false;
    }

    void handleWebCommand(const juce::var& command)
    {
        auto* root = command.getDynamicObject();
        if (root == nullptr)
            return;

        const auto type = root->getProperty("type").toString();
        if (type == "frontend-ready")
        {
            webFrontendReady_ = true;
            sendWebProjectState();
            sendWebStatus();
            sendWebVoiceTrainingState();
            sendWebTransportState();
            sendWebLoopState();
            sendWebVolumeState();
            return;
        }

        if (type == "project-action")
        {
            handleProjectAction(root->getProperty("action").toString());
            return;
        }

        if (type == "voice-training")
        {
            handleVoiceTrainingAction(*root);
            return;
        }

        if (type == "transport")
        {
            handleTransportCommand(root->getProperty("action").toString());
            return;
        }

        if (type == "set-playhead")
        {
            const auto position = juce::jlimit(
                0.0,
                100.0,
                static_cast<double>(root->getProperty("position")));
            stopPlayback();
            setTransportPosition(position * timelineDuration() / 100.0);
            sendWebTransportState();
            return;
        }

        if (type == "set-loop")
        {
            auto* range = root->getProperty("range").getDynamicObject();
            if (range == nullptr)
                return;

            const auto start = juce::jlimit(
                0.0,
                100.0,
                static_cast<double>(range->getProperty("start")));
            const auto end = juce::jlimit(
                start,
                100.0,
                static_cast<double>(range->getProperty("end")));
            loopStartTime_ = start * timelineDuration() / 100.0;
            loopEndTime_ = end * timelineDuration() / 100.0;
            cycleLoopActive_ = static_cast<bool>(root->getProperty("active"));
            sendWebLoopState();
            return;
        }

        if (type == "set-volume")
        {
            const auto value = juce::jlimit(
                0.0,
                100.0,
                static_cast<double>(root->getProperty("value")));
            masterVolume_.store(static_cast<float>(value / 100.0));
            sendWebVolumeState();
            return;
        }

        if (type == "preview-pitch-tone")
        {
            previewPitchTone(juce::jlimit(
                0.0,
                127.0,
                static_cast<double>(root->getProperty("pitch"))),
                static_cast<bool>(root->getProperty("restart")));
            return;
        }

        if (type == "stop-pitch-tone")
        {
            stopPitchTone();
            return;
        }

        if (type == "set-track-state")
        {
            const auto track = root->getProperty("track").toString();
            auto* state = root->getProperty("state").getDynamicObject();
            if (state == nullptr)
                return;

            const auto muted = static_cast<bool>(state->getProperty("mute"));
            const auto soloed = static_cast<bool>(state->getProperty("solo"));
            if (track == "Instrumental")
            {
                instrumentalMuted_.store(muted);
                instrumentalSolo_.store(soloed);
            }
            else if (track == "Voice Main")
            {
                leadMuted_.store(muted);
                leadSolo_.store(soloed);
            }
            else
            {
                const auto index = backingStyleIndex({}, track);
                if (index < 0 || index >= static_cast<int>(kMaxBackingTracks))
                    return;
                backingTrackMuted_[static_cast<size_t>(index)].store(
                    muted,
                    std::memory_order_relaxed);
                backingTrackSoloed_[static_cast<size_t>(index)].store(
                    soloed,
                    std::memory_order_relaxed);
                if (track == document_.backingStyleName)
                {
                    backingMuted_.store(muted);
                    backingSolo_.store(soloed);
                }
            }
            sendWebTrackState(track, muted, soloed);
            return;
        }

        if (type == "set-track-layer-state")
        {
            const auto track = root->getProperty("track").toString();
            auto* state = root->getProperty("state").getDynamicObject();
            if (state == nullptr)
                return;

            const auto audioMuted = static_cast<bool>(state->getProperty("audioMuted"));
            const auto notesMuted = static_cast<bool>(state->getProperty("notesMuted"));
            if (track == "Voice Main")
            {
                leadAudioMuted_.store(audioMuted);
                leadNotesMuted_.store(notesMuted);
            }
            else if (track != "Instrumental")
            {
                const auto index = backingStyleIndex({}, track);
                if (index < 0 || index >= static_cast<int>(kMaxBackingTracks))
                    return;
                backingTrackAudioMuted_[static_cast<size_t>(index)].store(
                    audioMuted,
                    std::memory_order_relaxed);
                backingTrackNotesMuted_[static_cast<size_t>(index)].store(
                    notesMuted,
                    std::memory_order_relaxed);
                if (track == document_.backingStyleName)
                {
                    backingAudioMuted_.store(audioMuted);
                    backingNotesMuted_.store(notesMuted);
                }
            }
            else
                return;

            sendWebTrackLayerState(track, audioMuted, notesMuted);
            return;
        }

        if (type == "select-tracks")
        {
            auto* tracks = root->getProperty("tracks").getArray();
            if (tracks != nullptr && tracks->size() == 1)
            {
                const auto track = tracks->getFirst().toString();
                selectedWebTrack_ = track;
                if (track != "Instrumental" && track != "Voice Main")
                    selectBackingTrackForEditing(track);
            }
            else
            {
                selectedWebTrack_.clear();
            }
            return;
        }

        if (type == "pitch-history")
        {
            const auto track = root->getProperty("track").toString();
            const auto action = root->getProperty("action").toString();
            if (action == "undo")
                undoPitchTrack(track);
            else if (action == "redo")
                redoPitchTrack(track);
            return;
        }

        if (type == "add-backing-track" || type == "regenerate-backing-track")
        {
            const auto requestedTrack = root->getProperty("track").toString();
            for (size_t i = 0; i < kBackingStyles.size(); ++i)
            {
                if (requestedTrack == kBackingStyles[i].name)
                {
                    backingStyleBox_.setSelectedId(
                        static_cast<int>(i) + 1,
                        juce::dontSendNotification);
                    runBackingVocalGeneration();
                    break;
                }
            }
            return;
        }

        if (type == "set-backing-voice-profile")
        {
            if (backingAudioRenderRunning_)
                return;

            const auto trackName = root->getProperty("track").toString();
            const auto requestedProfileId =
                root->getProperty("profileId").toString();
            const auto profileId =
                juce::File(requestedProfileId).getFileName();
            if (profileId.isEmpty() || profileId != requestedProfileId)
                return;

            const auto profiles = readVoiceProfileOptions();
            const auto profile = std::find_if(
                profiles.begin(),
                profiles.end(),
                [&profileId](const auto& candidate)
                {
                    return candidate.id == profileId;
                });
            auto* backingTrack = findBackingTrack({}, trackName);
            if (profile == profiles.end() || backingTrack == nullptr)
                return;
            if (backingTrack->voiceProfileId == profileId)
                return;

            beginUndoableAction();
            backingTrack->voiceProfileId = profileId;
            markChanged();
            setStatus("Voice for " + trackName + ": " + profile->name + ".");
            return;
        }

        if (type == "render-backing-track")
        {
            const auto requestedTrack = root->getProperty("track").toString();
            auto* backingTrack = findBackingTrack({}, requestedTrack);
            if (backingTrack == nullptr)
            {
                setStatus("Could not render backing vocal: track not found.");
                return;
            }

            makeBackingTrackActive(*backingTrack);
            ++backingPitchPreviewRevisions_[requestedTrack];
            updatePlaybackNotes();
            runBackingAudioRender();
            return;
        }

        if (handleWebPitchEdit(*root, type))
            return;

        if (type == "set-clip-pitch")
        {
            const auto trackName = root->getProperty("track").toString();
            auto* notes = notesForPitchTrack(trackName);
            const auto webClipId = root->getProperty("clipId").toString();
            const auto clipId = normaliseWebClipId(trackName, webClipId);
            auto pitch = juce::jlimit(
                0.0,
                127.0,
                static_cast<double>(root->getProperty("pitch")));
            if (notes == nullptr)
                return;
            const auto index = findPitchNoteIndex(*notes, clipId);
            if (! index)
                return;

            auto& note = (*notes)[*index];
            if (trackName != "Voice Main"
                && static_cast<bool>(root->getProperty("snapToChord")))
            {
                pitch = snapPitchToChord(
                    pitch,
                    note.start,
                    document_.chords);
            }

            beginPitchTrackEdit(trackName);
            moveNoteToPitch(note, pitch);
            finishPitchTrackEdit(trackName);

            auto* event = new juce::DynamicObject();
            event->setProperty("type", "clip-pitch-state");
            event->setProperty("track", trackName);
            event->setProperty("clipId", webClipId);
            event->setProperty("pitch", pitch);
            webView_.dispatch(event);
        }
    }

    void resetHistory()
    {
        undoStack_.clear();
        redoStack_.clear();
        pitchTrackHistories_.clear();
        ++leadPitchPreviewRevision_;
        restoreOriginalLeadAudio();
        backingPitchPreviewRevisions_.clear();
        for (const auto& [trackName, previewFile] : backingPitchPreviewFiles_)
        {
            juce::ignoreUnused(trackName);
            previewFile.deleteFile();
        }
        backingPitchPreviewFiles_.clear();
        undoButton_.setEnabled(false);
        redoButton_.setEnabled(false);
    }

    void updatePlaybackNotes()
    {
        auto& state = noteStates_[static_cast<size_t>(writeNoteStateIndex_)];
        state.count = 0;
        const auto addNote = [&state](const NoteBlock& note, int backingTrackIndex)
        {
            if (note.end <= note.start || state.count >= kMaxPlaybackNotes)
                return;

            auto& playbackNote = state.notes[static_cast<size_t>(state.count++)];
            playbackNote.start = note.start;
            playbackNote.end = note.end;
            playbackNote.frequency = midiNoteToFrequency(note.pitchExact);
            playbackNote.gain = static_cast<float>(
                std::pow(10.0, juce::jlimit(-24.0, 12.0, note.gainDb) / 20.0));
            playbackNote.isBacking = backingTrackIndex >= 0;
            playbackNote.backingTrackIndex = backingTrackIndex;
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
        };

        for (const auto& note : document_.notes)
            addNote(note, -1);
        if (! document_.backingTracks.empty())
        {
            for (const auto& track : document_.backingTracks)
            {
                const auto trackIndex = backingStyleIndex(track.styleId, track.styleName);
                for (const auto& note : track.notes)
                    addNote(note, trackIndex);
            }
        }
        else
        {
            const auto trackIndex = backingStyleIndex(
                document_.backingStyleId,
                document_.backingStyleName);
            for (const auto& note : document_.backingNotes)
                addNote(note, trackIndex);
        }
        std::sort(state.notes.begin(),
                  state.notes.begin() + state.count,
                  [](const auto& a, const auto& b) { return a.start < b.start; });

        activeNoteState_.store(&state);
        writeNoteStateIndex_ = 1 - writeNoteStateIndex_;
    }

    void trackPlaybackStateChanged(bool instrumentalMuted,
                                   bool instrumentalSolo,
                                   bool leadMuted,
                                   bool leadSolo,
                                   bool backingMuted,
                                   bool backingSolo) override
    {
        instrumentalMuted_.store(instrumentalMuted);
        instrumentalSolo_.store(instrumentalSolo);
        leadMuted_.store(leadMuted);
        leadSolo_.store(leadSolo);
        backingMuted_.store(backingMuted);
        backingSolo_.store(backingSolo);
        const auto activeIndex = activeBackingTrackIndex_.load(std::memory_order_relaxed);
        if (activeIndex >= 0 && activeIndex < static_cast<int>(kMaxBackingTracks))
        {
            backingTrackMuted_[static_cast<size_t>(activeIndex)].store(
                backingMuted,
                std::memory_order_relaxed);
            backingTrackSoloed_[static_cast<size_t>(activeIndex)].store(
                backingSolo,
                std::memory_order_relaxed);
        }
        sendWebTrackState("Instrumental", instrumentalMuted, instrumentalSolo);
        sendWebTrackState("Voice Main", leadMuted, leadSolo);
        if (document_.backingStyleName.isNotEmpty())
            sendWebTrackState(document_.backingStyleName, backingMuted, backingSolo);
    }

    void playheadPositionRequested(double seconds) override
    {
        setTransportPosition(seconds);
        sendWebTransportState();
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
        activeBackingTrackIndex_.store(
            backingStyleIndex(document_.backingStyleId, document_.backingStyleName),
            std::memory_order_relaxed);
        editor_.clearSelection();
        if (document_.audioFile.existsAsFile())
            editor_.setAudioFile(document_.audioFile);
        if (document_.backingAudioFile.existsAsFile())
        {
            editor_.setBackingAudioFile(document_.backingAudioFile);
            configureBackingAudioTransportForFile(document_.backingAudioFile);
        }
        else
        {
            clearBackingAudioTrack();
        }
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
        refreshWaveformData();
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

    void clearBackingAudioTrack()
    {
        backingAudioTransport_.stop();
        backingAudioTransport_.setSource(nullptr);
        backingAudioReaderSource_.reset();
        backingAudioTransportFile_ = juce::File();
        requestWaveformData(WaveformTrack::backing, {});
        editor_.setBackingAudioFile({});
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

    void configureBackingAudioTransportForFile(const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
        if (reader == nullptr)
        {
            deviceManager.closeAudioDevice();
            backingAudioTransport_.stop();
            backingAudioTransport_.setSource(nullptr);
            backingAudioReaderSource_.reset();
            backingAudioTransportFile_ = juce::File();
            deviceManager.restartLastAudioDevice();
            return;
        }

        const auto sampleRate = reader->sampleRate;
        auto nextReaderSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);

        deviceManager.closeAudioDevice();
        backingAudioTransport_.stop();
        backingAudioTransport_.setSource(nullptr);
        backingAudioReaderSource_ = std::move(nextReaderSource);
        backingAudioTransport_.setSource(backingAudioReaderSource_.get(), 0, nullptr, sampleRate);
        backingAudioTransportFile_ = file;
        deviceManager.restartLastAudioDevice();
    }

    void togglePlayback()
    {
        if (readerSource_ == nullptr && instrumentalReaderSource_ == nullptr)
        {
            setStatus("Open an audio file before playback.");
            return;
        }

        if (transport_.isPlaying() || instrumentalTransport_.isPlaying() || backingAudioTransport_.isPlaying())
        {
            transport_.stop();
            instrumentalTransport_.stop();
            backingAudioTransport_.stop();
            loopAuditionActive_ = false;
            playButton_.setButtonText("Play");
        }
        else
        {
            loopAuditionActive_ = false;
            auto startPosition = readerSource_ != nullptr ? transport_.getCurrentPosition() : instrumentalTransport_.getCurrentPosition();
            if (startPosition >= juce::jmax(0.0, document_.duration - 0.01))
                startPosition = 0.0;

            transport_.setPosition(startPosition);
            instrumentalTransport_.setPosition(startPosition);
            backingAudioTransport_.setPosition(startPosition);

            // Start secondary transports before the playhead master so they are ready
            // by the first block in which the UI/audio position starts advancing.
            if (backingAudioReaderSource_ != nullptr)
                backingAudioTransport_.start();
            if (instrumentalReaderSource_ != nullptr)
                instrumentalTransport_.start();
            if (readerSource_ != nullptr)
                transport_.start();
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
        if (backingAudioTransport_.isPlaying())
            backingAudioTransport_.stop();

        clickedToneFrequency_.store(midiNoteToFrequency(note.pitchExact));
        clickedToneHeld_.store(false, std::memory_order_release);
        clickedToneLeaseSamples_.store(0, std::memory_order_release);
        const auto totalSamples = juce::jmax(1, static_cast<int>(audioSampleRate_.load() * 0.55));
        clickedToneSamplesRemaining_.store(totalSamples);
        clickedToneSerial_.fetch_add(1);
        playButton_.setButtonText("Play");
        setStatus("Auditioning note " + note.id + " at MIDI " + juce::String(note.pitchExact, 2) + ".");
    }

    void previewPitchTone(double midiPitch, bool restart) noexcept
    {
        clickedToneFrequency_.store(
            midiNoteToFrequency(midiPitch),
            std::memory_order_relaxed);
        clickedToneHeld_.store(true, std::memory_order_release);
        clickedToneLeaseSamples_.store(
            juce::jmax(
                1,
                static_cast<int>(
                    audioSampleRate_.load(std::memory_order_relaxed) * 0.45)),
            std::memory_order_release);
        clickedToneSamplesRemaining_.store(1, std::memory_order_relaxed);
        if (restart)
            clickedToneSerial_.fetch_add(1, std::memory_order_release);
    }

    void stopPitchTone() noexcept
    {
        clickedToneLeaseSamples_.store(0, std::memory_order_release);
        clickedToneHeld_.store(false, std::memory_order_release);
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
        backingAudioTransport_.setPosition(loopStartTime_);
        if (backingAudioReaderSource_ != nullptr)
            backingAudioTransport_.start();
        if (instrumentalReaderSource_ != nullptr)
            instrumentalTransport_.start();
        if (readerSource_ != nullptr)
            transport_.start();
        playButton_.setButtonText("Stop");
        setStatus("Looping selected part from " + juce::String(loopStartTime_, 3) + "s to " + juce::String(loopEndTime_, 3) + "s.");
    }

    void timerCallback() override
    {
        const auto transportPosition = currentTransportPosition();
        const auto playhead = juce::jlimit(0.0, juce::jmax(0.0, document_.duration), transportPosition);
        editor_.setPlayheadTime(playhead);

        if (isPlaybackRunning() && (loopAuditionActive_ || cycleLoopActive_))
        {
            if (playhead >= loopEndTime_ - 0.004)
                setTransportPosition(loopStartTime_);
        }
        else if (isPlaybackRunning() && document_.duration > 0.0 && playhead >= document_.duration - 0.005)
        {
            stopPlayback();
        }

        sendWebTransportState();
        sendWebOutputMeterState();
        pollVoiceTrainingJobs();
        maybeAutosave();
    }

    void pollVoiceTrainingJobs()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastVoiceTrainingPollMs_ < 500.0)
            return;
        lastVoiceTrainingPollMs_ = now;

        detachedTrainingProcesses_.erase(
            std::remove_if(
                detachedTrainingProcesses_.begin(),
                detachedTrainingProcesses_.end(),
                [](const auto& process)
                {
                    return process == nullptr || ! process->isRunning();
                }),
            detachedTrainingProcesses_.end());

        const auto activeFile = voiceProfilesRoot().getChildFile("active.json");
        auto activeProfileChanged = false;
        if (activeFile.existsAsFile())
        {
            const auto modificationMs =
                activeFile.getLastModificationTime().toMilliseconds();
            if (modificationMs > lastActiveProfilePointerModificationMs_)
            {
                loadActiveVoiceProfile();
                activeProfileChanged = true;
            }
        }
        if (activeProfileChanged)
            sendWebProjectState();
        sendWebVoiceTrainingState();
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
        if (! document_.instrumentalFile.existsAsFile())
        {
            setStatus("Open instrumental before selecting vocal audio.");
            return;
        }

        chooser_ = std::make_unique<juce::FileChooser>(
            "Open vocal audio",
            juce::File(),
            supportedAudioFileWildcard());
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
        chooser_ = std::make_unique<juce::FileChooser>(
            "Open instrumental audio",
            juce::File(),
            supportedAudioFileWildcard());
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
                                  if (file.existsAsFile() && loadJson(file))
                                  {
                                      projectDirectory_ = isProjectManifest(file)
                                          ? file.getParentDirectory()
                                          : juce::File();
                                  }
                              });
    }

    static bool isProjectManifest(const juce::File& file)
    {
        const auto name = file.getFileName();
        return name.equalsIgnoreCase("project.synthetic-obsidian.json")
            || name.endsWithIgnoreCase(".synthetic-obsidian.json");
    }

    static juce::File findProjectManifest(const juce::File& directory)
    {
        const auto canonicalManifest = directory.getChildFile("project.synthetic-obsidian.json");
        if (canonicalManifest.existsAsFile())
            return canonicalManifest;

        juce::Array<juce::File> candidates;
        directory.findChildFiles(candidates, juce::File::findFiles, false, "*.json");

        for (const auto& candidate : candidates)
            if (isProjectManifest(candidate))
                return candidate;

        for (const auto& candidate : candidates)
            if (! candidate.getFileName().containsIgnoreCase(".autosave."))
                return candidate;

        return {};
    }

    void chooseProject()
    {
        if (projectSaveRunning_)
        {
            setStatus("Wait for the current project save to finish.");
            return;
        }

        const auto initialLocation = projectDirectory_.isDirectory()
            ? projectDirectory_
            : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        chooser_ = std::make_unique<juce::FileChooser>(
            "Open Synthetic Obsidian project",
            initialLocation,
            "*.json");
       #if JUCE_MAC || JUCE_IOS
        // A project owns sibling audio assets. Selecting its directory gives
        // the native picker one user-authorized scope instead of prompting
        // separately as each asset is restored.
        constexpr auto projectChooserFlags =
            juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories;
       #else
        constexpr auto projectChooserFlags =
            juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectDirectories;
       #endif
        chooser_->launchAsync(
            projectChooserFlags,
            [this](const juce::FileChooser& chooser)
            {
                const auto selected = chooser.getResult();
                const auto manifest = selected.isDirectory()
                    ? findProjectManifest(selected)
                    : selected;
                if (! manifest.existsAsFile())
                {
                    if (selected != juce::File())
                        setStatus("The selected folder does not contain a project manifest.");
                    return;
                }

                if (loadJson(manifest))
                {
                    projectDirectory_ = manifest.getParentDirectory();
                    currentJsonFile_ = manifest;
                    setStatus("Opened project: " + projectDirectory_.getFileName() + ".");
                }
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
        resetBackingTrackControls();
        document_.audioFile = file;
        leadAudioMuted_.store(false);
        leadNotesMuted_.store(false);
        document_.instrumentalFile = previousInstrumentalFile;
        document_.tempoSegments = std::move(previousTempoSegments);
        document_.timeSignatures = std::move(previousTimeSignatures);
        document_.chords = std::move(previousChords);
        document_.sampleRate = static_cast<int>(reader->sampleRate);
        document_.duration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        if (previousInstrumentalFile.existsAsFile())
        {
            std::unique_ptr<juce::AudioFormatReader> instrumentalReader(formatManager_.createReaderFor(previousInstrumentalFile));
            if (instrumentalReader != nullptr && instrumentalReader->sampleRate > 0.0)
                document_.duration = juce::jmax(document_.duration,
                                                static_cast<double>(instrumentalReader->lengthInSamples) / instrumentalReader->sampleRate);
        }
        currentJsonFile_ = file.withFileExtension(".annotation.json");
        ++documentRevision_;
        configureTransportForFile(file);
        clearBackingAudioTrack();

        editor_.setAudioFile(file);
        if (document_.instrumentalFile.existsAsFile())
            editor_.setInstrumentalFile(document_.instrumentalFile);
        refreshWaveformData();
        updatePlaybackNotes();
        syncInspector();
        markChanged();
        resetHistory();
        setStatus("Loaded vocal track: " + file.getFileName() + ". Starting analysis...");
        runCombinedAnalysis();
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
        if (reader->sampleRate > 0.0)
        {
            document_.sampleRate = document_.sampleRate > 0 ? document_.sampleRate : static_cast<int>(reader->sampleRate);
            document_.duration = juce::jmax(document_.duration,
                                            static_cast<double>(reader->lengthInSamples) / reader->sampleRate);
        }
        document_.backingNotes.clear();
        document_.backingStyleId.clear();
        document_.backingStyleName.clear();
        document_.backingTracks.clear();
        document_.backingAudioFile = juce::File();
        resetBackingTrackControls();
        clearBackingAudioTrack();
        editor_.setInstrumentalFile(file);
        if (! document_.audioFile.existsAsFile())
            editor_.fitToClip();
        configureInstrumentalTransportForFile(file);
        refreshWaveformData();
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
                const auto first = static_cast<int>(
                    std::max<size_t>(0, pointIndex > 2 ? pointIndex - 2 : 0));
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
                    const auto first = static_cast<int>(
                        std::max<size_t>(0, pointIndex > 2 ? pointIndex - 2 : 0));
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
        {
            document_.notes = std::move(rebuiltNotes);
            document_.backingNotes.clear();
            document_.backingStyleId.clear();
            document_.backingStyleName.clear();
            document_.backingTracks.clear();
            resetBackingTrackControls();
        }
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

        auto jsonText = output.trim();
        const auto jsonStart = jsonText.indexOfChar('{');
        const auto jsonEnd = jsonText.lastIndexOfChar('}');
        if (jsonStart >= 0 && jsonEnd > jsonStart)
            jsonText = jsonText.substring(jsonStart, jsonEnd + 1);

        auto parsed = juce::JSON::parse(jsonText);
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
        const auto& signature = document_.timeSignatures.front();
        setStatus("Detected " + juce::String(static_cast<int>(document_.tempoSegments.size()))
            + " tempo segment"
            + (document_.tempoSegments.size() == 1 ? ", " : "s, ")
            + juce::String(signature.numerator)
            + "/"
            + juce::String(signature.denominator)
            + " signature, "
            + juce::String(static_cast<int>(document_.chords.size()))
            + " chord"
            + (document_.chords.size() == 1 ? "." : "s."));
    }

    void finishBackingVocalGeneration()
    {
        backingGenerationRunning_ = false;
        backingGenerationTrackName_.clear();
        addBackingVocalButton_.setEnabled(true);
    }

    void runBackingVocalGeneration()
    {
        if (backingGenerationRunning_ || backingAudioRenderRunning_)
            return;

        if (document_.notes.empty())
        {
            setStatus("Analyze vocal first: backing generation needs lead melody notes.");
            return;
        }

        if (document_.chords.empty())
        {
            setStatus("Open instrumental first: backing generation needs chord context.");
            return;
        }

        const auto style = backingStyleForComboId(backingStyleBox_.getSelectedId());
        if (! style)
        {
            setStatus("Choose a backing vocal style first.");
            return;
        }

        auto* requestRoot = new juce::DynamicObject();
        requestRoot->setProperty("style_id", style->id);
        requestRoot->setProperty("style_name", style->name);
        requestRoot->setProperty("source_file", document_.audioFile.existsAsFile() ? document_.audioFile.getFullPathName() : "vocal_annotation_tool");
        requestRoot->setProperty("duration", document_.duration);
        requestRoot->setProperty("bpm", document_.bpm);
        requestRoot->setProperty("key", document_.key);

        juce::Array<juce::var> tempos;
        if (document_.tempoSegments.empty())
        {
            auto* tempo = new juce::DynamicObject();
            tempo->setProperty("start", 0.0);
            tempo->setProperty("end", document_.duration);
            tempo->setProperty("bpm", document_.bpm);
            tempos.add(tempo);
        }
        else
        {
            for (const auto& segment : document_.tempoSegments)
            {
                auto* tempo = new juce::DynamicObject();
                tempo->setProperty("start", segment.start);
                tempo->setProperty("end", segment.end);
                tempo->setProperty("bpm", segment.bpm);
                tempos.add(tempo);
            }
        }
        requestRoot->setProperty("tempo_segments", tempos);

        juce::Array<juce::var> signatures;
        if (document_.timeSignatures.empty())
        {
            auto* signature = new juce::DynamicObject();
            signature->setProperty("start", 0.0);
            signature->setProperty("end", document_.duration);
            signature->setProperty("numerator", 4);
            signature->setProperty("denominator", 4);
            signatures.add(signature);
        }
        else
        {
            for (const auto& segment : document_.timeSignatures)
            {
                auto* signature = new juce::DynamicObject();
                signature->setProperty("start", segment.start);
                signature->setProperty("end", segment.end);
                signature->setProperty("numerator", segment.numerator);
                signature->setProperty("denominator", segment.denominator);
                signatures.add(signature);
            }
        }
        requestRoot->setProperty("time_signatures", signatures);

        juce::Array<juce::var> chords;
        for (const auto& segment : document_.chords)
        {
            auto* chord = new juce::DynamicObject();
            chord->setProperty("start", segment.start);
            chord->setProperty("end", segment.end);
            chord->setProperty("name", segment.name);
            chord->setProperty("confidence", segment.confidence);
            chords.add(chord);
        }
        requestRoot->setProperty("chords", chords);

        const auto backingLeadNotes = leadNotesForBackingGeneration(document_.notes);
        juce::Array<juce::var> leadNotes;
        for (const auto& note : backingLeadNotes)
        {
            if (note.end <= note.start)
                continue;

            auto* object = new juce::DynamicObject();
            object->setProperty("id", note.id);
            object->setProperty("start", note.start);
            object->setProperty("end", note.end);
            object->setProperty("pitch", note.pitch);
            object->setProperty("lyric", note.lyric);
            object->setProperty("syllable_id", note.syllableId);
            object->setProperty("legato_from_previous", note.flags.contains("legato_from_previous"));
            object->setProperty("legato_to_next", note.flags.contains("legato_to_next"));
            object->setProperty("melisma_continuation", note.flags.contains("melisma_continuation"));
            leadNotes.add(object);
        }
        requestRoot->setProperty("lead_notes", leadNotes);

        const auto inputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getNonexistentChildFile("synthetic_obsidian_backing_request", ".json");
        const auto requestJson = juce::JSON::toString(juce::var(requestRoot), false);
        if (! inputFile.replaceWithText(requestJson))
        {
            setStatus("Could not write backing vocal request JSON.");
            return;
        }

        if (! startBackingVocalWorker())
        {
            inputFile.deleteFile();
            setStatus("Could not start backing vocal worker.");
            return;
        }

        const auto requestId = "bv_" + juce::String(juce::Time::currentTimeMillis());
        const auto queuedRequest = backingWorkerQueueDir_.getChildFile(requestId + ".request.json");
        if (! inputFile.moveFileTo(queuedRequest))
        {
            inputFile.deleteFile();
            setStatus("Could not queue backing vocal request.");
            return;
        }

        const juce::String styleId(style->id);
        const juce::String styleName(style->name);
        backingGenerationRunning_ = true;
        backingGenerationTrackName_ = styleName;
        clearPitchTrackHistory(styleName);
        ++backingPitchPreviewRevisions_[styleName];
        addBackingVocalButton_.setEnabled(false);
        beginUndoableAction();
        auto* backingTrack = findBackingTrack(styleId, styleName);
        if (backingTrack == nullptr)
        {
            document_.backingTracks.push_back({
                styleId,
                styleName,
                {},
                {},
                {},
                activeVoiceProfileId_
            });
            backingTrack = &document_.backingTracks.back();
        }
        backingTrack->notes.clear();
        backingTrack->renderedNotes.clear();
        backingTrack->audioFile = juce::File();
        makeBackingTrackActive(*backingTrack);
        clearBackingAudioTrack();
        if (const auto preview = backingPitchPreviewFiles_.find(styleName);
            preview != backingPitchPreviewFiles_.end())
        {
            preview->second.deleteFile();
            backingPitchPreviewFiles_.erase(preview);
        }
        markChanged();
        const auto expectedRevision = documentRevision_;
        editor_.repaint();
        auto generationStatus = "Generating backing vocal MIDI: " + styleName
            + " from " + juce::String(leadNotes.size()) + " lead notes";
        const auto collapsedLeadNotes = static_cast<int>(document_.notes.size()) - leadNotes.size();
        if (collapsedLeadNotes > 0)
            generationStatus += " (collapsed " + juce::String(collapsedLeadNotes) + " micro-detune notes)";
        generationStatus += "...";
        setStatus(generationStatus);

        juce::WeakReference<MainComponent> safeThis(this);
        const auto eventsFile = backingWorkerQueueDir_.getChildFile(requestId + ".events.jsonl");
        juce::Thread::launch([safeThis, eventsFile, requestId, expectedRevision, styleId, styleName]
        {
            int processedLines = 0;
            double lastEventTimeMs = juce::Time::getMillisecondCounterHiRes();

            while (true)
            {
                if (eventsFile.existsAsFile())
                {
                    const auto text = eventsFile.loadFileAsString();
                    auto lines = juce::StringArray::fromLines(text);
                    // The worker appends JSONL events. Do not consume a final line while it is
                    // still being written, otherwise a transient parse failure can hide the
                    // terminal event forever.
                    while (! lines.isEmpty() && lines[lines.size() - 1].trim().isEmpty())
                        lines.remove(lines.size() - 1);
                    if (! text.endsWithChar('\n') && ! text.endsWithChar('\r') && ! lines.isEmpty())
                        lines.remove(lines.size() - 1);

                    for (int i = processedLines; i < lines.size(); ++i)
                    {
                        const auto line = lines[i].trim();
                        if (line.isEmpty())
                            continue;

                        lastEventTimeMs = juce::Time::getMillisecondCounterHiRes();
                        const auto parsed = juce::JSON::parse(line);
                        auto* root = parsed.getDynamicObject();
                        if (root == nullptr)
                            continue;

                        const auto type = root->getProperty("type").toString();
                        const auto isTerminal = type == "done" || type == "error";
                        juce::MessageManager::callAsync([safeThis, parsed, expectedRevision, styleId, styleName]
                        {
                            if (safeThis != nullptr)
                                safeThis->applyBackingVocalWorkerEvent(parsed, expectedRevision, styleId, styleName);
                        });

                        if (isTerminal)
                            return;
                    }
                    processedLines = lines.size();
                }

                if (juce::Time::getMillisecondCounterHiRes() - lastEventTimeMs > 15.0 * 60.0 * 1000.0)
                {
                    juce::MessageManager::callAsync([safeThis, expectedRevision, styleId, styleName]
                    {
                        if (safeThis != nullptr)
                            safeThis->applyBackingVocalWorkerTimeout(expectedRevision, styleId, styleName);
                    });
                    return;
                }

                juce::Thread::sleep(80);
            }
        });
    }

    void applyBackingVocalWorkerEvent(const juce::var& event,
                                      unsigned int expectedRevision,
                                      const juce::String& styleId,
                                      const juce::String& styleName)
    {
        auto* root = event.getDynamicObject();
        if (root == nullptr)
            return;

        const auto type = root->getProperty("type").toString();
        if (type == "started")
        {
            setStatus("Backing vocal model is generating: " + styleName + "...");
            return;
        }

        if (type == "error")
        {
            finishBackingVocalGeneration();
            setStatus("Backing vocal generation failed: " + root->getProperty("message").toString());
            return;
        }

        if (type != "window" && type != "done")
            return;

        juce::var payload = event;
        auto* payloadRoot = root;
        if (type == "done")
        {
            const auto resultPath = root->getProperty("result_path").toString();
            if (resultPath.isNotEmpty())
            {
                const auto resultFile = juce::File(resultPath);
                if (resultFile.existsAsFile())
                {
                    payload = juce::JSON::parse(resultFile);
                    if (auto* parsedRoot = payload.getDynamicObject())
                        payloadRoot = parsedRoot;
                }
            }
        }

        if (expectedRevision != documentRevision_)
        {
            if (type == "done")
            {
                finishBackingVocalGeneration();
                setStatus("Discarded backing vocal result because the source annotation changed during generation.");
            }
            return;
        }

        juce::String parseError;
        auto generatedNotes = parseBackingNotes(*payloadRoot, parseError);
        if (generatedNotes.empty())
        {
            if (type == "done")
            {
                finishBackingVocalGeneration();
                setStatus(parseError.isNotEmpty() ? parseError : "Backing vocal model returned no notes.");
            }
            return;
        }

        auto* backingTrack = findBackingTrack(styleId, styleName);
        if (backingTrack == nullptr)
        {
            finishBackingVocalGeneration();
            setStatus("Discarded backing vocal result because its target track no longer exists.");
            return;
        }

        backingTrack->notes = std::move(generatedNotes);
        backingTrack->renderedNotes.clear();
        backingTrack->audioFile = juce::File();
        makeBackingTrackActive(*backingTrack);
        backingAudioMuted_.store(false);
        backingNotesMuted_.store(false);
        const auto generatedTrackIndex = backingStyleIndex(styleId, styleName);
        if (generatedTrackIndex >= 0
            && generatedTrackIndex < static_cast<int>(kMaxBackingTracks))
        {
            backingTrackAudioMuted_[static_cast<size_t>(generatedTrackIndex)].store(
                false,
                std::memory_order_relaxed);
            backingTrackNotesMuted_[static_cast<size_t>(generatedTrackIndex)].store(
                false,
                std::memory_order_relaxed);
        }
        updatePlaybackNotes();
        updateInfoText();
        editor_.repaint();

        const auto windowIndex = static_cast<int>(root->getProperty("window_index"));
        const auto windowCount = static_cast<int>(root->getProperty("window_count"));
        if (type == "window")
        {
            sendWebProjectState();
            setStatus("Generated backing vocal window "
                + juce::String(windowIndex)
                + "/"
                + juce::String(windowCount)
                + " ("
                + juce::String(static_cast<int>(document_.backingNotes.size()))
                + " notes): "
                + styleName
                + ".");
            return;
        }

        finishBackingVocalGeneration();
        markChanged();

        auto status = "Generated " + juce::String(static_cast<int>(document_.backingNotes.size()))
            + " backing vocal MIDI notes: " + styleName + ".";
        const auto debugDslPath = payloadRoot->getProperty("debug_dsl_path").toString();
        if (debugDslPath.isNotEmpty())
            status += " Debug DSL: " + debugDslPath;
        setStatus(status);
    }

    std::vector<NoteBlock> parseBackingNotes(const juce::DynamicObject& root, juce::String& error) const
    {
        auto* notes = root.getProperty("notes").getArray();
        if (notes == nullptr || notes->isEmpty())
        {
            error = "Backing vocal model returned no notes.";
            return {};
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
        std::vector<NoteBlock> generatedNotes;
        generatedNotes.reserve(static_cast<size_t>(notes->size()));
        for (const auto& value : *notes)
        {
            auto* object = value.getDynamicObject();
            if (object == nullptr)
                continue;

            NoteBlock note;
            note.id = stringProperty(*object, "id", "bv_" + juce::String(static_cast<int>(generatedNotes.size()) + 1).paddedLeft('0', 3));
            note.start = numberProperty(*object, "start", 0.0);
            note.end = numberProperty(*object, "end", note.start + 0.25);
            note.pitch = juce::jlimit(0, 127, static_cast<int>(numberProperty(*object, "midi_note", 60.0)));
            note.pitchExact = static_cast<double>(note.pitch);
            note.voicedStart = note.start;
            note.voicedEnd = note.end;
            note.lyric = stringProperty(*object, "strategy", "BV");
            note.syllableId = stringProperty(*object, "syllable_id");
            note.flags.add("backing_vocal");
            if (static_cast<bool>(object->getProperty("legato_from_previous")))
                note.flags.add("legato_from_previous");
            if (static_cast<bool>(object->getProperty("legato_to_next")))
                note.flags.add("legato_to_next");
            if (static_cast<bool>(object->getProperty("melisma_continuation")))
                note.flags.add("melisma_continuation");
            note.curve.push_back({ note.start, note.pitchExact, 1.0 });
            note.curve.push_back({ note.end, note.pitchExact, 1.0 });

            if (note.end > note.start)
                generatedNotes.push_back(std::move(note));
        }

        if (generatedNotes.empty())
            error = "Backing vocal model returned only invalid notes.";

        return generatedNotes;
    }

    void applyBackingVocalWorkerTimeout(unsigned int expectedRevision,
                                        const juce::String&,
                                        const juce::String& styleName)
    {
        if (expectedRevision != documentRevision_)
            return;

        finishBackingVocalGeneration();
        setStatus("Backing vocal worker timed out while generating: " + styleName + ".");
    }

    bool startBackingVocalWorker()
    {
        if (backingWorkerProcess_ != nullptr && backingWorkerProcess_->isRunning())
            return true;

        const auto root = juce::File(SYNTHETIC_OBSIDIAN_ROOT);
        const auto script = root.getChildFile("tools")
                                .getChildFile("vocal_annotation_tool")
                                .getChildFile("backing_vocal_worker.py");
        if (! script.existsAsFile())
        {
            setStatus("Backing vocal worker script not found: " + script.getFullPathName());
            return false;
        }

        if (backingWorkerQueueDir_ == juce::File())
        {
            backingWorkerQueueDir_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                         .getChildFile("synthetic_obsidian_bv_worker_"
                                             + juce::String(juce::Time::currentTimeMillis()));
        }
        backingWorkerQueueDir_.createDirectory();

        backingWorkerProcess_ = std::make_unique<juce::ChildProcess>();
        juce::StringArray args;
        args.add(backingModelPythonExecutable());
        args.add(script.getFullPathName());
        args.add("--queue-dir");
        args.add(backingWorkerQueueDir_.getFullPathName());

        if (! backingWorkerProcess_->start(args))
        {
            backingWorkerProcess_.reset();
            return false;
        }

        setStatus("Loading backing vocal model worker...");
        return true;
    }

    void stopBackingVocalWorker()
    {
        if (backingWorkerProcess_ != nullptr)
        {
            if (backingWorkerProcess_->isRunning())
                backingWorkerProcess_->kill();
            backingWorkerProcess_.reset();
        }
    }

    static juce::String shellQuote(juce::String text)
    {
        text = text.replace("'", "'\\''");
        return "'" + text + "'";
    }

    void openBackingDebugDslPlayer()
    {
        const auto playerDir = juce::File(SYNTHETIC_OBSIDIAN_ROOT)
                                   .getChildFile("tools")
                                   .getChildFile("dsl_player");
        const auto shellCommand = "cd " + shellQuote(playerDir.getFullPathName())
            + " && (lsof -iTCP:8765 -sTCP:LISTEN >/dev/null 2>&1"
            + " || nohup python3 -m http.server 8765 --bind 127.0.0.1 >/tmp/synthetic_obsidian_dsl_player.log 2>&1 &)";
        const auto command = "/bin/zsh -lc " + shellQuote(shellCommand);
        std::system(command.toRawUTF8());
        juce::URL("http://127.0.0.1:8765/?file=generated/latest_backing_debug.yaml").launchInDefaultBrowser();
    }

    bool startBackingAudioWorker()
    {
        if (backingAudioWorkerProcess_ != nullptr && backingAudioWorkerProcess_->isRunning())
            return true;

        const auto script = juce::File(SYNTHETIC_OBSIDIAN_ROOT)
                                .getChildFile("tools")
                                .getChildFile("vocal_annotation_tool")
                                .getChildFile("backing_audio_worker.py");
        if (! script.existsAsFile())
        {
            setStatus("SoulX-Singer worker script not found: " + script.getFullPathName());
            return false;
        }

        if (backingAudioWorkerQueueDir_ == juce::File())
        {
            backingAudioWorkerQueueDir_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                              .getChildFile("synthetic_obsidian_soulx_worker_"
                                                  + juce::String(juce::Time::currentTimeMillis()));
        }
        backingAudioWorkerQueueDir_.createDirectory();
        backingAudioWorkerQueueDir_.getChildFile("worker_status.json").deleteFile();

        backingAudioWorkerProcess_ = std::make_unique<juce::ChildProcess>();
        juce::StringArray args;
        args.add(soulXPythonExecutable());
        args.add(script.getFullPathName());
        args.add("--queue-dir");
        args.add(backingAudioWorkerQueueDir_.getFullPathName());

        if (! backingAudioWorkerProcess_->start(args))
        {
            backingAudioWorkerProcess_.reset();
            return false;
        }

        setStatus("Loading SoulX-Singer-SVC model...");
        return true;
    }

    void stopBackingAudioWorker()
    {
        if (backingAudioWorkerProcess_ != nullptr)
        {
            if (backingAudioWorkerProcess_->isRunning())
                backingAudioWorkerProcess_->kill();
            backingAudioWorkerProcess_.reset();
        }
    }

    void finishBackingAudioRender()
    {
        backingAudioRenderRunning_ = false;
        backingAudioRenderTrackName_.clear();
        renderBackingAudioButton_.setEnabled(true);
    }

    void runBackingAudioRender()
    {
        if (backingAudioRenderRunning_ || backingGenerationRunning_)
            return;

        if (! document_.audioFile.existsAsFile())
        {
            setStatus("Open main vocal audio before rendering backing vocal audio.");
            return;
        }

        if (document_.backingNotes.empty())
        {
            setStatus("Generate backing vocal MIDI first.");
            return;
        }

        const auto profiles = readVoiceProfileOptions();
        const auto* activeTrack =
            findBackingTrack({}, document_.backingStyleName);
        const auto requestedProfileId =
            activeTrack != nullptr
                ? activeTrack->voiceProfileId
                : juce::String();
        const auto findProfile =
            [&profiles](const juce::String& profileId)
                -> const VoiceProfileOption*
        {
            const auto profile = std::find_if(
                profiles.begin(),
                profiles.end(),
                [&profileId](const auto& candidate)
                {
                    return candidate.id == profileId;
                });
            return profile != profiles.end() ? &*profile : nullptr;
        };
        const auto* renderProfile = findProfile(requestedProfileId);
        if (renderProfile == nullptr)
            renderProfile = findProfile(activeVoiceProfileId_);
        if (renderProfile == nullptr && ! profiles.empty())
            renderProfile = &profiles.front();

        const auto& promptSources =
            renderProfile != nullptr
                ? renderProfile->sources
                : activeVoiceProfileSources_;
        const auto renderQuality =
            renderProfile != nullptr
                ? renderProfile->quality
                : activeVoiceProfileQuality_;
        const auto renderAdapter =
            renderProfile != nullptr
                ? renderProfile->adapterFile
                : activeVoiceAdapterFile_;

        auto* root = new juce::DynamicObject();
        root->setProperty("audio", document_.audioFile.getFullPathName());
        if (! promptSources.empty())
        {
            juce::Array<juce::var> promptCandidates;
            const auto maximumCandidates =
                renderQuality == "high" ? 8 : 4;
            for (const auto& source : promptSources)
            {
                if (promptCandidates.size() >= maximumCandidates)
                    break;
                if (source.existsAsFile())
                    promptCandidates.add(source.getFullPathName());
            }
            if (! promptCandidates.isEmpty())
                root->setProperty("prompt_audio_candidates", promptCandidates);
        }
        if (renderAdapter.existsAsFile())
            root->setProperty(
                "voice_adapter",
                renderAdapter.getFullPathName());
        const auto outputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getNonexistentChildFile("synthetic_obsidian_backing_audio", ".wav");
        root->setProperty("output", outputFile.getFullPathName());
        root->setProperty("duration", document_.duration);

        juce::Array<juce::var> backingNotes;
        for (const auto& note : document_.backingNotes)
        {
            if (note.end <= note.start)
                continue;

            auto* object = new juce::DynamicObject();
            object->setProperty("id", note.id);
            object->setProperty("start", note.start);
            object->setProperty("end", note.end);
            object->setProperty("voiced_start", note.voicedStart);
            object->setProperty("voiced_end", note.voicedEnd);
            object->setProperty("pitch", note.pitch);
            object->setProperty("pitch_exact", note.pitchExact);
            object->setProperty("syllable_id", note.syllableId);
            object->setProperty("legato_from_previous", note.flags.contains("legato_from_previous"));
            object->setProperty("legato_to_next", note.flags.contains("legato_to_next"));
            object->setProperty("melisma_continuation", note.flags.contains("melisma_continuation"));

            juce::Array<juce::var> curve;
            for (const auto& point : note.curve)
            {
                auto* curvePoint = new juce::DynamicObject();
                curvePoint->setProperty("time", point.time);
                curvePoint->setProperty("midi", point.midi);
                curvePoint->setProperty("confidence", point.confidence);
                curve.add(curvePoint);
            }
            object->setProperty("curve", curve);
            backingNotes.add(object);
        }
        root->setProperty("backing_notes", backingNotes);

        const auto requestFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                     .getNonexistentChildFile("synthetic_obsidian_backing_audio_request", ".json");
        if (! requestFile.replaceWithText(juce::JSON::toString(juce::var(root), false)))
        {
            setStatus("Could not write backing audio request JSON.");
            return;
        }

        if (! startBackingAudioWorker())
        {
            requestFile.deleteFile();
            setStatus("Could not start SoulX-Singer-SVC worker.");
            return;
        }

        const auto requestId = "soulx_" + juce::String(juce::Time::currentTimeMillis());
        const auto queuedRequest = backingAudioWorkerQueueDir_.getChildFile(requestId + ".request.json");
        if (! requestFile.moveFileTo(queuedRequest))
        {
            requestFile.deleteFile();
            setStatus("Could not queue SoulX-Singer-SVC render request.");
            return;
        }

        backingAudioRenderRunning_ = true;
        backingAudioRenderTrackName_ = document_.backingStyleName;
        backingAudioRenderSourceNotes_ = document_.backingNotes;
        renderBackingAudioButton_.setEnabled(false);
        setStatus("Rendering backing vocal audio with SoulX-Singer-SVC: "
            + backingAudioRenderTrackName_
            + (renderProfile != nullptr
                ? " / voice " + renderProfile->name
                : juce::String())
            + "...");

        juce::WeakReference<MainComponent> safeThis(this);
        const auto expectedRevision = documentRevision_;
        const auto eventsFile = backingAudioWorkerQueueDir_.getChildFile(requestId + ".events.jsonl");
        const auto statusFile = backingAudioWorkerQueueDir_.getChildFile("worker_status.json");
        juce::Thread::launch([safeThis, eventsFile, statusFile, outputFile, expectedRevision]
        {
            int processedLines = 0;
            double lastEventTimeMs = juce::Time::getMillisecondCounterHiRes();
            while (true)
            {
                if (eventsFile.existsAsFile())
                {
                    const auto text = eventsFile.loadFileAsString();
                    auto lines = juce::StringArray::fromLines(text);
                    // The worker appends JSONL events. Do not consume a final line while it is
                    // still being written, otherwise a transient parse failure can hide the
                    // terminal event forever.
                    while (! lines.isEmpty() && lines[lines.size() - 1].trim().isEmpty())
                        lines.remove(lines.size() - 1);
                    if (! text.endsWithChar('\n') && ! text.endsWithChar('\r') && ! lines.isEmpty())
                        lines.remove(lines.size() - 1);

                    for (int i = processedLines; i < lines.size(); ++i)
                    {
                        const auto line = lines[i].trim();
                        if (line.isEmpty())
                            continue;

                        lastEventTimeMs = juce::Time::getMillisecondCounterHiRes();
                        const auto parsed = juce::JSON::parse(line);
                        auto* eventRoot = parsed.getDynamicObject();
                        if (eventRoot == nullptr)
                            continue;

                        const auto type = eventRoot->getProperty("type").toString();
                        if (type == "started")
                        {
                            juce::MessageManager::callAsync([safeThis]
                            {
                                if (safeThis != nullptr)
                                    safeThis->setStatus("SoulX-Singer-SVC is rendering backing vocal audio...");
                            });
                            continue;
                        }

                        if (type == "done")
                        {
                            const auto output = juce::JSON::toString(parsed, false);
                            juce::MessageManager::callAsync([safeThis, output, outputFile, expectedRevision]
                            {
                                if (safeThis != nullptr)
                                    safeThis->applyBackingAudioRender(output, {}, outputFile, expectedRevision);
                            });
                            return;
                        }

                        if (type == "error")
                        {
                            const auto error = "SoulX-Singer-SVC render failed: "
                                + eventRoot->getProperty("message").toString();
                            juce::MessageManager::callAsync([safeThis, error, outputFile, expectedRevision]
                            {
                                if (safeThis != nullptr)
                                    safeThis->applyBackingAudioRender({}, error, outputFile, expectedRevision);
                            });
                            return;
                        }
                    }
                    processedLines = lines.size();
                }

                if (statusFile.existsAsFile())
                {
                    const auto status = juce::JSON::parse(statusFile.loadFileAsString());
                    if (auto* statusRoot = status.getDynamicObject())
                    {
                        if (statusRoot->getProperty("type").toString() == "error")
                        {
                            const auto error = "Could not load SoulX-Singer-SVC: "
                                + statusRoot->getProperty("message").toString();
                            juce::MessageManager::callAsync([safeThis, error, outputFile, expectedRevision]
                            {
                                if (safeThis != nullptr)
                                    safeThis->applyBackingAudioRender({}, error, outputFile, expectedRevision);
                            });
                            return;
                        }
                    }
                }

                if (juce::Time::getMillisecondCounterHiRes() - lastEventTimeMs > 30.0 * 60.0 * 1000.0)
                {
                    const juce::String error("SoulX-Singer-SVC render timed out.");
                    juce::MessageManager::callAsync([safeThis, error, outputFile, expectedRevision]
                    {
                        if (safeThis != nullptr)
                            safeThis->applyBackingAudioRender({}, error, outputFile, expectedRevision);
                    });
                    return;
                }
                juce::Thread::sleep(80);
            }
        });
    }

    void applyBackingAudioRender(const juce::String& output,
                                 const juce::String& error,
                                 const juce::File& outputFile,
                                 unsigned int expectedRevision)
    {
        const auto renderedTrackName = backingAudioRenderTrackName_;
        auto renderedNotes = std::move(backingAudioRenderSourceNotes_);
        backingAudioRenderSourceNotes_.clear();
        finishBackingAudioRender();

        if (error.isNotEmpty())
        {
            setStatus(error);
            return;
        }

        if (! outputFile.existsAsFile())
        {
            setStatus("Backing audio render did not produce a WAV file.");
            return;
        }

        const auto sourceChangedDuringRender = expectedRevision != documentRevision_;
        if (sourceChangedDuringRender)
            setStatus("Applied rendered backing audio to the current document; source annotation changed during render.");

        auto* backingTrack = findBackingTrack({}, renderedTrackName);
        if (backingTrack == nullptr)
        {
            setStatus("Discarded rendered backing audio because its target track no longer exists.");
            return;
        }

        beginUndoableAction();
        const auto previousPreview = backingPitchPreviewFiles_.find(renderedTrackName);
        const auto previousPreviewFile =
            previousPreview != backingPitchPreviewFiles_.end()
            ? previousPreview->second
            : juce::File();
        if (previousPreview != backingPitchPreviewFiles_.end())
            backingPitchPreviewFiles_.erase(previousPreview);
        ++backingPitchPreviewRevisions_[renderedTrackName];
        backingTrack->audioFile = outputFile;
        backingTrack->renderedNotes = renderedNotes.empty()
            ? backingTrack->notes
            : std::move(renderedNotes);
        if (sourceChangedDuringRender)
        {
            auto& history = pitchTrackHistories_[renderedTrackName];
            history.undo.clear();
            history.redo.clear();
            history.undo.push_back({ backingTrack->renderedNotes, {} });
        }
        else
        {
            clearPitchTrackHistory(renderedTrackName);
        }
        makeBackingTrackActive(*backingTrack);
        backingAudioMuted_.store(false);
        const auto renderedTrackIndex = activeBackingTrackIndex_.load(std::memory_order_relaxed);
        if (renderedTrackIndex >= 0
            && renderedTrackIndex < static_cast<int>(kMaxBackingTracks))
        {
            backingTrackAudioMuted_[static_cast<size_t>(renderedTrackIndex)].store(
                false,
                std::memory_order_relaxed);
        }
        editor_.setBackingAudioFile(outputFile);
        configureBackingAudioTransportForFile(outputFile);
        if (previousPreviewFile.existsAsFile())
            previousPreviewFile.deleteFile();
        requestWaveformData(WaveformTrack::backing, outputFile);
        markChanged();
        if (sourceChangedDuringRender)
            scheduleBackingPitchPreview(renderedTrackName);
        updateInfoText();

        auto jsonText = output.trim();
        const auto jsonStart = jsonText.indexOfChar('{');
        const auto jsonEnd = jsonText.lastIndexOfChar('}');
        if (jsonStart >= 0 && jsonEnd > jsonStart)
            jsonText = jsonText.substring(jsonStart, jsonEnd + 1);

        auto status = "Rendered backing vocal audio for "
            + renderedTrackName
            + ": "
            + outputFile.getFileName();
        if (auto parsed = juce::JSON::parse(jsonText); parsed.isObject())
            if (auto* root = parsed.getDynamicObject())
                status << " (" << root->getProperty("engine").toString() << ").";
        setStatus(status);
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
                                        .getChildFile("multilingual_mps_full_strict_pause_silence_head_thr090_tcn.pt");
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
            document_.backingNotes.clear();
            document_.backingStyleId.clear();
            document_.backingStyleName.clear();
            document_.backingTracks.clear();
            rebuildPyinNotesAroundBoundaries(pyinNotes);
            if (document_.notes.empty())
                document_.notes = std::move(pyinNotes);
            collapsedNoteGaps = collapseShortNoteGaps(document_.notes, document_.boundaries);
        }
        else
        {
            document_.notes = std::move(pyinNotes);
            document_.backingNotes.clear();
            document_.backingStyleId.clear();
            document_.backingStyleName.clear();
            document_.backingTracks.clear();
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
        resetBackingTrackControls();

        for (const auto& note : document_.notes)
        {
            document_.regions.push_back({ note.start, juce::jmin(note.start + 0.12, note.end), "slide_in", note.id });
            document_.regions.push_back({ juce::jmax(note.start, note.end - 0.12), note.end, "slide_out", note.id });
        }

        std::sort(document_.boundaries.begin(), document_.boundaries.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        std::sort(document_.notes.begin(), document_.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });

        leadNotesMuted_.store(false);
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

    bool loadJson(const juce::File& file)
    {
        const auto result = AnnotationJson::load(file, document_);
        if (result.failed())
        {
            setStatus(resultMessage("Load JSON", result));
            return false;
        }

        if (activeVoiceAdapterFile_.existsAsFile()
            && activeVoiceProfileId_.isNotEmpty())
        {
            for (auto& track : document_.backingTracks)
                if (track.voiceProfileId.isEmpty())
                    track.voiceProfileId = activeVoiceProfileId_;
        }

        ++documentRevision_;
        currentJsonFile_ = file;
        dirty_ = false;
        leadAudioMuted_.store(false);
        leadNotesMuted_.store(false);
        backingAudioMuted_.store(false);
        backingNotesMuted_.store(false);
        resetBackingTrackControls();
        activeBackingTrackIndex_.store(
            backingStyleIndex(document_.backingStyleId, document_.backingStyleName),
            std::memory_order_relaxed);
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
        if (document_.backingAudioFile.existsAsFile())
        {
            editor_.setBackingAudioFile(document_.backingAudioFile);
            configureBackingAudioTransportForFile(document_.backingAudioFile);
        }
        else
        {
            clearBackingAudioTrack();
        }
        refreshWaveformData();
        updatePlaybackNotes();
        syncInspector();
        resetHistory();
        updateValidationStatus();
        return true;
    }

    static juce::Result copyProjectAsset(
        const juce::File& source,
        const juce::File& directory,
        const juce::String& stem,
        juce::File& copiedFile)
    {
        copiedFile = juce::File();
        if (! source.existsAsFile())
            return juce::Result::ok();

        if (const auto result = directory.createDirectory(); result.failed())
            return result;

        auto extension = source.getFileExtension();
        if (extension.isEmpty())
            extension = ".wav";
        const auto legalStem = juce::File::createLegalFileName(
            stem.isNotEmpty() ? stem : "audio");
        const auto target = directory.getChildFile(legalStem + extension);
        if (source == target)
        {
            copiedFile = target;
            return juce::Result::ok();
        }

        if (target.existsAsFile() && ! target.deleteFile())
            return juce::Result::fail("Could not replace " + target.getFileName() + ".");
        if (! source.copyFileTo(target))
            return juce::Result::fail("Could not copy " + source.getFileName() + ".");

        copiedFile = target;
        return juce::Result::ok();
    }

    static juce::Result consolidateProject(
        const AnnotationDocument& source,
        const juce::File& projectDirectory,
        AnnotationDocument& consolidated)
    {
        if (projectDirectory.existsAsFile())
            return juce::Result::fail("The project path points to a file.");
        if (const auto result = projectDirectory.createDirectory(); result.failed())
            return result;

        const auto audioDirectory = projectDirectory.getChildFile("artifacts").getChildFile("audio");
        const auto backingDirectory = audioDirectory.getChildFile("backing");
        consolidated = source;
        consolidated.audioFile = juce::File();
        consolidated.instrumentalFile = juce::File();
        consolidated.backingAudioFile = juce::File();

        if (const auto result = copyProjectAsset(
                source.audioFile,
                audioDirectory,
                "voice_main",
                consolidated.audioFile);
            result.failed())
        {
            return result;
        }
        if (const auto result = copyProjectAsset(
                source.instrumentalFile,
                audioDirectory,
                "instrumental",
                consolidated.instrumentalFile);
            result.failed())
        {
            return result;
        }

        for (size_t index = 0; index < source.backingTracks.size(); ++index)
        {
            const auto& sourceTrack = source.backingTracks[index];
            auto& consolidatedTrack = consolidated.backingTracks[index];
            const auto trackName = sourceTrack.styleName.isNotEmpty()
                ? sourceTrack.styleName
                : (sourceTrack.styleId.isNotEmpty() ? sourceTrack.styleId : "backing_vocal");
            const auto stem = "backing_"
                + juce::String(static_cast<int>(index) + 1).paddedLeft('0', 2)
                + "_"
                + trackName;

            if (const auto result = copyProjectAsset(
                    sourceTrack.audioFile,
                    backingDirectory,
                    stem,
                    consolidatedTrack.audioFile);
                result.failed())
            {
                return result;
            }

            if (source.backingAudioFile.existsAsFile()
                && source.backingAudioFile == sourceTrack.audioFile)
            {
                consolidated.backingAudioFile = consolidatedTrack.audioFile;
            }
        }

        if (source.backingAudioFile.existsAsFile()
            && ! consolidated.backingAudioFile.existsAsFile())
        {
            if (const auto result = copyProjectAsset(
                    source.backingAudioFile,
                    backingDirectory,
                    "active_backing_vocal",
                    consolidated.backingAudioFile);
                result.failed())
            {
                return result;
            }
        }

        return AnnotationJson::save(
            consolidated,
            projectDirectory.getChildFile("project.synthetic-obsidian.json"));
    }

    bool projectHasBackgroundWork() const
    {
        return analysisRunning_
            || aiPartsAnalysisRunning_
            || musicalContextAnalysisRunning_
            || backingGenerationRunning_
            || backingAudioRenderRunning_
            || lyricsAlignmentRunning_
            || noteRecalculationRunning_;
    }

    void beginProjectSave(const juce::File& directory)
    {
        if (projectSaveRunning_)
            return;

        projectSaveRunning_ = true;
        setStatus("Consolidating project files...");
        const auto snapshot = document_;
        const auto expectedRevision = documentRevision_;
        juce::WeakReference<MainComponent> safeThis(this);

        juce::Thread::launch([safeThis, snapshot, expectedRevision, directory]
        {
            auto consolidated = std::make_shared<AnnotationDocument>();
            const auto result = consolidateProject(snapshot, directory, *consolidated);
            juce::MessageManager::callAsync(
                [safeThis, consolidated, expectedRevision, directory, result]
                {
                    if (safeThis == nullptr)
                        return;

                    safeThis->projectSaveRunning_ = false;
                    if (result.failed())
                    {
                        safeThis->setStatus(resultMessage("Save project", result));
                        return;
                    }

                    safeThis->projectDirectory_ = directory;
                    safeThis->currentJsonFile_ =
                        directory.getChildFile("project.synthetic-obsidian.json");
                    if (safeThis->documentRevision_ == expectedRevision)
                    {
                        safeThis->document_ = *consolidated;
                        safeThis->dirty_ = false;
                        safeThis->refreshWaveformData();
                        safeThis->updateInfoText();
                        safeThis->setStatus(
                            "Saved consolidated project to "
                            + directory.getFullPathName()
                            + ".");
                    }
                    else
                    {
                        safeThis->dirty_ = true;
                        safeThis->updateInfoText();
                        safeThis->setStatus(
                            "Saved a project snapshot, but the project changed during save. Save again to include the latest changes.");
                    }
                });
        });
    }

    void chooseProjectSaveLocation()
    {
        auto projectName = document_.audioFile.existsAsFile()
            ? document_.audioFile.getFileNameWithoutExtension()
            : document_.instrumentalFile.getFileNameWithoutExtension();
        if (projectName.isEmpty())
            projectName = "Synthetic Obsidian Project";
        projectName = juce::File::createLegalFileName(projectName);

        const auto initialLocation =
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(projectName);
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save Synthetic Obsidian project",
            initialLocation,
            juce::String());
        chooser_->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& chooser)
            {
                auto directory = chooser.getResult();
                if (directory == juce::File())
                    return;
                if (directory.hasFileExtension(".json"))
                {
                    directory = directory.getParentDirectory().getChildFile(
                        directory.getFileNameWithoutExtension());
                }
                if (directory.existsAsFile())
                {
                    setStatus("Choose a folder name rather than an existing file.");
                    return;
                }

                beginProjectSave(directory);
            });
    }

    void saveProject()
    {
        if (projectSaveRunning_)
        {
            setStatus("Project save is already running.");
            return;
        }
        if (projectHasBackgroundWork())
        {
            setStatus("Wait for analysis, generation, or rendering to finish before saving the project.");
            return;
        }

        if (projectDirectory_.isDirectory())
            beginProjectSave(projectDirectory_);
        else
            chooseProjectSaveLocation();
    }

    juce::File exportDirectory() const
    {
        if (projectDirectory_.isDirectory())
            return projectDirectory_.getChildFile("exports");

        auto baseFile = currentJsonFile_ != juce::File()
            ? currentJsonFile_
            : document_.audioFile;
        if (baseFile == juce::File())
        {
            baseFile = juce::File::getSpecialLocation(
                juce::File::userDocumentsDirectory).getChildFile("synthetic_obsidian");
        }

        auto projectName = baseFile.getFileNameWithoutExtension();
        if (projectName.endsWithIgnoreCase(".annotation"))
            projectName = projectName.dropLastCharacters(
                juce::String(".annotation").length());
        if (projectName.isEmpty())
            projectName = "synthetic_obsidian";

        return baseFile.getParentDirectory().getChildFile(
            juce::File::createLegalFileName(projectName + "_export"));
    }

    juce::Result exportMidiFilesTo(const juce::File& directory, int& exportedFileCount) const
    {
        const auto exportDocument = [&directory, &exportedFileCount](
                                        const AnnotationDocument& document,
                                        const juce::String& fileName)
        {
            const auto result = AnnotationJson::exportMidi(
                document,
                directory.getChildFile(
                    juce::File::createLegalFileName(fileName) + ".mid"));
            if (result.wasOk())
                ++exportedFileCount;
            return result;
        };

        if (! document_.notes.empty())
        {
            auto leadDocument = document_;
            leadDocument.backingNotes.clear();
            if (const auto result = exportDocument(
                    leadDocument,
                    "voice_main");
                result.failed())
            {
                return result;
            }
        }

        if (! document_.backingTracks.empty())
        {
            for (const auto& track : document_.backingTracks)
            {
                if (track.notes.empty())
                    continue;

                auto backingDocument = document_;
                backingDocument.notes.clear();
                backingDocument.backingNotes = track.notes;
                if (const auto result = exportDocument(
                        backingDocument,
                        track.styleName.isNotEmpty() ? track.styleName : "backing_vocal");
                    result.failed())
                {
                    return result;
                }
            }
        }
        else if (! document_.backingNotes.empty())
        {
            auto backingDocument = document_;
            backingDocument.notes.clear();
            if (const auto result = exportDocument(
                    backingDocument,
                    document_.backingStyleName.isNotEmpty()
                        ? document_.backingStyleName
                        : "backing_vocal");
                result.failed())
            {
                return result;
            }
        }

        return exportedFileCount > 0
            ? juce::Result::ok()
            : juce::Result::fail("There are no MIDI tracks to export.");
    }

    void exportMidi()
    {
        const auto directory = exportDirectory();
        if (const auto result = directory.createDirectory(); result.failed())
        {
            setStatus(resultMessage("Export MIDI", result));
            return;
        }

        int exportedFileCount = 0;
        const auto result = exportMidiFilesTo(directory, exportedFileCount);
        if (result.failed())
        {
            setStatus(resultMessage("Export MIDI", result));
            return;
        }

        completeExport("Exported "
            + juce::String(exportedFileCount)
            + " MIDI file"
            + (exportedFileCount == 1 ? "" : "s"),
            directory);
    }

    void exportAllTracks()
    {
        const auto directory = exportDirectory();
        if (const auto result = directory.createDirectory(); result.failed())
        {
            setStatus(resultMessage("Export all tracks", result));
            return;
        }

        int exportedAudioCount = 0;
        const auto copyAudioTrack =
            [&directory, &exportedAudioCount](
                const juce::File& source,
                const juce::String& stemName) -> juce::Result
        {
            if (! source.existsAsFile())
                return juce::Result::ok();

            const auto target = directory.getChildFile(
                juce::File::createLegalFileName(stemName)
                + source.getFileExtension());
            if (target.existsAsFile() && ! target.deleteFile())
                return juce::Result::fail("Could not replace " + target.getFileName() + ".");
            if (! source.copyFileTo(target))
                return juce::Result::fail("Could not export " + source.getFileName() + ".");

            ++exportedAudioCount;
            return juce::Result::ok();
        };

        if (const auto result = copyAudioTrack(document_.instrumentalFile, "instrumental");
            result.failed())
        {
            setStatus(resultMessage("Export all tracks", result));
            return;
        }
        if (const auto result = copyAudioTrack(document_.audioFile, "voice_main");
            result.failed())
        {
            setStatus(resultMessage("Export all tracks", result));
            return;
        }
        for (const auto& track : document_.backingTracks)
        {
            if (const auto result = copyAudioTrack(
                    track.audioFile,
                    track.styleName.isNotEmpty() ? track.styleName : "backing_vocal");
                result.failed())
            {
                setStatus(resultMessage("Export all tracks", result));
                return;
            }
        }

        int exportedMidiCount = 0;
        const auto midiResult = exportMidiFilesTo(directory, exportedMidiCount);
        if (midiResult.failed() && exportedAudioCount == 0)
        {
            setStatus(resultMessage("Export all tracks", midiResult));
            return;
        }

        completeExport("Exported "
            + juce::String(exportedAudioCount)
            + " audio stem"
            + (exportedAudioCount == 1 ? "" : "s")
            + " and "
            + juce::String(exportedMidiCount)
            + " MIDI file"
            + (exportedMidiCount == 1 ? "" : "s"),
            directory);
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
        openAudioButton_.setEnabled(document_.instrumentalFile.existsAsFile());
        dirty_ = true;
        ++documentRevision_;
        lastDirtyTimeMs_ = juce::Time::getMillisecondCounterHiRes();
        updatePlaybackNotes();
        updateInfoText();
        repaint();
        sendWebProjectState();
    }

    void syncInspector()
    {
        openAudioButton_.setEnabled(document_.instrumentalFile.existsAsFile());
        bpmEditor_.setText(juce::String(document_.bpm, 0), false);
        keyEditor_.setText(document_.key, false);
        updateInfoText();
        repaint();
        sendWebProjectState();
    }

    void updateInfoText()
    {
        juce::String info;
        info << (dirty_ ? "Unsaved changes\n" : "Saved\n")
             << "Audio: " << (document_.audioFile.existsAsFile() ? document_.audioFile.getFileName() : "none") << "\n"
             << "Instrumental: " << (document_.instrumentalFile.existsAsFile() ? document_.instrumentalFile.getFileName() : "none") << "\n"
             << "Duration: " << juce::String(document_.duration, 2) << "s\n"
             << "Notes: " << static_cast<int>(document_.notes.size()) << "\n"
             << "Backing: " << static_cast<int>(document_.backingNotes.size())
             << (document_.backingStyleName.isNotEmpty() ? " (" + document_.backingStyleName + ")" : "") << "\n"
             << "Boundaries: " << static_cast<int>(document_.boundaries.size()) << "\n"
             << "Tempo/Signature/Chords: " << static_cast<int>(document_.tempoSegments.size())
             << " / " << static_cast<int>(document_.timeSignatures.size())
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
        sendWebStatus();
    }

    juce::AudioFormatManager formatManager_;
    AnnotationDocument document_;
    AnnotationEditorComponent editor_;
    juce::AudioTransportSource transport_;
    juce::AudioTransportSource instrumentalTransport_;
    juce::AudioTransportSource backingAudioTransport_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    std::unique_ptr<juce::AudioFormatReaderSource> instrumentalReaderSource_;
    std::unique_ptr<juce::AudioFormatReaderSource> backingAudioReaderSource_;
    juce::File backingAudioTransportFile_;
    juce::AudioBuffer<float> leadAudioMixBuffer_;
    juce::AudioBuffer<float> instrumentalMixBuffer_;
    juce::AudioBuffer<float> backingAudioMixBuffer_;
    std::array<PlaybackNoteState, 2> noteStates_;
    std::atomic<PlaybackNoteState*> activeNoteState_ { nullptr };
    int writeNoteStateIndex_ = 0;
    std::atomic<bool> leadAudioMuted_ { false };
    std::atomic<bool> leadNotesMuted_ { false };
    std::atomic<bool> backingAudioMuted_ { false };
    std::atomic<bool> backingNotesMuted_ { false };
    std::atomic<bool> instrumentalMuted_ { false };
    std::atomic<bool> instrumentalSolo_ { false };
    std::atomic<bool> leadMuted_ { false };
    std::atomic<bool> leadSolo_ { false };
    std::atomic<bool> backingMuted_ { false };
    std::atomic<bool> backingSolo_ { false };
    std::array<std::atomic<bool>, kMaxBackingTracks> backingTrackMuted_ {};
    std::array<std::atomic<bool>, kMaxBackingTracks> backingTrackSoloed_ {};
    std::array<std::atomic<bool>, kMaxBackingTracks> backingTrackAudioMuted_ {};
    std::array<std::atomic<bool>, kMaxBackingTracks> backingTrackNotesMuted_ {};
    std::atomic<int> activeBackingTrackIndex_ { -1 };
    std::atomic<float> masterVolume_ { 0.76f };
    std::array<std::atomic<float>, 2> outputPeakLevels_ {};
    std::array<float, 2> outputMeterLevels_ {};
    std::atomic<double> audioSampleRate_ { 44100.0 };
    std::array<double, kMaxPlaybackNotes> timelineNotePhases_ {};
    const PlaybackNoteState* renderedNoteState_ = nullptr;
    double lastTimelineRenderEnd_ = -1.0;
    std::atomic<double> clickedToneFrequency_ { 440.0 };
    std::atomic<bool> clickedToneHeld_ { false };
    std::atomic<int> clickedToneLeaseSamples_ { 0 };
    std::atomic<int> clickedToneSamplesRemaining_ { 0 };
    std::atomic<unsigned int> clickedToneSerial_ { 0 };
    unsigned int renderedClickedToneSerial_ = 0;
    double clickedTonePhase_ = 0.0;
    double clickedToneDetunedPhase_ = 0.0;
    double clickedToneCurrentFrequency_ = 440.0;
    int clickedToneElapsedSamples_ = 0;
    bool clickedToneWasHeld_ = false;
    bool loopAuditionActive_ = false;
    bool cycleLoopActive_ = false;
    double loopStartTime_ = 0.0;
    double loopEndTime_ = 0.0;
    WaveformData vocalWaveform_;
    WaveformData instrumentalWaveform_;
    WaveformData backingWaveform_;
    juce::String vocalPackedWaveform_;
    juce::String instrumentalPackedWaveform_;
    juce::String backingPackedWaveform_;
    std::array<BackingWaveformCache, kMaxBackingTracks> backingTrackWaveforms_;
    juce::File vocalWaveformFile_;
    juce::File instrumentalWaveformFile_;
    juce::File backingWaveformFile_;
    unsigned int vocalWaveformRequestRevision_ = 0;
    unsigned int instrumentalWaveformRequestRevision_ = 0;
    unsigned int backingWaveformRequestRevision_ = 0;
    bool vocalWaveformLoading_ = false;
    bool instrumentalWaveformLoading_ = false;
    bool backingWaveformLoading_ = false;
    bool webFrontendReady_ = false;
    juce::File currentJsonFile_;
    juce::File projectDirectory_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::vector<AnnotationDocument> undoStack_;
    std::vector<AnnotationDocument> redoStack_;
    std::map<juce::String, PitchTrackHistory> pitchTrackHistories_;
    unsigned int leadPitchPreviewRevision_ = 0;
    juce::File leadPitchPreviewFile_;
    std::map<juce::String, unsigned int> backingPitchPreviewRevisions_;
    std::map<juce::String, juce::File> backingPitchPreviewFiles_;
    juce::String selectedWebTrack_ { "Voice Main" };
    bool dirty_ = false;
    bool analysisRunning_ = false;
    bool aiPartsAnalysisRunning_ = false;
    bool musicalContextAnalysisRunning_ = false;
    bool backingGenerationRunning_ = false;
    bool backingAudioRenderRunning_ = false;
    juce::String backingGenerationTrackName_;
    juce::String backingAudioRenderTrackName_;
    std::vector<NoteBlock> backingAudioRenderSourceNotes_;
    bool analyzeAfterAiParts_ = false;
    bool lyricsAlignmentRunning_ = false;
    bool noteRecalculationRunning_ = false;
    bool projectSaveRunning_ = false;
    bool restoringHistory_ = false;
    unsigned int documentRevision_ = 0;
    double lastDirtyTimeMs_ = 0.0;
    std::unique_ptr<juce::ChildProcess> backingWorkerProcess_;
    juce::File backingWorkerQueueDir_;
    std::unique_ptr<juce::ChildProcess> backingAudioWorkerProcess_;
    juce::File backingAudioWorkerQueueDir_;
    std::vector<VoiceTrainingSource> voiceTrainingSources_;
    std::vector<juce::File> activeVoiceProfileSources_;
    juce::File activeVoiceProfileDirectory_;
    juce::File activeVoiceAdapterFile_;
    juce::String activeVoiceProfileId_;
    juce::String activeVoiceProfileName_;
    juce::String activeVoiceProfileQuality_ { "balanced" };
    juce::String voiceTrainingPhase_ { "idle" };
    juce::String voiceTrainingMessage_ {
        "Add clean, dry vocal recordings to build a reusable voice profile."
    };
    double voiceTrainingProgress_ = 0.0;
    bool voiceProfileCreationRunning_ = false;
    std::shared_ptr<std::atomic<bool>> voiceProfileCancellation_;
    std::vector<std::unique_ptr<juce::ChildProcess>> detachedTrainingProcesses_;
    double lastVoiceTrainingPollMs_ = 0.0;
    juce::int64 lastActiveProfilePointerModificationMs_ = 0;

    juce::TextButton openAudioButton_;
    juce::TextButton openInstrumentalButton_;
    juce::TextButton loadJsonButton_;
    juce::TextButton loadLyricsButton_;
    juce::TextButton analyzeButton_;
    juce::TextButton aiPartsButton_;
    juce::TextButton addBackingVocalButton_;
    juce::TextButton renderBackingAudioButton_;
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
    juce::ComboBox backingStyleBox_;
    juce::ComboBox boundaryKindBox_;
    juce::Label infoLabel_;
    juce::Label statusLabel_;
    synthetic_obsidian::SyntheticObsidianWebView webView_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(MainComponent)
};

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow()
        : DocumentWindow("Synthetic Obsidian",
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
    const juce::String getApplicationName() override { return "Synthetic Obsidian"; }
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
