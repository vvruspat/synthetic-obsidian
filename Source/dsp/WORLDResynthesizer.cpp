#include "WORLDResynthesizer.h"
#include <cstring>

//==============================================================================
// Binary protocol constants (must match world_bridge.py)
static constexpr int kStatusOk         = 0;
static constexpr int kStatusUnavailable = 1;
// static constexpr int kStatusError   = 2;

//==============================================================================
WORLDResynthesizer::WORLDResynthesizer() = default;

WORLDResynthesizer::~WORLDResynthesizer()
{
    stopBridge();
}

//==============================================================================
juce::File WORLDResynthesizer::findBridgeScript()
{
    // 1. Next to the plugin binary (production install)
    const auto binDir = juce::File::getSpecialLocation (
                            juce::File::currentExecutableFile).getParentDirectory();
    {
        auto f = binDir.getChildFile ("world_bridge.py");
        if (f.existsAsFile()) return f;
    }

    // 2. Walk up the tree looking for Source/dsp/world_bridge.py (dev build)
    juce::File dir = binDir;
    for (int depth = 0; depth < 10; ++depth)
    {
        auto candidate = dir.getChildFile ("Source/dsp/world_bridge.py");
        if (candidate.existsAsFile()) return candidate;
        auto parent = dir.getParentDirectory();
        if (parent == dir) break;
        dir = parent;
    }

    return {};
}

//==============================================================================
bool WORLDResynthesizer::startBridge()
{
    juce::ScopedLock lock (callLock_);

    if (available_.load()) return true;

    const auto script = findBridgeScript();
    if (! script.existsAsFile())
    {
        DBG ("WORLDResynthesizer: world_bridge.py not found");
        return false;
    }

#if JUCE_WINDOWS
    const juce::String pythonExe = "python";
#else
    // The app may be launched with a conda/pyenv PATH that has the right python3.
    // Search the inherited PATH explicitly rather than relying on execvp resolution,
    // which can pick up a system python3 that lacks numpy/pyworld.
    juce::String pythonExe = "python3";
    {
        const auto pathEnv = juce::SystemStats::getEnvironmentVariable ("PATH", {});
        for (const auto& dir : juce::StringArray::fromTokens (pathEnv, ":", {}))
        {
            const auto candidate = juce::File (dir).getChildFile ("python3");
            if (candidate.existsAsFile())
            {
                pythonExe = candidate.getFullPathName();
                break;
            }
        }
    }
#endif
    DBG ("WORLDResynthesizer: using python: " + pythonExe);

    // Pass args as StringArray so JUCE does not re-tokenize and strip quotes.
    const juce::StringArray args { pythonExe, script.getFullPathName() };

    // Capture only stdout — Python's stderr goes to terminal/log, not our pipe.
    if (! process_.start (args, juce::ChildProcess::wantStdOut))
    {
        DBG ("WORLDResynthesizer: failed to start: " + args.joinIntoString (" "));
        return false;
    }

    // ── Read "PORT:<n>\n" from child stdout ───────────────────────────────────
    juce::String portLine;
    const juce::Time deadline = juce::Time::getCurrentTime()
                                + juce::RelativeTime::seconds (5.0);

    while (juce::Time::getCurrentTime() < deadline)
    {
        char ch = 0;
        if (process_.readProcessOutput (&ch, 1) == 1)
        {
            if (ch == '\n') break;
            portLine += ch;
        }
        else
        {
            juce::Thread::sleep (10);
        }
    }

    if (! portLine.startsWith ("PORT:"))
    {
        DBG ("WORLDResynthesizer: unexpected startup output: " + portLine);
        process_.kill();
        return false;
    }

    const int port = portLine.fromFirstOccurrenceOf ("PORT:", false, false).getIntValue();
    if (port <= 0 || port > 65535)
    {
        DBG ("WORLDResynthesizer: invalid port: " + portLine);
        process_.kill();
        return false;
    }

    // ── Connect socket ────────────────────────────────────────────────────────
    if (! socket_.connect ("127.0.0.1", port, 3000))
    {
        DBG ("WORLDResynthesizer: could not connect to port " + juce::String (port));
        process_.kill();
        return false;
    }

    // ── Send a probe frame to check pyworld availability ─────────────────────
    {
        const float  probeAudio[1] = { 0.0f };
        const double probeF0[1]   = { 440.0 };
        const double sr = 44100.0;
        const int32_t ns = 1, nf = 1;

        uint8_t hdr[16];
        std::memcpy (hdr,      &sr, 8);
        std::memcpy (hdr + 8,  &ns, 4);
        std::memcpy (hdr + 12, &nf, 4);

        if (! sockWrite (hdr, 16)
            || ! sockWrite (probeAudio, 4)
            || ! sockWrite (probeF0, 8))
        {
            DBG ("WORLDResynthesizer: probe send failed");
            socket_.close();
            process_.kill();
            return false;
        }

        uint8_t resp[8] = {};
        if (! sockRead (resp, 8))
        {
            DBG ("WORLDResynthesizer: no probe response");
            socket_.close();
            process_.kill();
            return false;
        }

        int32_t status = 0, outSamples = 0;
        std::memcpy (&status,     resp,     4);
        std::memcpy (&outSamples, resp + 4, 4);

        if (status == kStatusUnavailable)
        {
            DBG ("WORLDResynthesizer: pyworld not installed - using phase vocoder fallback");
            socket_.close();
            process_.kill();
            return false;
        }

        // Drain any probe output data
        if (status == kStatusOk && outSamples > 0)
        {
            std::vector<uint8_t> drain (static_cast<size_t> (outSamples) * 4);
            sockRead (drain.data(), outSamples * 4);  // best-effort
        }
    }

    available_.store (true);
    DBG ("WORLDResynthesizer: bridge ready");
    return true;
}

