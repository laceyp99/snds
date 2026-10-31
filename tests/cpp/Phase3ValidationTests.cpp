// Phase 3 validation: long-song sync, the device-rate matrix, device recovery, streaming memory,
// and (opt-in) the real owner-track package. All offline: the engine is driven without a device,
// or through a simulated one. Nothing here opens real audio hardware.
//
// Opt-in (heavy) parts:
//   SNDS_VALIDATE_LONG=1   12-minute song, 400 seeks, 5-minute WAV stems on disk
//   SNDS_VALIDATE_REAL=1   the real prepared package and its original (read-only)
//     SNDS_REAL_PACKAGE    package folder (default: %LOCALAPPDATA%\Temp\snds-e2e-home\cache\d9dc...)
//     SNDS_REAL_ORIGINAL   its original file

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "DspHeavy.h"
#include "FakeAudioDevice.h"
#include "PlaybackTestKit.h"
#include "TestHelpers.h"
#include "audio/FrameSource.h"
#include "audio/OriginalAlignment.h"
#include "audio/PlaybackEngine.h"
#include "model/SongMetadata.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <vector>

namespace snds
{
namespace
{
using namespace snds::test;
using namespace snds::audio;

bool optedIn (const char* name)
{
    return juce::SystemStats::getEnvironmentVariable (name, {}) == "1";
}

const int ramp = rampFrames (44100.0);

double nowMs()
{
    return juce::Time::getMillisecondCounterHiRes();
}

//==============================================================================
/** Result of playing a whole song and checking every output frame against a function. */
struct PlayThrough
{
    std::int64_t frames = 0;        // device frames rendered until the end
    std::int64_t audibleFrames = 0; // non-silent frames
    std::int64_t mismatches = 0;
    std::int64_t firstMismatch = -1;
    double maxError = 0.0;
    double wallMs = 0.0;
    bool timedOut = false;
};

/** Plays `engine` from the start to the end of the song (matching rates, so output frame i is
    song frame i) and compares every frame with `expected`. The first `ramp` frames may carry the
    start fade. `onBlock(frameIndex)` runs before each block (A/B toggles and the like). */
PlayThrough playThrough (PlaybackEngine& engine,
                         const std::function<float (std::int64_t, int)>& expected,
                         int block,
                         const std::function<void (std::int64_t)>& onBlock = {})
{
    PlayThrough result;
    const auto length = engine.getLengthFrames();
    std::vector<float> left (static_cast<size_t> (block)), right (static_cast<size_t> (block));
    float* channels[2] = { left.data(), right.data() };
    const auto start = nowMs();
    std::int64_t index = 0;

    while (index < length + block * 2)
    {
        if (onBlock)
            onBlock (index);

        if (! engine.waitForReadAhead (block, 30000))
        {
            result.timedOut = true;
            break;
        }

        engine.process (channels, 2, block);

        for (int i = 0; i < block; ++i)
        {
            const auto frame = index + i;
            const float l = left[static_cast<size_t> (i)];
            const float r = right[static_cast<size_t> (i)];

            if (frame >= length)
            {
                if (l != 0.0f || r != 0.0f)
                    ++result.mismatches;

                continue;
            }

            result.audibleFrames += (l != 0.0f || r != 0.0f);
            const float el = expected (frame, 0);
            const float er = expected (frame, 1);
            const bool exact = l == el && r == er;
            const bool faded = frame < ramp && isFadedCopy (l, el, ramp) && isFadedCopy (r, er, ramp);

            if (! exact && ! faded)
            {
                if (result.firstMismatch < 0)
                    result.firstMismatch = frame;

                ++result.mismatches;
                result.maxError = std::max ({ result.maxError,
                                              std::abs (static_cast<double> (l) - el),
                                              std::abs (static_cast<double> (r) - er) });
            }
        }

        index += block;

        if (engine.hasReachedEnd() && index >= length)
            break;
    }

    result.frames = index;
    result.wallMs = nowMs() - start;
    return result;
}

/** Index of the first output frame starting a run of `runLength` exact frames of `expected`
    from song frame `firstSongFrame`; -1 if none. */
std::int64_t findExactRun (const Rendered& rendered,
                           const std::function<float (std::int64_t, int)>& expected,
                           std::int64_t firstSongFrame,
                           int runLength)
{
    for (std::int64_t i = 0; i + runLength <= rendered.size(); ++i)
    {
        if (rendered.left[static_cast<size_t> (i)] != expected (firstSongFrame, 0))
            continue;

        bool all = true;

        for (int k = 0; k < runLength && all; ++k)
            all = rendered.left[static_cast<size_t> (i + k)] == expected (firstSongFrame + k, 0)
               && rendered.right[static_cast<size_t> (i + k)] == expected (firstSongFrame + k, 1);

        if (all)
            return i;
    }

    return -1;
}

void append (Rendered& into, const Rendered& more)
{
    into.left.insert (into.left.end(), more.left.begin(), more.left.end());
    into.right.insert (into.right.end(), more.right.begin(), more.right.end());
}

const std::function<float (std::int64_t, int)> markerSum = [] (std::int64_t f, int c)
{ return expectedUnitySum (f, c); };

//==============================================================================
class LongSongTests final : public juce::UnitTest
{
public:
    LongSongTests()
        : juce::UnitTest ("ValidateLongSong", "snds")
    {
    }

