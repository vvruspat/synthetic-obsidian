#include "SeedVCBridge.h"

namespace
{
    constexpr int kDiffusionSteps = 30;

    bool isProjectRoot (const juce::File& dir)
    {
        return dir.getChildFile ("research/svc_pitch/run_seed_vc_file.py").existsAsFile()
            && dir.getChildFile ("research/.venv_seed_vc/bin/python").existsAsFile()
            && dir.getChildFile ("third_party/seed-vc/inference.py").existsAsFile();
    }
}

juce::String SeedVCBridge::render (const juce::File& sourceFile,
                                   const juce::File& outputRoot,
                                   const std::vector<RenderSpec>& specs,
                                   RenderedFiles& renderedFiles) const
{
    renderedFiles.clear();

    if (! sourceFile.existsAsFile())
        return "Source audio file does not exist.";

    if (specs.empty())
        return "No Seed-VC render specs requested.";

    const auto projectRoot = findProjectRoot();
    if (! projectRoot.isDirectory())
        return "Could not find Synthetic Obsidian project root with Seed-VC research runner.";

    const auto python = projectRoot.getChildFile ("research/.venv_seed_vc/bin/python");
    const auto runner = projectRoot.getChildFile ("research/svc_pitch/run_seed_vc_file.py");
    const auto polisher = projectRoot.getChildFile ("research/svc_pitch/polish_outputs.py");
    if (! polisher.existsAsFile())
        return "Could not find Seed-VC polish script.";

    outputRoot.createDirectory();
    const auto rawRoot = outputRoot.getChildFile ("raw");
    const auto polishedRoot = outputRoot.getChildFile ("headroom_only");

    juce::StringArray args {
        python.getFullPathName(),
        runner.getFullPathName(),
        "--source", sourceFile.getFullPathName(),
        "--output-root", rawRoot.getFullPathName(),
        "--python-bin", python.getFullPathName(),
        "--diffusion-steps", juce::String (kDiffusionSteps)
    };

    for (const auto& spec : specs)
        args.addArray ({ "--interval", spec.name + ":" + juce::String (spec.semitones) });

    juce::ChildProcess process;
    if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return "Could not start Seed-VC subprocess.";

    juce::String output;
    while (process.isRunning())
    {
        output << process.readAllProcessOutput();
        juce::Thread::sleep (100);
    }
    output << process.readAllProcessOutput();

    const auto exitCode = process.getExitCode();
    if (exitCode != 0)
        return "Seed-VC failed with exit code " + juce::String (exitCode) + ":\n" + output;

    const juce::StringArray polishArgs {
        python.getFullPathName(),
        polisher.getFullPathName(),
        "--input-dir", rawRoot.getFullPathName(),
        "--output-dir", polishedRoot.getFullPathName(),
        "--target-peak-db", "-3.0",
        "--mode", "headroom"
    };

    juce::ChildProcess polishProcess;
    if (! polishProcess.start (polishArgs, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return "Could not start Seed-VC polishing subprocess.";

    juce::String polishOutput;
    while (polishProcess.isRunning())
    {
        polishOutput << polishProcess.readAllProcessOutput();
        juce::Thread::sleep (100);
    }
    polishOutput << polishProcess.readAllProcessOutput();

    const auto polishExitCode = polishProcess.getExitCode();
    if (polishExitCode != 0)
        return "Seed-VC polishing failed with exit code " + juce::String (polishExitCode)
             + ":\n" + polishOutput;

    for (const auto& spec : specs)
    {
        const auto wav = findFirstWav (polishedRoot.getChildFile (spec.name));
        if (! wav.existsAsFile())
            return "Seed-VC did not produce polished output for " + spec.name + ".\n" + output;

        renderedFiles.push_back ({ spec.name, spec.semitones, spec.style, wav });
    }

    return {};
}

bool SeedVCBridge::isAvailable() const
{
    return findProjectRoot().isDirectory();
}

juce::File SeedVCBridge::findProjectRoot()
{
    juce::Array<juce::File> starts;
    starts.add (juce::File::getCurrentWorkingDirectory());
    starts.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
    starts.add (juce::File ("/Users/aleksandrkolesov/synthetic-obsidian"));

    for (auto start : starts)
    {
        for (int depth = 0; depth < 8 && start.isDirectory(); ++depth)
        {
            if (isProjectRoot (start))
                return start;
            start = start.getParentDirectory();
        }
    }

    return {};
}

juce::File SeedVCBridge::findFirstWav (const juce::File& directory)
{
    if (! directory.isDirectory())
        return {};

    juce::Array<juce::File> wavs;
    directory.findChildFiles (wavs, juce::File::findFiles, false, "*.wav");
    if (wavs.isEmpty())
        return {};

    wavs.sort();
    return wavs.getFirst();
}