void WORLDResynthesizer::stopBridge()
{
    available_.store (false);
    socket_.close();
    process_.kill();
}

//==============================================================================
bool WORLDResynthesizer::resynthesizeAtPitch (
    const juce::AudioBuffer<float>& inputMono,
    double                          sampleRate,
    const std::vector<float>&       f0TargetHz,
    juce::AudioBuffer<float>&       output)
{
    if (! available_.load()) return false;

    juce::ScopedLock lock (callLock_);

    const int numSamples = inputMono.getNumSamples();
    if (numSamples < 1 || inputMono.getNumChannels() < 1) return false;
    if (output.getNumSamples() < numSamples || output.getNumChannels() < 1)
        return false;

    std::vector<double> f0d (f0TargetHz.size());
    for (size_t i = 0; i < f0TargetHz.size(); ++i)
        f0d[i] = static_cast<double> (f0TargetHz[i]);

    if (! sendFrame (inputMono.getReadPointer (0), numSamples, sampleRate, f0d))
    {
        available_.store (false);
        return false;
    }

    if (! recvFrame (output.getWritePointer (0), numSamples))
    {
        available_.store (false);
        return false;
    }

    return true;
}

//==============================================================================
bool WORLDResynthesizer::sendFrame (const float*              audio,
                                    int                        numSamples,
                                    double                     sampleRate,
                                    const std::vector<double>& f0Target)
{
    const int32_t ns = numSamples;
    const int32_t nf = static_cast<int32_t> (f0Target.size());

    uint8_t hdr[16];
    std::memcpy (hdr,      &sampleRate, 8);
    std::memcpy (hdr + 8,  &ns,        4);
    std::memcpy (hdr + 12, &nf,        4);

    return sockWrite (hdr, 16)
        && sockWrite (audio, numSamples * (int)sizeof (float))
        && (nf == 0 || sockWrite (f0Target.data(),
                                  static_cast<int> (f0Target.size()) * (int)sizeof (double)));
}

bool WORLDResynthesizer::recvFrame (float* output, int numSamples)
{
    uint8_t hdr[8] = {};
    if (! sockRead (hdr, 8)) return false;

    int32_t status = 0, outSamples = 0;
    std::memcpy (&status,     hdr,     4);
    std::memcpy (&outSamples, hdr + 4, 4);

    if (status != kStatusOk || outSamples <= 0) return false;

    const int toRead = std::min (outSamples, numSamples) * (int)sizeof (float);
    if (! sockRead (output, toRead)) return false;

    // Drain any extra bytes
    const int extra = (outSamples - numSamples) * (int)sizeof (float);
    if (extra > 0)
    {
        std::vector<uint8_t> drain (static_cast<size_t> (extra));
        sockRead (drain.data(), extra);  // best-effort, ignore result
    }

    return true;
}

//==============================================================================
bool WORLDResynthesizer::sockWrite (const void* data, int bytes)
{
    auto* ptr = static_cast<const char*> (data);
    int sent = 0;
    while (sent < bytes)
    {
        const int n = socket_.write (ptr + sent, bytes - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool WORLDResynthesizer::sockRead (void* data, int bytes)
{
    auto* ptr = static_cast<char*> (data);
    int got = 0;
    while (got < bytes)
    {
        const int n = socket_.read (ptr + got, bytes - got, true);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}