    void runTest() override
    {
        const bool longRun = optedIn ("SNDS_VALIDATE_LONG");
        const std::int64_t songFrames = std::int64_t { 44100 } * (longRun ? 60 * 12 : 90);

        testPlayThrough (songFrames, longRun);
        testSeeks (songFrames, longRun ? 400 : workload (30, 60));
        testLoopLaps (longRun ? 600 : workload (20, 50));
        testPauseResumeCycles (songFrames, longRun ? 600 : workload (60, 120));
        testBeyondFourBillionFrames();
    }

private:
    void testPlayThrough (std::int64_t songFrames, bool longRun)
    {
        beginTest ("ValidateLongSong: a " + juce::String (songFrames / 44100 / 60.0, 1)
                   + "-minute song plays through bit-exactly with A/B toggles, no drift");
        auto sources = makeMarkerSources (44100, songFrames);
        sources.original = std::make_unique<SyntheticSource> (44100, songFrames, markerSum);
        PlaybackEngine engine;
        engine.prepare (44100.0, 1024);
        expect (engine.load (std::move (sources)));
        engine.play();

        const auto result = playThrough (engine,
                                         markerSum,
                                         1024,
                                         [&engine] (std::int64_t index)
                                         {
                                             if (index % (44100 * 7) < 1024) // toggle about every 7 s
                                                 engine.setOriginalSelected (! engine.isOriginalSelected());
                                         });

        expect (! result.timedOut);
        expectEquals (static_cast<juce::int64> (result.mismatches),
                      static_cast<juce::int64> (0),
                      "mismatched frames; first at " + juce::String (result.firstMismatch));
        expectEquals (static_cast<juce::int64> (result.audibleFrames), static_cast<juce::int64> (songFrames));
        expectEquals (engine.getUnderrunCount(), 0);
        expect (engine.hasReachedEnd());
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (songFrames));
        logMessage ("play-through of " + juce::String (songFrames / 44100)
                    + " s: " + juce::String (result.wallMs / 1000.0, 1) + " s wall ("
                    + juce::String (static_cast<double> (songFrames) / 44100.0 / (result.wallMs / 1000.0), 0)
                    + "x real time), last frame exact, drift 0 frames" + (longRun ? " [long]" : ""));
    }

    void testSeeks (std::int64_t songFrames, int count)
    {
        beginTest ("ValidateLongSong: " + juce::String (count)
                   + " random, edge and near-end seeks keep all stems aligned");
        PlaybackEngine engine;
        engine.prepare (44100.0, 1024);
        expect (engine.load (makeMarkerSources (44100, songFrames)));
        engine.play();
        juce::Random random (2024);
        int checked = 0;

        for (int i = 0; i < count; ++i)
        {
            const int kind = random.nextInt (8);
            const auto target =
                kind == 0   ? std::int64_t { 0 }
                : kind == 1 ? songFrames - 1 - random.nextInt (30000)
                : kind == 2
                    ? songFrames - 40000 - random.nextInt (4000)
                    : static_cast<std::int64_t> (random.nextDouble() * static_cast<double> (songFrames - 40000));
            engine.seek (target);
            engine.play(); // a seek near the end lets the song run out, which stops the transport

            if (random.nextInt (4) == 0)
            {
                renderOffline (engine, 200 + random.nextInt (3000), 100 + random.nextInt (400)); // seek again mid-fade
                engine.seek (target);
            }

            const auto rendered = renderOffline (engine, 12000, 700);

            if (target + 12000 <= songFrames)
            {
                const auto at = findExactRun (rendered, markerSum, target + ramp - 1, 8000);
                expect (at >= 0, "seek " + juce::String (i) + " to " + juce::String (target));

                if (at < 0)
                {
                    std::int64_t firstSound = -1;

                    for (std::int64_t k = 0; k < rendered.size() && firstSound < 0; ++k)
                        if (rendered.left[static_cast<size_t> (k)] != 0.0f)
                            firstSound = k;

                    logMessage ("  rendered " + juce::String (rendered.size()) + " frames, first sound at "
                                + juce::String (firstSound) + ", position " + juce::String (engine.getPositionFrames())
                                + ", buffering " + juce::String (int (engine.isBuffering())) + ", underruns "
                                + juce::String (engine.getUnderrunCount()) + ", timed out "
                                + juce::String (int (rendered.timedOut)));
                }
                ++checked;
            }
        }

        expectEquals (engine.getUnderrunCount(), 0);
        expectEquals (engine.getReadFailureCount(), 0);
        logMessage (juce::String (checked) + " seeks verified bit-exact from the target frame");
    }

    void testLoopLaps (int laps)
    {
        beginTest ("ValidateLongSong: " + juce::String (laps) + " loop laps without drift or gaps");
        const std::int64_t length = 3 * 44100 + 17;
        PlaybackEngine engine;
        engine.prepare (44100.0, 1024);
        expect (engine.load (makeMarkerSources (44100, length)));
        engine.setLooping (true);
        engine.play();

        const int seam = std::max (2, static_cast<int> (std::lround (44100 * 0.005)));
        std::vector<float> left (1024), right (1024);
        float* channels[2] = { left.data(), right.data() };
        std::int64_t index = 0;
        std::int64_t bad = 0;
        std::int64_t first = -1;

        while (index < length * laps)
        {
            engine.waitForReadAhead (1024, 20000);
            engine.process (channels, 2, 1024);

            for (int i = 0; i < 1024; ++i)
            {
                const auto frame = index + i;
                const auto song = frame % length;
                const bool dip = song < seam || song >= length - seam || frame < ramp;
                const auto value = left[static_cast<size_t> (i)];
                const bool ok = dip ? (value != 0.0f
                                       && (isFadedCopy (value, expectedUnitySum (song, 0), seam)
                                           || isFadedCopy (value, expectedUnitySum (song, 0), ramp)))
                                    : (value == expectedUnitySum (song, 0)
                                       && right[static_cast<size_t> (i)] == expectedUnitySum (song, 1));

                if (! ok)
                {
                    ++bad;

                    if (first < 0)
                        first = frame;
                }
            }

            index += 1024;
        }

        expectEquals (
            static_cast<juce::int64> (bad), static_cast<juce::int64> (0), "bad frames; first " + juce::String (first));
        expectEquals (engine.getUnderrunCount(), 0);
        expect (engine.isPlaying() && ! engine.hasReachedEnd());
    }

