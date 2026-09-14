#include "RuntimeManager.h"

#include <array>

namespace vocal_annotation
{

namespace
{
juce::File preferredOrLegacy(
    const juce::File& preferred,
    const juce::File& legacy)
{
    if (preferred.exists() || ! legacy.exists())
        return preferred;

    return legacy;
}

juce::File executableInsidePack(
    const juce::File& root,
    const juce::String& scriptName)
{
    const auto workerName = juce::File(scriptName).getFileNameWithoutExtension();
    const std::array candidates {
        root.getChildFile("workers").getChildFile(workerName),
        root.getChildFile("bin").getChildFile(workerName)
    };

    for (const auto& candidate : candidates)
        if (candidate.existsAsFile())
            return candidate;

    return {};
}

juce::File dispatcherInsidePack(
    const juce::File& root,
    RuntimePack pack)
{
    juce::String dispatcherName;
    switch (pack)
    {
        case RuntimePack::core: dispatcherName = "core-worker"; break;
        case RuntimePack::backingNotes: dispatcherName = "backing-notes-worker"; break;
        case RuntimePack::voiceEngine: dispatcherName = "voice-engine-worker"; break;
    }

    const std::array candidates {
        root.getChildFile("workers").getChildFile(dispatcherName),
        root.getChildFile(dispatcherName)
    };
    for (const auto& candidate : candidates)
        if (candidate.existsAsFile())
            return candidate;

    return {};
}

juce::File manifestPackRoot(
    const juce::File& runtimeRoot,
    const juce::String& expectedPlatform,
    const juce::String& id)
{
    const auto manifestFile = runtimeRoot.getChildFile("runtime-manifest.json");
    if (! manifestFile.existsAsFile())
        return {};

    const auto manifestValue = juce::JSON::parse(manifestFile);
    const auto* manifest = manifestValue.getDynamicObject();
    if (manifest == nullptr
        || manifest->getProperty("platform").toString() != expectedPlatform)
        return {};

    const auto* packs =
        manifest->getProperty("packs").getDynamicObject();
    if (packs == nullptr)
        return {};

    const auto* pack =
        packs->getProperty(juce::Identifier(id)).getDynamicObject();
    if (pack == nullptr)
        return {};

    const auto relativePath =
        pack->getProperty("relativePath").toString().trim();
    if (relativePath.isEmpty()
        || juce::File::isAbsolutePath(relativePath)
        || relativePath.contains(".."))
        return {};

    const auto result = runtimeRoot.getChildFile(relativePath);
    return result.isDirectory() ? result : juce::File {};
}
} // namespace

bool RuntimeManager::isAppleSiliconBuild() noexcept
{
   #if JUCE_MAC && (defined(__arm64__) || defined(__aarch64__))
    return true;
   #else
    return false;
   #endif
}

juce::String RuntimeManager::platformId()
{
    return isAppleSiliconBuild() ? "macos-arm64" : "unsupported";
}

juce::File RuntimeManager::applicationSupportRoot()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Library")
        .getChildFile("Application Support")
        .getChildFile("Synthetic Obsidian");
   #else
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Synthetic Obsidian");
   #endif
}

juce::File RuntimeManager::runtimeRoot()
{
    return applicationSupportRoot().getChildFile("Runtime");
}

juce::File RuntimeManager::modelsRoot()
{
    return applicationSupportRoot().getChildFile("Models");
}

juce::File RuntimeManager::legacyRoot()
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile(".synthetic_obsidian");
}

juce::File RuntimeManager::voiceProfilesRoot()
{
    return preferredOrLegacy(
        applicationSupportRoot().getChildFile("Voice Profiles"),
        legacyRoot().getChildFile("models").getChildFile("voice_profiles"));
}

juce::File RuntimeManager::voiceTrainingJobsRoot()
{
    return preferredOrLegacy(
        applicationSupportRoot()
            .getChildFile("Training")
            .getChildFile("Jobs"),
        legacyRoot().getChildFile("training").getChildFile("jobs"));
}

