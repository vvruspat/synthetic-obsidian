#pragma once

#include <JuceHeader.h>

namespace vocal_annotation
{

enum class RuntimePack
{
    core,
    backingNotes,
    voiceEngine
};

// Runtime discovery is intentionally kept out of the audio path. The release
// application uses signed, versioned Apple Silicon packs from Application
// Support, while non-Release builds may fall back to the source checkout.
class RuntimeManager final
{
public:
    static bool isAppleSiliconBuild() noexcept;
    static juce::String platformId();

    static juce::File applicationSupportRoot();
    static juce::File runtimeRoot();
    static juce::File modelsRoot();
    static juce::File voiceProfilesRoot();
    static juce::File voiceTrainingJobsRoot();

    static juce::File packRoot(RuntimePack pack);
    static juce::File scriptFile(RuntimePack pack, const juce::String& scriptName);
    static juce::StringArray workerCommand(
        RuntimePack pack,
        const juce::String& scriptName);
    static bool isWorkerAvailable(
        RuntimePack pack,
        const juce::String& scriptName);

    static juce::File soulXSourceRoot();
    static juce::File soulXModelFile();
    static juce::File soulXSingerModelFile();
    static juce::File rmvpeModelFile();
    static juce::File boundaryCheckpointFile();
    static juce::File boundaryNonVocalCheckpointFile();
    static juce::File backingTransformerModel();

    static juce::File developmentFile(const juce::String& relativePath);

private:
    static juce::String packId(RuntimePack pack);
    static juce::File bundledPackRoot(RuntimePack pack);
    static juce::File legacyRoot();
    static juce::File pythonExecutable(RuntimePack pack);
};

} // namespace vocal_annotation