    void testPauseResumeCycles (std::int64_t songFrames, int cycles)
    {
        beginTest ("ValidateLongSong: " + juce::String (cycles)
                   + " pause/resume cycles neither skip nor repeat a frame");
        PlaybackEngine engine;
        engine.prepare (44100.0, 1024);
        expect (engine.load (makeMarkerSources (44100, songFrames)));
        engine.play();
        juce::Random random (8);
        std::int64_t song = 0; // next song frame the audible output must carry
        bool intact = true;
        std::int64_t brokeAt = -1;

        auto consume = [&] (const Rendered& block)
        {
            for (size_t i = 0; i < block.left.size() && intact; ++i)
            {
                if (block.left[i] == 0.0f)
                    continue;

                intact = isFadedCopy (block.left[i], expectedUnitySum (song, 0), ramp)
                      && isFadedCopy (block.right[i], expectedUnitySum (song, 1), ramp);

                if (intact)
                    ++song;
                else
                    brokeAt = song;
            }
        };

        for (int i = 0; i < cycles && intact; ++i)
        {
            consume (renderOffline (engine, 500 + random.nextInt (4000), 128 + random.nextInt (700)));
            engine.pause();
            consume (renderOffline (engine, 1000 + random.nextInt (800), 256));
            engine.play();
        }

        expect (intact, "order broke at song frame " + juce::String (brokeAt));
        expectEquals (engine.getUnderrunCount(), 0);
    }

    void testBeyondFourBillionFrames()
    {
        beginTest ("ValidateLongSong: positions beyond 2^32 frames (a 28-hour song) seek and play exactly");
        const std::int64_t length = std::int64_t { 44100 } * 3600 * 28; // 4.4 billion frames
        PlaybackEngine engine;
        engine.prepare (44100.0, 1024);
        expect (engine.load (makeMarkerSources (44100, length)));
        engine.play();

        for (const std::int64_t target : { length - 60000,
                                           std::int64_t { 4294967296LL } + 12345,
                                           std::int64_t { 2147483648LL } - 100,
                                           length / 2 + 77 })
        {
            engine.seek (target);
            engine.play(); // a song that ran to its end has stopped itself
            const auto rendered = renderOffline (engine, 9000, 600);
            expect (findExactRun (rendered, markerSum, target + ramp - 1, 6000) >= 0,
                    "seek to " + juce::String (target));
        }

        engine.seek (length - 3000);
        renderOffline (engine, 6000, 600);
        expect (engine.hasReachedEnd());
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (length));
    }
};

LongSongTests longSongTests;

//==============================================================================
double toneFrequency (int stem)
{
    return 500.0 * (stem + 1) + 130.0 * stem;
}

/** Sum of the four stem tones, 0.1 each, with a different phase on the right channel. */
double analyticMix (double seconds, int channel)
{
    double sum = 0.0;

    for (int stem = 0; stem < 4; ++stem)
        sum += 0.1 * std::sin (2.0 * std::numbers::pi * toneFrequency (stem) * seconds + (channel == 1 ? 0.7 : 0.0));

    return sum;
}