juce::String RuntimeManager::packId(RuntimePack pack)
{
    switch (pack)
    {
        case RuntimePack::core: return "core";
        case RuntimePack::backingNotes: return "backing-notes";
        case RuntimePack::voiceEngine: return "voice-engine";
    }

    jassertfalse;
    return {};
}

juce::File RuntimeManager::bundledPackRoot(RuntimePack pack)
{
    const auto executable =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    return executable.getParentDirectory()
        .getParentDirectory()
        .getChildFile("Resources")
        .getChildFile("Runtime")
        .getChildFile(packId(pack));
}

juce::File RuntimeManager::packRoot(RuntimePack pack)
{
    if (const auto installed = manifestPackRoot(
            runtimeRoot(),
            platformId(),
            packId(pack));
        installed.isDirectory())
    {
        return installed;
    }

    const auto bundled = bundledPackRoot(pack);
    if (bundled.isDirectory())
        return bundled;

    if (pack == RuntimePack::voiceEngine)
    {
        const auto legacy = legacyRoot()
                                .getChildFile("runtime")
                                .getChildFile("soulx-singer");
        if (legacy.isDirectory())
            return legacy;
    }

    return runtimeRoot()
        .getChildFile("Packs")
        .getChildFile(packId(pack))
        .getChildFile("current");
}

juce::File RuntimeManager::developmentFile(const juce::String& relativePath)
{
   #if defined(SYNTHETIC_OBSIDIAN_DEV_ROOT)
    return juce::File(SYNTHETIC_OBSIDIAN_DEV_ROOT)
        .getChildFile(relativePath);
   #else
    juce::ignoreUnused(relativePath);
    return {};
   #endif
}

juce::File RuntimeManager::scriptFile(
    RuntimePack pack,
    const juce::String& scriptName)
{
    const auto installed =
        packRoot(pack).getChildFile("scripts").getChildFile(scriptName);
    if (installed.existsAsFile())
        return installed;

    if (scriptName == "analyze_notes_pyin.py")
    {
        const auto development =
            developmentFile("research/svc_pitch/analyze_notes_pyin.py");
        if (development.existsAsFile())
            return development;
    }

    const auto development = developmentFile(
        "tools/vocal_annotation_tool/" + scriptName);
    return development.existsAsFile() ? development : installed;
}

juce::File RuntimeManager::pythonExecutable(RuntimePack pack)
{
    const auto root = packRoot(pack);
    const std::array installedCandidates {
        root.getChildFile("python").getChildFile("bin").getChildFile("python3"),
        root.getChildFile("python").getChildFile("bin").getChildFile("python"),
        root.getChildFile(".venv").getChildFile("bin").getChildFile("python")
    };
    for (const auto& candidate : installedCandidates)
        if (candidate.existsAsFile())
            return candidate;

    if (pack == RuntimePack::backingNotes)
    {
        const auto development =
            developmentFile(".venv-llm/bin/python");
        if (development.existsAsFile())
            return development;
    }
    else if (pack == RuntimePack::core)
    {
        const auto development =
            developmentFile("research/.venv_seed_vc/bin/python");
        if (development.existsAsFile())
            return development;
    }

    return {};
}

juce::StringArray RuntimeManager::workerCommand(
    RuntimePack pack,
    const juce::String& scriptName)
{
    const auto root = packRoot(pack);
    if (const auto executable = executableInsidePack(root, scriptName);
        executable.existsAsFile())
    {
        return { executable.getFullPathName() };
    }

    if (const auto dispatcher = dispatcherInsidePack(root, pack);
        dispatcher.existsAsFile())
    {
        return {
            dispatcher.getFullPathName(),
            "--synthetic-entrypoint",
            scriptName
        };
    }

    const auto python = pythonExecutable(pack);
    const auto script = scriptFile(pack, scriptName);
    if (! python.existsAsFile() || ! script.existsAsFile())
        return {};

    return { python.getFullPathName(), script.getFullPathName() };
}

bool RuntimeManager::isWorkerAvailable(
    RuntimePack pack,
    const juce::String& scriptName)
{
    return ! workerCommand(pack, scriptName).isEmpty();
}

