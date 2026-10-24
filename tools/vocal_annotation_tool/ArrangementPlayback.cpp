#include "ArrangementPlayback.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace vocal_annotation
{
ArrangementPlayback::ArrangementPlayback() : juce::Thread("Arrangement audio preparation") { startThread(); }
ArrangementPlayback::~ArrangementPlayback()
{
    signalThreadShouldExit();
    notify();
    stopThread(-1);
    cancelPendingUpdate();
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete active_;
}
void ArrangementPlayback::request(const AnnotationDocument& document)
{
    { std::lock_guard lock(requestMutex_); request_ = document; ++revision_; }
    notify();
}
void ArrangementPlayback::prepare(double sampleRate) noexcept { sampleRate_ = sampleRate; }
void ArrangementPlayback::seek(double seconds) noexcept
{
    if (! std::isfinite(seconds)) return;
    seconds = juce::jmax(0.0, seconds);
    reportedPosition_.store(seconds);
    seek_.store(seconds);
}
void ArrangementPlayback::setLoop(bool enabled, double start, double end) noexcept
{
    loopStart_.store(start);
    loopEnd_.store(end);
    looping_.store(enabled && end > start && std::isfinite(start) && std::isfinite(end));
}
double ArrangementPlayback::waveformDuration(const juce::File& file) const
{
    const auto found = waveformDurations_.find(file.getFullPathName());
    return found == waveformDurations_.end() ? 0.0 : found->second;
}
juce::var ArrangementPlayback::waveform(const juce::File& file) const
{
    const auto found = waveforms_.find(file.getFullPathName());
    return found == waveforms_.end() ? juce::var(juce::Array<juce::var>()) : found->second;
}
std::shared_ptr<const ArrangementPlayback::Audio> ArrangementPlayback::readAudio(
    const juce::File& file, Cache& cache, juce::String& error)
{
    if (file == juce::File()) return {};
    const auto key = file.getFullPathName() + ":" + juce::String(file.getLastModificationTime().toMilliseconds())
        + ":" + juce::String(file.getSize());
    if (const auto existing = cache[key].lock()) return existing;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0
        || reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        error = "Could not decode clip: " + file.getFileName();
        return {};
    }
    auto result = std::make_shared<Audio>();
    result->sampleRate = reader->sampleRate;
    result->samples.setSize(static_cast<int>(juce::jmin(2u, reader->numChannels)), static_cast<int>(reader->lengthInSamples));
    if (! reader->read(&result->samples, 0, result->samples.getNumSamples(), 0, true, true))
    {
        error = "Could not read clip: " + file.getFileName();
        return {};
    }
    for (int c = 0; c < result->samples.getNumChannels(); ++c)
        for (int i = 0; i < result->samples.getNumSamples(); ++i)
            if (! std::isfinite(result->samples.getSample(c, i))) result->samples.setSample(c, i, 0.0f);
    juce::Array<juce::var> waveform;
    const auto step = juce::jmax(1, result->samples.getNumSamples() / 2048);
    for (int i = 0; i < result->samples.getNumSamples(); i += step)
    {
        const auto range = result->samples.findMinMax(0, i, juce::jmin(step, result->samples.getNumSamples() - i));
        waveform.add(juce::Array<juce::var> { range.getStart(), range.getEnd() });
    }
    result->waveform = waveform;
    cache[key] = result;
    return result;
}
std::unique_ptr<ArrangementPlayback::Plan> ArrangementPlayback::build(
    const AnnotationDocument& document, Cache& cache, juce::String& error)
{
    auto plan = std::make_unique<Plan>();
    plan->duration = arrangement::duration(document);
    const auto anySolo = std::any_of(document.tracks.begin(), document.tracks.end(), [](const auto& t) { return t.solo; });
    for (const auto& source : document.tracks)
    {
        Track track;
        const auto audible = ! source.muted && (! anySolo || source.solo);
        track.audioAudible = audible && ! source.audioMuted;
        track.notesAudible = audible && ! source.notesMuted;
        track.gain = source.id == "Instrumental" ? static_cast<float>(document.instrumentalGain) : 1.0f;
        track.notes = arrangement::notes(document, source.id);
        std::erase_if(track.notes, [](const auto& note)
        {
            return ! std::isfinite(note.start) || ! std::isfinite(note.end) || note.end <= note.start
                || ! std::isfinite(note.pitchExact) || ! std::isfinite(note.gainDb);
        });
        for (auto& note : track.notes)
        {
            std::erase_if(note.curve, [](const auto& point)
            { return ! std::isfinite(point.time) || ! std::isfinite(point.midi); });
            std::stable_sort(note.curve.begin(), note.curve.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        }
        track.phases.resize(track.notes.size(), 0.0);
        std::sort(track.notes.begin(), track.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        for (const auto& placement : source.clips)
        {
            Clip clip;
            clip.placement = placement;
            clip.audio = readAudio(placement.audioFile, cache, error);
            // Resolve clip IDs and dB conversions on the worker, never per audio sample.
            for (const auto& note : track.notes)
            {
                const auto gain = juce::Decibels::decibelsToGain(
                    static_cast<float>(juce::jlimit(-24.0, 12.0, note.gainDb)));
                if (juce::approximatelyEqual(gain, 1.0f) || note.start >= placement.start + placement.duration
                    || note.end <= placement.start
                    || (note.clipId != placement.id && placement.id != "pitch-preview"))
                    continue;
                clip.gains.push_back({ note.start, note.end, gain });
                clip.gainEndTimes.push_back(juce::jmax(
                    note.end, clip.gainEndTimes.empty() ? note.end : clip.gainEndTimes.back()));
            }
            track.clips.push_back(std::move(clip));
        }
        plan->tracks.push_back(std::move(track));
    }
    return plan;
}
void ArrangementPlayback::run()
{
    while (! threadShouldExit())
    {
        delete retired_.exchange(nullptr);
        std::optional<AnnotationDocument> document;
        unsigned int revision;
        { std::lock_guard lock(requestMutex_); document.swap(request_); revision = revision_; }
        if (! document) { wait(30); continue; }
        juce::String error;
        try
        {
            auto plan = build(*document, cache_, error);
            std::lock_guard lock(requestMutex_);
            if (revision != revision_ || threadShouldExit()) continue;
            readyWaveforms_.clear();
            readyWaveformDurations_.clear();
            for (const auto& track : plan->tracks)
                for (const auto& clip : track.clips)
                    if (clip.audio)
                    {
                        const auto path = clip.placement.audioFile.getFullPathName();
                        readyWaveforms_[path] = clip.audio->waveform;
                        readyWaveformDurations_[path] = clip.audio->samples.getNumSamples() / clip.audio->sampleRate;
                    }
            readyError_ = error;
            delete pending_.exchange(plan.release());
            std::erase_if(cache_, [](const auto& entry) { return entry.second.expired(); });
            triggerAsyncUpdate();
        }
        catch (const std::exception& exception)
        {
            std::lock_guard lock(requestMutex_);
            readyError_ = "Could not prepare arrangement: " + juce::String(exception.what());
            triggerAsyncUpdate();
        }
    }
}
void ArrangementPlayback::handleAsyncUpdate()
{
    juce::String error;
    { std::lock_guard lock(requestMutex_); for (const auto& [path, waveform] : readyWaveforms_) waveforms_[path] = waveform; waveformDurations_ = readyWaveformDurations_; error = readyError_; }
    if (onReady) onReady(error);
}
void ArrangementPlayback::mix(Plan& plan, juce::AudioBuffer<float>& buffer,
    int start, int count, double time, double sampleRate) noexcept
{
    const auto discontinuity = std::abs(time - plan.lastEnd) > 1.5 / sampleRate;
    plan.lastEnd = time + count / sampleRate;
    for (auto& track : plan.tracks)
    {
        if (discontinuity) std::fill(track.phases.begin(), track.phases.end(), 0.0);
        if (track.audioAudible)
            for (const auto& clip : track.clips)
            {
                if (! clip.audio || clip.placement.start >= time + count / sampleRate
                    || clip.placement.start + clip.placement.duration <= time) continue;
                const auto& source = clip.audio->samples;
                // These bounds include only gains overlapping this block, even after a seek/loop.
                const auto firstGain = static_cast<size_t>(std::upper_bound(
                    clip.gainEndTimes.begin(), clip.gainEndTimes.end(), time) - clip.gainEndTimes.begin());
                const auto lastGain = static_cast<size_t>(std::lower_bound(
                    clip.gains.begin(), clip.gains.end(), time + count / sampleRate,
                    [](const auto& gain, double end) { return gain.start < end; }) - clip.gains.begin());
                for (int i = 0; i < count; ++i)
                {
                    const auto relative = time + i / sampleRate - clip.placement.start;
                    if (relative < 0.0 || relative >= clip.placement.duration) continue;
                    const auto sourcePosition = (relative + clip.placement.offset) * clip.audio->sampleRate;
                    if (sourcePosition < 0.0 || sourcePosition >= source.getNumSamples()) continue;
                    const auto index = static_cast<int>(sourcePosition);
                    const auto next = juce::jmin(index + 1, source.getNumSamples() - 1);
                    const auto fraction = static_cast<float>(sourcePosition - index);
                    auto noteGain = 1.0f;
                    const auto absoluteTime = time + i / sampleRate;
                    for (auto gainIndex = firstGain; gainIndex < lastGain; ++gainIndex)
                    {
                        const auto& gain = clip.gains[gainIndex];
                        if (absoluteTime < gain.start || absoluteTime >= gain.end) continue;
                        const auto edge = juce::jlimit(0.0, 1.0,
                            juce::jmin(absoluteTime - gain.start, gain.end - absoluteTime) / 0.005);
                        noteGain *= 1.0f + (gain.gain - 1.0f) * static_cast<float>(edge);
                    }
                    const auto fade = static_cast<float>(juce::jlimit(0.0, 1.0,
                        juce::jmin(relative, clip.placement.duration - relative) / 0.003));
                    for (int c = 0; c < buffer.getNumChannels(); ++c)
                    {
                        const auto channel = juce::jmin(c, source.getNumChannels() - 1);
                        const auto a = source.getSample(channel, index);
                        const auto b = source.getSample(channel, next);
                        buffer.addSample(c, start + i, (a + (b - a) * fraction) * fade * track.gain * noteGain);
                    }
                }
            }
        if (track.notesAudible)
            for (size_t noteIndex = 0; noteIndex < track.notes.size(); ++noteIndex)
            {
                const auto& note = track.notes[noteIndex];
                auto& phase = track.phases[noteIndex];
                if (note.start >= time + count / sampleRate) break;
                if (note.end <= time) continue;
                for (int i = 0; i < count; ++i)
                {
                    const auto t = time + i / sampleRate;
                    if (t < note.start || t >= note.end) continue;
                    const auto envelope = juce::jlimit(0.0, 1.0, juce::jmin(t - note.start, note.end - t) / 0.01);
                    auto midi = note.pitchExact;
                    if (! note.curve.empty()) midi = note.curve.back().midi;
                    const auto point = std::lower_bound(note.curve.begin(), note.curve.end(), t,
                        [](const auto& p, double at) { return p.time < at; });
                    if (point != note.curve.end() && std::isfinite(point->midi))
                    {
                        midi = point->midi;
                        if (point != note.curve.begin())
                        {
                            const auto& previous = *std::prev(point);
                            if (point->time > previous.time && std::isfinite(previous.midi))
                                midi = previous.midi + (point->midi - previous.midi) * (t - previous.time) / (point->time - previous.time);
                        }
                    }
                    const auto frequency = 440.0 * std::pow(2.0, (juce::jlimit(0.0, 127.0, midi) - 69.0) / 12.0);
                    const auto value = static_cast<float>(std::sin(phase) * envelope * 0.13
                        * std::pow(10.0, juce::jlimit(-24.0, 12.0, note.gainDb) / 20.0));
                    phase = std::fmod(phase + frequency * juce::MathConstants<double>::twoPi / sampleRate, juce::MathConstants<double>::twoPi);
                    for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.addSample(c, start + i, value);
                }
            }
    }
}
void ArrangementPlayback::process(const juce::AudioSourceChannelInfo& info) noexcept
{
    if (retired_.load() == nullptr)
        if (auto* next = pending_.exchange(nullptr))
        {
            retired_.store(active_);
            active_ = next;
        }
    const auto requestedPosition = seek_.exchange(-1.0);
    if (requestedPosition >= 0.0) position_ = requestedPosition;
    if (active_ == nullptr || ! playing_.load() || info.buffer == nullptr || sampleRate_ <= 0.0) return;
    int rendered = 0;
    while (rendered < info.numSamples)
    {
        const auto loopStart = loopStart_.load();
        const auto end = looping_.load() ? loopEnd_.load() : active_->duration;
        if (position_ >= end)
        {
            if (looping_.load() && end > loopStart) position_ = loopStart;
            else { playing_.store(false); break; }
        }
        const auto count = juce::jmin(info.numSamples - rendered,
            juce::jmax(1, static_cast<int>(std::ceil(juce::jmin((end - position_) * sampleRate_, static_cast<double>(info.numSamples))))));
        mix(*active_, *info.buffer, info.startSample + rendered, count, position_, sampleRate_);
        position_ += count / sampleRate_;
        rendered += count;
    }
    reportedPosition_.store(position_);
}
juce::Result ArrangementPlayback::renderTrack(const AnnotationDocument& document, const juce::String& id, const juce::File& file)
{
    auto copy = document;
    std::erase_if(copy.tracks, [&](const auto& track) { return track.id != id; });
    if (copy.tracks.empty()) return juce::Result::fail("Track not found.");
    copy.tracks.front().muted = copy.tracks.front().audioMuted = copy.tracks.front().solo = false;
    copy.tracks.front().notesMuted = true;
    Cache cache;
    juce::String error;
    auto plan = build(copy, cache, error);
    if (error.isNotEmpty()) return juce::Result::fail(error);
    auto stream = file.createOutputStream();
    if (! stream) return juce::Result::fail("Could not create track audio.");
    stream->setPosition(0);
    if (stream->truncate().failed()) return juce::Result::fail("Could not replace track audio.");
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), 48000.0, 2, 24, {}, 0));
    if (! writer) return juce::Result::fail("Could not create WAV writer.");
    stream.release();
    juce::AudioBuffer<float> buffer(2, 4096);
    const auto total = static_cast<juce::int64>(std::ceil(plan->duration * 48000.0));
    for (juce::int64 sample = 0; sample < total; sample += buffer.getNumSamples())
    {
        const auto count = static_cast<int>(juce::jmin<juce::int64>(buffer.getNumSamples(), total - sample));
        buffer.clear();
        mix(*plan, buffer, 0, count, static_cast<double>(sample) / 48000.0, 48000.0);
        if (! writer->writeFromAudioSampleBuffer(buffer, 0, count)) return juce::Result::fail("Could not write track audio.");
    }
    return juce::Result::ok();
}
}