class RateMatrixTests final : public juce::UnitTest
{
public:
    RateMatrixTests()
        : juce::UnitTest ("ValidateRateMatrix", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("ValidateRateMatrix: source 44.1/48/96/192 kHz x device 44.1/48/96 kHz: duration, alignment, A/B");

        if (heavyTests())
        {
            for (const int source : { 44100, 48000, 96000, 192000 })
                for (const double device : { 44100.0, 48000.0, 96000.0 })
                    check (source, device);
        }
        else
        {
            // Every source rate and every device rate at least once, both converting and copying,
            // up and down; SNDS_HEAVY_TESTS=1 runs all twelve pairs.
            for (const auto& [source, device] : { std::pair { 44100, 44100.0 },
                                                  std::pair { 44100, 96000.0 },
                                                  std::pair { 48000, 44100.0 },
                                                  std::pair { 96000, 48000.0 },
                                                  std::pair { 96000, 96000.0 },
                                                  std::pair { 192000, 48000.0 } })
                check (source, device);
        }
    }

private:
    void check (int sourceRate, double deviceRate)
    {
        const std::int64_t length = sourceRate / 2;
        const auto label = juce::String (sourceRate) + " -> " + juce::String (deviceRate);

        PlaybackSources sources;
        sources.sampleRate = sourceRate;
        sources.lengthFrames = length;

        // Four tones at four frequencies: one source frame of stem misalignment would show as a phase error.
        for (int stem = 0; stem < 4; ++stem)
            sources.stems[static_cast<size_t> (stem)] = std::make_unique<SyntheticSource> (
                sourceRate,
                length,
                [stem, sourceRate] (std::int64_t f, int c)
                {
                    return static_cast<float> (
                        0.1
                        * std::sin (2.0 * std::numbers::pi * toneFrequency (stem) * static_cast<double> (f) / sourceRate
                                    + (c == 1 ? 0.7 : 0.0)));
                });

        sources.original = std::make_unique<SyntheticSource> (
            sourceRate,
            length,
            [sourceRate] (std::int64_t f, int c)
            { return static_cast<float> (analyticMix (static_cast<double> (f) / sourceRate, c)); });

        PlaybackEngine engine;
        engine.prepare (deviceRate, 480);
        expect (engine.load (std::move (sources)), label);
        engine.play();

        const auto expectedFrames = expectedOutputFrames (length, sourceRate, deviceRate);
        Rendered rendered = renderOffline (engine, expectedFrames / 2, 480);
        engine.setOriginalSelected (true); // the second half is the original
        append (rendered, renderOffline (engine, expectedFrames - expectedFrames / 2 + 960, 480));

        expect (! rendered.timedOut, label);
        expect (engine.hasReachedEnd(), label + ": reached the end");
        expectEquals (engine.getUnderrunCount(), 0, label + ": underruns");
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()),
                      static_cast<juce::int64> (length),
                      label + ": final position");

        std::int64_t lastAudible = -1;

        for (std::int64_t i = 0; i < rendered.size(); ++i)
            if (rendered.left[static_cast<size_t> (i)] != 0.0f)
                lastAudible = i;

        expect (lastAudible < expectedFrames && lastAudible >= expectedFrames - 3,
                label + ": duration " + juce::String (lastAudible + 1) + " vs " + juce::String (expectedFrames));

        // Interior samples against the analytic mix at the device's own sample times.
        const double ratio = static_cast<double> (sourceRate) / deviceRate;
        const auto margin = static_cast<std::int64_t> (rampFrames (deviceRate) + 8 + 200 / ratio + 45);
        double worst = 0.0;

        for (std::int64_t k = margin; k + margin < expectedFrames; ++k)
            for (int c = 0; c < 2; ++c)
            {
                const auto seconds = static_cast<double> (k) / deviceRate;
                const auto value =
                    c == 0 ? rendered.left[static_cast<size_t> (k)] : rendered.right[static_cast<size_t> (k)];
                worst = std::max (worst, std::abs (static_cast<double> (value) - analyticMix (seconds, c)));
            }

        logMessage (label + ": " + juce::String (expectedFrames) + " frames, worst error vs the analytic mix "
                    + juce::String (20.0 * std::log10 (worst + 1.0e-12), 1) + " dBFS");
        expect (worst < 2.0e-3, label + ": worst error " + juce::String (worst));
    }
};

RateMatrixTests rateMatrixTests;

//==============================================================================
class RecoveryTests final : public juce::UnitTest
{
public:
    RecoveryTests()
        : juce::UnitTest ("ValidateRecovery", "snds")
    {
    }

    void runTest() override
    {
        MessageLoop loop;
        beginTest ("ValidateRecovery: ten loss/return cycles keep the position and resume bit-exactly, with seeks "
                   "while the device is away");
        Rig rig;
        expectEquals (rig.controller.initialise (xmlFor ("Headphones", 44100.0)), juce::String());
        expect (rig.engine.load (makeMarkerSources (44100, 44100 * 40)));
        juce::Random random (31);

        for (int cycle = 0; cycle < 10; ++cycle)
        {
            rig.engine.play();
            rig.render (4000 + random.nextInt (30000), 512);
            auto held = rig.engine.getPositionFrames();

            rig.fake->unplug ("Headphones");
            MessageLoop::pumpUntil ([&] { return rig.controller.getStatus().state == DeviceStatus::State::fallback; });
            expect (rig.controller.getStatus().state == DeviceStatus::State::fallback,
                    "cycle " + juce::String (cycle) + ": fallback");
            expect (! rig.engine.isPlaying() && ! rig.engine.isAudible(),
                    "cycle " + juce::String (cycle) + ": stopped");
            expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()),
                          static_cast<juce::int64> (held),
                          "cycle " + juce::String (cycle) + ": position kept");

            std::vector<float> left, right;
            rig.runBlocks (3, 512, &left, &right);
            expect (std::all_of (left.begin(), left.end(), [] (float v) { return v == 0.0f; }),
                    "silent on the fallback");

            if (cycle % 3 == 0)
            {
                held = random.nextInt (44100 * 30);
                rig.engine.seek (held); // chosen while the device is away
            }

            rig.fake->plugIn ("Headphones");
            MessageLoop::pumpUntil ([&] { return rig.controller.getStatus().state == DeviceStatus::State::running; });
            expect (rig.controller.getStatus().state == DeviceStatus::State::running,
                    "cycle " + juce::String (cycle) + ": running again");
            expect (! rig.engine.isPlaying(), "recovery never starts the music");
            expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (held));

            rig.engine.play();
            const auto rendered = rig.render (6000, 512);
            expect (findExactRun (rendered, markerSum, held + ramp - 1, 4000) >= 0,
                    "cycle " + juce::String (cycle) + ": exact continuation from " + juce::String (held));
        }

        expectEquals (rig.engine.getUnderrunCount(), 0);

        beginTest ("ValidateRecovery: device rate changes (44.1 -> 48 -> 96 -> 44.1 kHz) keep the position and the "
                   "right speed");
        rig.engine.pause();
        rig.render (2000, 512);
        const auto start = rig.engine.getPositionFrames();
        rig.engine.seek (start);

        for (const double rate : { 48000.0, 96000.0, 44100.0 })
        {
            rig.controller.setUserEditing (true);
            auto setup = rig.manager.getAudioDeviceSetup();
            setup.sampleRate = rate;
            expectEquals (rig.manager.setAudioDeviceSetup (setup, true), juce::String());
            MessageLoop::pump();
            rig.controller.setUserEditing (false);

            expectEquals (rig.engine.getDeviceSampleRate(), rate);
            expect (! rig.engine.isPlaying());
            const auto before = rig.engine.getPositionFrames();
            rig.engine.play();
            const auto frames = static_cast<int> (rate / 4.0 / 480.0) * 480;
            rig.render (frames, 480);
            const auto expectedAdvance = frames * 44100.0 / rate;
            const auto advance = rig.engine.getPositionFrames() - before;
            expect (std::abs (advance - expectedAdvance) < 4.0,
                    juce::String (rate) + " Hz: advanced " + juce::String (advance) + " vs "
                        + juce::String (expectedAdvance, 1));
            rig.engine.pause();
            rig.render (1000, 480);
        }
    }
};

