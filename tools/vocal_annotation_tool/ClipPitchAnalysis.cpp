#include "ClipPitchAnalysis.h"
#include "RuntimeManager.h"
#include "SashaNoteAnalysis.h"
#include <algorithm>
#include <cmath>

namespace vocal_annotation
{
juce::Result enrichClipPitch(const ArrangementClip& clip, std::vector<NoteBlock>& notes,
                             const std::atomic<bool>& cancelled)
{
    if (notes.empty()) return juce::Result::ok();
    auto args = RuntimeManager::workerCommand(RuntimePack::core, "analyze_notes_pyin.py");
    if (args.isEmpty()) return juce::Result::fail("The Core AI runtime is not installed.");
    args.addArray({ clip.audioFile.getFullPathName(), "--sensitivity", "0.72",
        "--pitch-backend", "rmvpe", "--rmvpe-model", RuntimeManager::rmvpeModelFile().getFullPathName(),
        "--rmvpe-implementation", RuntimeManager::soulXSourceRoot().getChildFile("preprocess/tools/f0_extraction.py").getFullPathName(),
        "--analysis-window", juce::String(clip.offset, 6) + ":" + juce::String(clip.offset + clip.duration, 6) });
    juce::ChildProcess process;
    if (! process.start(args)) return juce::Result::fail("Could not start the RMVPE analysis worker.");
    juce::String output;
    while (process.isRunning())
    {
        if (cancelled.load()) { process.kill(); return juce::Result::fail("Analysis cancelled."); }
        output += process.readAllProcessOutput();
        juce::Thread::sleep(40);
    }
    output += process.readAllProcessOutput();
    if (process.getExitCode() != 0) return juce::Result::fail("RMVPE failed: " + output.substring(juce::jmax(0, output.length() - 1200)));
    const auto parsed = juce::JSON::parse(output);
    const auto* values = parsed["notes"].getArray();
    if (values == nullptr) return juce::Result::fail("RMVPE returned invalid note data.");
    std::vector<NoteBlock> observations;
    for (const auto& value : *values)
    {
        NoteBlock note;
        note.start = value["start"];
        note.end = note.start + static_cast<double>(value["duration"]);
        note.pitchExact = value.hasProperty("pitch_exact") ? static_cast<double>(value["pitch_exact"])
                                                           : static_cast<double>(value["pitch"]);
        if (! std::isfinite(note.start) || ! std::isfinite(note.end) || ! std::isfinite(note.pitchExact)) continue;
        for (const auto& token : juce::StringArray::fromTokens(value["curve"].toString(), ";", ""))
        {
            const auto fields = juce::StringArray::fromTokens(token, ":", "");
            if (fields.size() >= 2)
                note.curve.push_back({ note.start + fields[0].getDoubleValue(), fields[1].getDoubleValue(),
                    fields.size() >= 3 ? fields[2].getDoubleValue() : static_cast<double>(value["confidence"]) });
        }
        observations.push_back(std::move(note));
    }
    SashaNoteAnalysis::attachPitchCurves(notes, observations);
    for (auto& note : notes)
    {
        std::vector<double> pitches;
        for (const auto& point : note.curve)
            if (std::isfinite(point.midi) && point.confidence > 0.0) pitches.push_back(point.midi);
        if (pitches.empty()) continue;
        const auto middle = pitches.begin() + static_cast<std::ptrdiff_t>(pitches.size() / 2);
        std::nth_element(pitches.begin(), middle, pitches.end());
        note.pitchExact = juce::jlimit(0.0, 127.0, *middle);
        note.pitch = juce::roundToInt(note.pitchExact);
    }
    return juce::Result::ok();
}
}
