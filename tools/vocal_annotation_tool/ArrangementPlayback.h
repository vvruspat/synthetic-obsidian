#pragma once
#include "ArrangementModel.h"
#include <map>
#include <mutex>

namespace vocal_annotation
{
// The worker decodes files and prepares immutable plans. The audio thread only
// borrows a plan; retired plans and their sample buffers are deleted by the worker.
class ArrangementPlayback final : private juce::Thread, private juce::AsyncUpdater
{
public:
    ArrangementPlayback();
    ~ArrangementPlayback() override; // Call only after the audio callback has stopped.
    void request(const AnnotationDocument&);
    void prepare(double sampleRate) noexcept;
    void process(const juce::AudioSourceChannelInfo&) noexcept;
    void seek(double seconds) noexcept;
    void setPlaying(bool value) noexcept { playing_.store(value); }
    bool isPlaying() const noexcept { return playing_.load(); }
    double position() const noexcept { return reportedPosition_.load(); }
    void setLoop(bool enabled, double start, double end) noexcept;
    juce::var waveform(const juce::File&) const; // Message thread only.
    std::function<void(const juce::String&)> onReady;
    static juce::Result renderTrack(const AnnotationDocument&, const juce::String&, const juce::File&);

private:
    struct Audio
    {
        juce::AudioBuffer<float> samples;
        double sampleRate = 44100.0;
        juce::var waveform;
    };
    struct NoteGain
    {
        double start = 0.0, end = 0.0;
        float gain = 1.0f;
    };
    struct Clip
    {
        ArrangementClip placement;
        std::shared_ptr<const Audio> audio;
        std::vector<NoteGain> gains;
        std::vector<double> gainEndTimes; // Prefix maxima, for skipping elapsed gain regions.
    };
    struct Track
    {
        bool audioAudible = false;
        bool notesAudible = false;
        float gain = 1.0f;
        std::vector<Clip> clips;
        std::vector<NoteBlock> notes;
        std::vector<double> phases;
    };
    struct Plan { std::vector<Track> tracks; double duration = 1.0; double lastEnd = -1.0; };
    using Cache = std::map<juce::String, std::weak_ptr<const Audio>>;
    static std::shared_ptr<const Audio> readAudio(const juce::File&, Cache&, juce::String& error);
    static std::unique_ptr<Plan> build(const AnnotationDocument&, Cache&, juce::String& error);
    static void mix(Plan&, juce::AudioBuffer<float>&, int start, int count,
                    double time, double sampleRate) noexcept;
    void run() override;
    void handleAsyncUpdate() override;
    std::mutex requestMutex_; // Worker/message threads only.
    std::optional<AnnotationDocument> request_;
    unsigned int revision_ = 0;
    Cache cache_;
    std::map<juce::String, juce::var> readyWaveforms_, waveforms_;
    juce::String readyError_;
    std::atomic<Plan*> pending_ { nullptr }, retired_ { nullptr };
    Plan* active_ = nullptr; // Audio-thread-owned until shutdown.
    std::atomic<bool> playing_ { false }, looping_ { false };
    std::atomic<double> seek_ { -1.0 }, reportedPosition_ { 0.0 };
    std::atomic<double> loopStart_ { 0.0 }, loopEnd_ { 0.0 };
    double position_ = 0.0, sampleRate_ = 44100.0;
};
}