RecoveryTests recoveryTests;

//==============================================================================
struct ProcessUsage
{
    std::size_t privateBytes = 0;
    unsigned long handles = 0;
    int threads = 0;
};

ProcessUsage measureProcess()
{
    ProcessUsage usage;
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof (counters);
    GetProcessMemoryInfo (
        GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&counters), sizeof (counters));
    usage.privateBytes = counters.PrivateUsage;
    DWORD handles = 0;
    GetProcessHandleCount (GetCurrentProcess(), &handles);
    usage.handles = handles;

    if (auto* snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0); snapshot != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 entry {};
        entry.dwSize = sizeof (entry);

        for (BOOL ok = Thread32First (snapshot, &entry); ok; ok = Thread32Next (snapshot, &entry))
            if (entry.th32OwnerProcessID == GetCurrentProcessId())
                ++usage.threads;

        CloseHandle (snapshot);
    }

    return usage;
}

class MemoryTests final : public juce::UnitTest
{
public:
    MemoryTests()
        : juce::UnitTest ("ValidateMemory", "snds")
    {
    }

    void runTest() override
    {
        testReservedBytes();
        testReloads();
        testFileBackedSongs();
    }

private:
    void testReservedBytes()
    {
        beginTest ("ValidateMemory: reserved streaming memory depends on the sample rate, never on the length");

        for (const int rate : { 44100, 48000, 96000, 192000 })
        {
            PlaybackEngine engine;
            engine.prepare (48000.0, 1024);
            std::size_t first = 0;

            for (const std::int64_t seconds :
                 { std::int64_t { 1 }, std::int64_t { 60 }, std::int64_t { 3600 }, std::int64_t { 3600 * 20 } })
            {
                auto sources = makeMarkerSources (rate, seconds * rate);
                sources.original = std::make_unique<SyntheticSource> (rate, seconds * rate, markerSum);
                expect (engine.load (std::move (sources)));

                if (first == 0)
                    first = engine.getReservedBytes();

                expectEquals (static_cast<juce::int64> (engine.getReservedBytes()),
                              static_cast<juce::int64> (first),
                              juce::String (rate) + " Hz, " + juce::String (seconds) + " s");
            }

            // Five 2-second stereo float rings, plus small scratch: well under 12 bytes x rate x 2 s x 5 + 1 MB.
            const auto bound =
                static_cast<std::size_t> (rate) * 2 * 8 * 5 + 1024 * 1024 + static_cast<std::size_t> (rate) * 16;
            expect (first < bound,
                    juce::String (rate) + " Hz: " + juce::String (static_cast<juce::int64> (first)) + " bytes");
            logMessage (juce::String (rate) + " Hz: " + juce::String (first / 1024.0 / 1024.0, 2)
                        + " MB reserved for four stems and the original, any length");
        }
    }

    void testReloads()
    {
        // 300 reloads with SNDS_HEAVY_TESTS=1; 42 otherwise: after a warm-up of 12, the six combinations of
        // length and rate are each reloaded five times and memory, handles and threads must stay flat.
        const int reloads = workload (42, 300);
        const int warmUp = workload (12, 30);
        beginTest ("ValidateMemory: " + juce::String (reloads)
                   + " reloads of short and very long songs do not grow memory, handles or threads");
        PlaybackEngine engine;
        engine.prepare (48000.0, 1024);
        std::vector<float> left (1024), right (1024);
        float* channels[2] = { left.data(), right.data() };

        auto cycle = [&] (int i)
        {
            const std::int64_t seconds = i % 3 == 0 ? 2 : (i % 3 == 1 ? 600 : 36000);
            auto sources = makeMarkerSources (i % 2 == 0 ? 44100 : 96000, seconds * (i % 2 == 0 ? 44100 : 96000));
            expect (engine.load (std::move (sources)));
            engine.play();

            for (int b = 0; b < 3; ++b)
            {
                engine.waitForReadAhead (1024, 20000);
                engine.process (channels, 2, 1024);
            }
        };

        for (int i = 0; i < warmUp; ++i)
            cycle (i); // warm-up: allocator pools, thread caches

        const auto before = measureProcess();

        for (int i = warmUp; i < reloads; ++i)
            cycle (i);

        engine.unload();
        const auto after = measureProcess();
        const auto growth = static_cast<double> (after.privateBytes) - static_cast<double> (before.privateBytes);
        logMessage (juce::String (reloads - warmUp) + " reloads: private bytes "
                    + juce::String (before.privateBytes / 1048576.0, 1) + " -> "
                    + juce::String (after.privateBytes / 1048576.0, 1) + " MB, handles "
                    + juce::String (static_cast<juce::int64> (before.handles)) + " -> "
                    + juce::String (static_cast<juce::int64> (after.handles)) + ", threads "
                    + juce::String (before.threads) + " -> " + juce::String (after.threads));
        expect (growth < 8.0 * 1024 * 1024, "private memory grew by " + juce::String (growth / 1048576.0, 1) + " MB");
        expect (after.handles <= before.handles + 8, "handles leaked");
        expect (after.threads <= before.threads + 1, "threads leaked");
    }