juce::File RuntimeManager::soulXSourceRoot()
{
    const auto installed = packRoot(RuntimePack::voiceEngine)
                               .getChildFile("source");
    if (installed.isDirectory())
        return installed;

    return legacyRoot()
        .getChildFile("runtime")
        .getChildFile("soulx-singer")
        .getChildFile("source");
}

juce::File RuntimeManager::soulXModelFile()
{
    const auto bundled = packRoot(RuntimePack::voiceEngine)
                             .getChildFile("models")
                             .getChildFile("model-svc.pt");
    if (bundled.existsAsFile())
        return bundled;

    const auto preferred = modelsRoot()
                               .getChildFile("SoulX-Singer")
                               .getChildFile("model-svc.pt");
    if (preferred.existsAsFile())
        return preferred;

    return legacyRoot()
        .getChildFile("models")
        .getChildFile("SoulX-Singer")
        .getChildFile("model-svc.pt");
}

juce::File RuntimeManager::soulXSingerModelFile()
{
    const auto bundled = packRoot(RuntimePack::voiceEngine)
                             .getChildFile("models")
                             .getChildFile("model.pt");
    if (bundled.existsAsFile())
        return bundled;

    const auto preferred = modelsRoot()
                               .getChildFile("SoulX-Singer")
                               .getChildFile("model.pt");
    if (preferred.existsAsFile())
        return preferred;

    return legacyRoot()
        .getChildFile("models")
        .getChildFile("SoulX-Singer")
        .getChildFile("model.pt");
}

juce::File RuntimeManager::rmvpeModelFile()
{
    const auto bundled = packRoot(RuntimePack::voiceEngine)
                             .getChildFile("models")
                             .getChildFile("rmvpe.pt");
    if (bundled.existsAsFile())
        return bundled;

    const auto preferred = modelsRoot()
                               .getChildFile("rmvpe")
                               .getChildFile("rmvpe.pt");
    if (preferred.existsAsFile())
        return preferred;

    return legacyRoot()
        .getChildFile("models")
        .getChildFile("rmvpe")
        .getChildFile("rmvpe.pt");
}

juce::File RuntimeManager::boundaryCheckpointFile()
{
    constexpr auto checkpointName = "sasha_manual_boundary_tcn.pt";
    const auto bundled = packRoot(RuntimePack::core)
                             .getChildFile("models")
                             .getChildFile(checkpointName);
    if (bundled.existsAsFile())
        return bundled;

    const auto installed = modelsRoot()
                               .getChildFile("Analysis")
                               .getChildFile(checkpointName);
    if (installed.existsAsFile())
        return installed;

    return developmentFile(
        "data/user_boundary_alignment/" + juce::String(checkpointName));
}

juce::File RuntimeManager::boundaryNonVocalCheckpointFile()
{
    constexpr auto checkpointName =
        "multilingual_mps_full_strict_pause_silence_head_thr090_tcn.pt";
    const auto bundled = packRoot(RuntimePack::core)
                             .getChildFile("models")
                             .getChildFile(checkpointName);
    if (bundled.existsAsFile())
        return bundled;

    const auto installed = modelsRoot()
                               .getChildFile("Analysis")
                               .getChildFile(checkpointName);
    if (installed.existsAsFile())
        return installed;

    return developmentFile(
        "data/gtsinger_alignment/" + juce::String(checkpointName));
}

juce::File RuntimeManager::backingTransformerModel()
{
    const auto bundled = packRoot(RuntimePack::backingNotes)
                             .getChildFile("models")
                             .getChildFile("backing_transformer_v1");
    if (bundled.isDirectory())
        return bundled;

    const auto preferred = modelsRoot()
                               .getChildFile("Backing Vocals")
                               .getChildFile("backing_transformer_v1");
    return preferredOrLegacy(
        preferred,
        legacyRoot()
            .getChildFile("models")
            .getChildFile("backing_vocals")
            .getChildFile("backing_transformer_v1"));
}

} // namespace vocal_annotation