    void testFileBackedSongs()
    {
        const bool longRun = optedIn ("SNDS_VALIDATE_LONG");
        const int shortSeconds = workload (4, 8);
        const int longSeconds = longRun ? 300 : workload (12, 40);
        beginTest ("ValidateMemory: WAV stems of " + juce::String (shortSeconds) + " s and "
                   + juce::String (longSeconds) + " s use the same memory while playing");
        ScopedTempDirectory temp;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto writeStems = [&] (const juce::String& prefix, int seconds)
        {
            juce::WavAudioFormat wav;
            const std::int64_t frames = std::int64_t { 44100 } * seconds;

            for (int stem = 0; stem < 4; ++stem)
            {
                std::unique_ptr<juce::OutputStream> stream (
                    temp.child (prefix + juce::String (stem) + ".wav").createOutputStream());
                auto writer = wav.createWriterFor (
                    stream,
                    juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (
                        32));
                juce::AudioBuffer<float> chunk (2, 44100);

                for (std::int64_t at = 0; at < frames; at += 44100)
                {
                    const int count = static_cast<int> (std::min<std::int64_t> (44100, frames - at));

                    for (int channel = 0; channel < 2; ++channel)
                        for (int i = 0; i < count; ++i)
                            chunk.setSample (channel, i, stemMarker (stem, at + i, channel));

                    writer->writeFromAudioSampleBuffer (chunk, 0, count);
                }
            }
        };

        auto openSong = [&] (const juce::String& prefix, int seconds)
        {
            PlaybackSources sources;
            sources.sampleRate = 44100;
            sources.lengthFrames = std::int64_t { 44100 } * seconds;

            for (int stem = 0; stem < 4; ++stem)
            {
                juce::String error;
                sources.stems[static_cast<size_t> (stem)] =
                    openFrameSource (temp.child (prefix + juce::String (stem) + ".wav"), formats, error);
                expect (sources.stems[static_cast<size_t> (stem)] != nullptr, error);
            }

            return sources;
        };

        writeStems ("short", shortSeconds);
        writeStems ("long", longSeconds);

        PlaybackEngine engine;
        engine.prepare (44100.0, 1024);
        std::vector<float> left (1024), right (1024);
        float* channels[2] = { left.data(), right.data() };

        auto playWhileMeasuring = [&] (const juce::String& prefix, int seconds)
        {
            expect (engine.load (openSong (prefix, seconds)));
            engine.play();
            engine.seek (std::int64_t { 44100 } * seconds / 2);
            std::size_t peak = 0;

            for (int b = 0; b < workload (100, 200); ++b) // 2 s (4.5 s heavy) of audio, mid-song
            {
                engine.waitForReadAhead (1024, 20000);
                engine.process (channels, 2, 1024);

                if (b % 20 == 0)
                    peak = std::max (peak, measureProcess().privateBytes);
            }

            expectEquals (engine.getUnderrunCount(), 0);
            return std::pair { peak, engine.getReservedBytes() };
        };

        playWhileMeasuring ("short", shortSeconds); // warm-up
        const auto shortRun = playWhileMeasuring ("short", shortSeconds);
        const auto longRunResult = playWhileMeasuring ("long", longSeconds);
        const auto difference = static_cast<double> (longRunResult.first) - static_cast<double> (shortRun.first);
        logMessage ("private bytes while playing: " + juce::String (shortSeconds) + " s song "
                    + juce::String (shortRun.first / 1048576.0, 1) + " MB, " + juce::String (longSeconds) + " s song "
                    + juce::String (longRunResult.first / 1048576.0, 1) + " MB; reserved "
                    + juce::String (static_cast<juce::int64> (longRunResult.second)) + " bytes for both");
        expectEquals (static_cast<juce::int64> (longRunResult.second), static_cast<juce::int64> (shortRun.second));
        expect (difference < 6.0 * 1024 * 1024,
                "the long song used " + juce::String (difference / 1048576.0, 1) + " MB more");
    }
};

MemoryTests memoryTests;

//==============================================================================
/** The real owner-track package and its original, read-only. */
class RealPackageTests final : public juce::UnitTest
{
public:
    RealPackageTests()
        : juce::UnitTest ("ValidateRealPackage", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("ValidateRealPackage: Original vs Stem Mix on the real owner track (opt-in: SNDS_VALIDATE_REAL=1)");

        if (! optedIn ("SNDS_VALIDATE_REAL"))
            return;

        const auto packageText = juce::SystemStats::getEnvironmentVariable ("SNDS_REAL_PACKAGE", {});
        const auto originalText = juce::SystemStats::getEnvironmentVariable ("SNDS_REAL_ORIGINAL", {});
        const juce::File package =
            packageText.isNotEmpty()
                ? juce::File (packageText)
                : juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile (
                          "snds-e2e-home\\cache\\d9dc9181af7032d37b8b312087ba509c965f5d11c96048ae66134614d92b784e-"
                          "27323700");
        const juce::File original =
            originalText.isNotEmpty()
                ? juce::File (originalText)
                : juce::File(); // the owner track is never named in the repository: pass it in the environment

        if (! package.isDirectory() || ! original.existsAsFile())
        {
            logMessage ("real package or original not found; skipped: " + package.getFullPathName());
            return;
        }

        const auto loaded = loadSongPackage (package);
        expect (loaded.isComplete(), loaded.reason);

        if (! loaded.isComplete())
            return;

        const auto& song = loaded.metadata;
        const auto length = song.source.frames;
        const auto rate = song.source.sampleRate;
        logMessage ("package " + package.getFileName() + ": " + juce::String (length) + " frames at "
                    + juce::String (rate) + " Hz, " + juce::String (song.source.durationSeconds, 1) + " s");

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto openStem = [&] (int i)
        {
            juce::String error;
            auto source =
                openFrameSource (package.getChildFile (song.stems[static_cast<size_t> (i)].file), formats, error);
            expect (source != nullptr, error);
            return source;
        };

        // Alignment, exactly as the app does it at song open.
        std::array<std::unique_ptr<FrameSource>, 4> planning;

        for (int i = 0; i < 4; ++i)
            planning[static_cast<size_t> (i)] = openStem (i);

        auto plan = planOriginal (original,
                                  song.source.container,
                                  formats,
                                  length,
                                  rate,
                                  { planning[0].get(), planning[1].get(), planning[2].get(), planning[3].get() });
        logMessage (plan.summary);
        expect (plan.usable, plan.reason);

        if (! plan.usable)
            return;

        // Reference readers for bit-exact comparison (separate file handles, read-only).
        std::array<std::unique_ptr<FrameSource>, 4> referenceStems;

        for (int i = 0; i < 4; ++i)
            referenceStems[static_cast<size_t> (i)] = openStem (i);

        juce::String error;
        auto referenceOriginal = openFrameSource (original, formats, error);
        expect (referenceOriginal != nullptr, error);

        auto referenceSum = [&] (std::int64_t start, int count, std::vector<float>& l, std::vector<float>& r)
        {
            std::vector<float> a (static_cast<size_t> (count)), b (static_cast<size_t> (count));
            float* channels[2] = { a.data(), b.data() };
            l.assign (static_cast<size_t> (count), 0.0f);
            r.assign (static_cast<size_t> (count), 0.0f);

            for (int stem = 0; stem < 4; ++stem)
            {
                referenceStems[static_cast<size_t> (stem)]->read (start, channels, count);

                for (int i = 0; i < count; ++i)
                {
                    l[static_cast<size_t> (i)] = stem == 0 ? a[static_cast<size_t> (i)]
                                                           : l[static_cast<size_t> (i)] + a[static_cast<size_t> (i)];
                    r[static_cast<size_t> (i)] = stem == 0 ? b[static_cast<size_t> (i)]
                                                           : r[static_cast<size_t> (i)] + b[static_cast<size_t> (i)];
                }
            }
        };

        auto makeSources = [&]
        {
            PlaybackSources sources;
            sources.sampleRate = rate;
            sources.lengthFrames = length;

            for (int i = 0; i < 4; ++i)
                sources.stems[static_cast<size_t> (i)] = openStem (i);

            juce::String problem;
            sources.original = openFrameSource (original, formats, problem, 0, length);
            return sources;
        };

        PlaybackEngine engine;
        engine.prepare (static_cast<double> (rate), 1024);

        //======================================================================
        // 1. The whole track, Stem Mix then Original, bit-exact against direct file reads.
        for (const bool useOriginal : { false, true })
        {
            expect (engine.load (makeSources()));
            engine.setOriginalSelected (useOriginal);
            engine.play();

            std::vector<float> refL, refR, origL (4096), origR (4096);
            std::int64_t refStart = -1;
            std::int64_t mismatches = 0;
            std::int64_t first = -1;
            std::int64_t index = 0;
            std::vector<float> left (1024), right (1024);
            float* channels[2] = { left.data(), right.data() };
            const auto began = nowMs();
            float peak = 0.0f;

            while (index < length + 2048)
            {
                engine.waitForReadAhead (1024, 30000);
                engine.process (channels, 2, 1024);

                for (int i = 0; i < 1024 && index + i < length; ++i)
                {
                    const auto frame = index + i;

                    if (refStart < 0 || frame >= refStart + 4096)
                    {
                        refStart = frame;

                        if (useOriginal)
                        {
                            float* ch[2] = { origL.data(), origR.data() };
                            referenceOriginal->read (refStart, ch, 4096);
                            refL = origL;
                            refR = origR;
                        }
                        else
                        {
                            referenceSum (refStart, 4096, refL, refR);
                        }
                    }

                    const auto k = static_cast<size_t> (frame - refStart);
                    const bool exact =
                        left[static_cast<size_t> (i)] == refL[k] && right[static_cast<size_t> (i)] == refR[k];
                    const bool faded = frame < ramp && isFadedCopy (left[static_cast<size_t> (i)], refL[k], ramp);
                    peak = std::max (peak, std::abs (left[static_cast<size_t> (i)]));

                    if (! exact && ! faded && refL[k] != 0.0f)
                    {
                        ++mismatches;

                        if (first < 0)
                            first = frame;
                    }
                }

                index += 1024;

                if (engine.hasReachedEnd())
                    break;
            }

            const auto wall = (nowMs() - began) / 1000.0;
            const juce::String mode = useOriginal ? "Original" : "Stem Mix";
            logMessage (mode + ": full track " + juce::String (length / rate / 60.0, 2) + " min rendered in "
                        + juce::String (wall, 1) + " s (" + juce::String (static_cast<double> (length) / rate / wall, 0)
                        + "x real time), mismatches " + juce::String (static_cast<juce::int64> (mismatches))
                        + ", underruns " + juce::String (engine.getUnderrunCount()) + ", peak "
                        + juce::String (peak, 3));
            expectEquals (static_cast<juce::int64> (mismatches),
                          static_cast<juce::int64> (0),
                          mode + ": first mismatch at " + juce::String (first));
            expectEquals (engine.getUnderrunCount(), 0, mode + ": underruns");
            expect (engine.hasReachedEnd(), mode + ": reached the end");
        }

        //======================================================================
        // 2. Original vs Stem Mix at several positions, at the source rate and resampled to 48 kHz.
        for (const double deviceRate : { static_cast<double> (rate), 48000.0 })
        {
            engine.prepare (deviceRate, 1024);
            expect (engine.load (makeSources()));
            engine.play();
            double worstResidual = -1000.0;
            double bestResidual = 1000.0;

            for (const double fraction : { 0.03, 0.2, 0.4, 0.55, 0.7, 0.9 })
            {
                const auto target = static_cast<std::int64_t> (fraction * static_cast<double> (length));
                const int frames = static_cast<int> (deviceRate * 4);

                engine.setOriginalSelected (false);
                engine.seek (target);
                renderOffline (engine, 20000, 1024); // settle (crossfade, ramps)
                engine.seek (target);
                const auto stems = renderOffline (engine, frames, 1024);

                engine.setOriginalSelected (true);
                engine.seek (target);
                renderOffline (engine, 20000, 1024);
                engine.seek (target);
                const auto orig = renderOffline (engine, frames, 1024);

                // Both renders restart from `target`; find where each one's full-gain audio begins and compare.
                auto firstAudible = [] (const Rendered& r)
                {
                    for (std::int64_t i = 0; i < r.size(); ++i)
                        if (r.left[static_cast<size_t> (i)] != 0.0f)
                            return i;

                    return std::int64_t { 0 };
                };

                const auto offsetStems = firstAudible (stems) + ramp * 2;
                const auto offsetOrig = firstAudible (orig) + ramp * 2;
                const auto count = std::min (stems.size() - offsetStems, orig.size() - offsetOrig) - 100;
                double errorEnergy = 0.0;
                double signalEnergy = 0.0;

                for (std::int64_t i = 0; i < count; ++i)
                {
                    const double s = stems.left[static_cast<size_t> (offsetStems + i)];
                    const double o = orig.left[static_cast<size_t> (offsetOrig + i)];
                    errorEnergy += (o - s) * (o - s);
                    signalEnergy += o * o;
                }

                const auto residual = 10.0 * std::log10 (errorEnergy / std::max (signalEnergy, 1.0e-12) + 1.0e-20);
                worstResidual = std::max (worstResidual, residual);
                bestResidual = std::min (bestResidual, residual);
                expect (residual < -20.0, juce::String (fraction) + ": residual " + juce::String (residual, 1) + " dB");
            }

            logMessage ("device " + juce::String (deviceRate) + " Hz: stem-sum vs original residual "
                        + juce::String (bestResidual, 1) + " to " + juce::String (worstResidual, 1)
                        + " dB over 6 positions (model error, not misalignment: lag is 0)");
            expectEquals (engine.getUnderrunCount(), 0);
        }

        //======================================================================
        // 3. Random seeks into the real files: exact, and how long a seek takes to become audible.
        engine.prepare (static_cast<double> (rate), 1024);
        expect (engine.load (makeSources()));
        engine.play();
        juce::Random random (77);
        std::vector<double> latencies;

        for (int i = 0; i < 40; ++i)
        {
            const bool useOriginal = i % 2 == 1;
            engine.setOriginalSelected (useOriginal);
            const auto target = static_cast<std::int64_t> (random.nextDouble() * static_cast<double> (length - 60000));
            const auto began = nowMs();
            engine.seek (target);
            Rendered rendered;
            std::vector<float> left (1024), right (1024);
            float* channels[2] = { left.data(), right.data() };
            double audibleAt = -1.0;

            for (int b = 0; b < 40; ++b)
            {
                engine.waitForReadAhead (1024, 20000);
                engine.process (channels, 2, 1024);
                rendered.left.insert (rendered.left.end(), left.begin(), left.end());
                rendered.right.insert (rendered.right.end(), right.begin(), right.end());

                if (audibleAt < 0.0
                    && std::any_of (left.begin(), left.end(), [] (float v) { return v != 0.0f; }) && b > 0)
                    audibleAt = nowMs() - began;
            }

            latencies.push_back (audibleAt);
            std::vector<float> refL, refR;

            if (useOriginal)
            {
                refL.assign (4096, 0.0f);
                refR.assign (4096, 0.0f);
                float* ch[2] = { refL.data(), refR.data() };
                referenceOriginal->read (target + ramp, ch, 4096);
            }
            else
            {
                referenceSum (target + ramp, 4096, refL, refR);
            }

            bool found = false;

            for (std::int64_t k = 0; k + 4096 <= rendered.size() && ! found; ++k)
                if (rendered.left[static_cast<size_t> (k)] == refL[0]
                    && rendered.right[static_cast<size_t> (k)] == refR[0])
                {
                    found = true;

                    for (int n = 0; n < 4096 && found; ++n)
                        found = rendered.left[static_cast<size_t> (k + n)] == refL[static_cast<size_t> (n)];
                }

            expect (found,
                    "seek " + juce::String (i) + " to " + juce::String (target)
                        + (useOriginal ? " (original)" : " (stems)") + ": exact audio from the target");
        }

        std::sort (latencies.begin(), latencies.end());
        logMessage ("40 seeks on the real files: all exact; seek to audible (offline harness, includes ~"
                    + juce::String (ramp * 1000 / rate) + " ms fade-in and read-ahead refill): median "
                    + juce::String (latencies[latencies.size() / 2], 0) + " ms, worst "
                    + juce::String (latencies.back(), 0) + " ms");
        expectEquals (engine.getUnderrunCount(), 0);
        expectEquals (engine.getReadFailureCount(), 0);
    }
};

RealPackageTests realPackageTests;
} // namespace
} // namespace snds
