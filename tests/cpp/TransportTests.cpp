#include <juce_core/juce_core.h>

#include "DspHeavy.h"
#include "PlaybackTestKit.h"
#include "audio/PlaybackEngine.h"

#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace snds
{
namespace
{
using namespace snds::audio;
using namespace snds::test;

constexpr std::int64_t rate = 44100;
const int ramp = rampFrames (44100.0);
const int seam = std::max (2, static_cast<int> (std::lround (44100 * 0.005)));

void append (Rendered& into, const Rendered& more)
{
    into.left.insert (into.left.end(), more.left.begin(), more.left.end());
    into.right.insert (into.right.end(), more.right.begin(), more.right.end());
    into.timedOut = into.timedOut || more.timedOut;
}

/** Index of the first output frame that starts a run of `runLength` frames equal to the song
    from `firstSongFrame` on, in both channels (exact: full gain, nothing skipped). -1 if none. */
std::int64_t findRun (const Rendered& rendered, std::int64_t firstSongFrame, int runLength, std::int64_t from = 0)
{
    for (auto i = from; i + runLength <= rendered.size(); ++i)
    {
        if (rendered.left[static_cast<size_t> (i)] != expectedUnitySum (firstSongFrame, 0))
            continue;

        bool all = true;

        for (int k = 0; k < runLength && all; ++k)
            all = rendered.left[static_cast<size_t> (i + k)] == expectedUnitySum (firstSongFrame + k, 0)
               && rendered.right[static_cast<size_t> (i + k)] == expectedUnitySum (firstSongFrame + k, 1);

        if (all)
            return i;
    }

    return -1;
}

float toneSample (std::int64_t frame, int sourceRate)
{
    return 0.2f
         * static_cast<float> (std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (frame) / sourceRate));
}

PlaybackSources makeToneSources (int sourceRate, std::int64_t frames)
{
    PlaybackSources sources;
    sources.sampleRate = sourceRate;
    sources.lengthFrames = frames;

    for (auto& stem : sources.stems)
        stem = std::make_unique<SyntheticSource> (
            sourceRate, frames, [sourceRate] (std::int64_t frame, int) { return toneSample (frame, sourceRate); });

    return sources;
}

double largestStep (const std::vector<float>& samples, std::size_t from = 0)
{
    double largest = 0.0;

    for (auto i = from + 1; i < samples.size(); ++i)
        largest = std::max (largest, std::abs (static_cast<double> (samples[i]) - samples[i - 1]));

    return largest;
}

class TransportTests final : public juce::UnitTest
{
public:
    TransportTests()
        : juce::UnitTest ("Transport", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        testSeekWhilePaused();
        testSeekNearEndAndEnd();
        testSeekWhilePlaying();
        testRepeatedSeeks();
        testPauseResume();
        testStop();
        testLoop();
        testLoopSeamIsClickFree();
        testSeeksAreClickFree();
        testConcurrentControl();
    }

private:
    static constexpr std::int64_t songLength = rate * 6 + 11;

    static bool loadMarkers (PlaybackEngine& engine, std::int64_t length = songLength, double deviceRate = 44100.0)
    {
        engine.prepare (deviceRate, 1024);
        return engine.load (makeMarkerSources (static_cast<int> (rate), length));
    }

    /** After seeking to `target` and playing, the first full-gain frame is song frame
        target + ramp - 1 and the audio then runs unbroken. */
    void expectCleanStartAt (const Rendered& rendered, std::int64_t target, const juce::String& what)
    {
        const auto start = findRun (rendered, target + ramp - 1, 2000);
        expect (start >= 0, what + ": a gap-free run from the target frame, all stems aligned");

        if (start < 0)
            return;

        // The frames before it fade the first `ramp - 1` frames of the target in.
        for (int k = 0; k < ramp - 1 && start - (ramp - 1) + k >= 0; ++k)
        {
            const auto i = static_cast<size_t> (start - (ramp - 1) + k);
            expect (isFadedCopy (rendered.left[i], expectedUnitySum (target + k, 0), ramp), what + ": fade-in frame");
        }
    }

    void testSeekWhilePaused()
    {
        beginTest ("Transport: seeking while paused repositions every stem; playback starts there with a fade-in");
        PlaybackEngine engine;
        expect (loadMarkers (engine));

        for (const std::int64_t target : { std::int64_t { 0 },
                                           std::int64_t { 1 },
                                           std::int64_t { 100000 },
                                           std::int64_t { 4096 * 7 },
                                           songLength - 20000 })
        {
            engine.seek (target);
            expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (target));
            expect (engine.isBuffering());
            engine.play();
            Rendered rendered = renderOffline (engine, 5000, 700);
            expect (! rendered.timedOut);
            expectCleanStartAt (rendered, target, "target " + juce::String (target));
            engine.pause();
            append (rendered, renderOffline (engine, 1000, 512));
            expect (! engine.isAudible());
        }

        expectEquals (engine.getUnderrunCount(), 0);
    }

    void testSeekNearEndAndEnd()
    {
        beginTest ("Transport: a seek near the end plays out the rest and stops; play restarts from 0");
        PlaybackEngine engine;
        expect (loadMarkers (engine, 3000));

        engine.seek (3000 - 100);
        engine.play();
        auto rendered = renderOffline (engine, 1000, 256);
        std::int64_t audibleFrames = 0;

        for (auto value : rendered.left)
            audibleFrames += value != 0.0f;

        expectEquals (static_cast<juce::int64> (audibleFrames), static_cast<juce::int64> (100));
        expect (engine.hasReachedEnd());
        expect (! engine.isPlaying(), "the transport stops itself at the end");
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (3000));

        engine.play(); // at the end: starts over
        rendered = renderOffline (engine, 2000, 512);
        expect (findRun (rendered, ramp - 1, 1500) >= 0, "restarted from the first frame");

        // A seek to the very end is allowed; the transport rests there until play() starts over.
        engine.pause();
        engine.seek (3000);
        renderOffline (engine, 1000, 512);
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (3000));
        expect (! engine.isAudible());
    }

    void testSeekWhilePlaying()
    {
        beginTest ("Transport: seeking while playing fades out, reseeks all streams, and resumes cleanly");
        PlaybackEngine engine;
        expect (loadMarkers (engine));
        engine.play();
        Rendered rendered = renderOffline (engine, 20000, 512);
        expect (findRun (rendered, ramp - 1, 5000) >= 0);

        juce::Random random (7);

        for (int round = 0; round < 12; ++round)
        {
            const auto target = static_cast<std::int64_t> (random.nextInt (static_cast<int> (songLength - 20000)));
            engine.seek (target);
            Rendered after = renderOffline (engine, 12000, 333 + round * 17);
            expect (! after.timedOut);
            expectCleanStartAt (after, target, "seek " + juce::String (round) + " to " + juce::String (target));
        }

        expectEquals (engine.getUnderrunCount(), 0);
    }

    void testRepeatedSeeks()
    {
        beginTest ("Transport: hundreds of rapid seeks never misalign the streams");
        PlaybackEngine engine;
        expect (loadMarkers (engine));
        engine.play();
        juce::Random random (11);

        for (int round = 0; round < workload (150, 400); ++round)
        {
            const int kind = random.nextInt (6);
            const auto target = kind == 0 ? std::int64_t { 0 }
                              : kind == 1 ? songLength - random.nextInt (200)
                                          : static_cast<std::int64_t> (random.nextInt (static_cast<int> (songLength)));
            engine.seek (target);
            const auto blocks = random.nextInt (4);

            for (int block = 0; block < blocks; ++block)
                expect (! renderOffline (engine, 1 + random.nextInt (900), 1 + random.nextInt (700)).timedOut);

            if (random.nextInt (9) == 0)
                engine.setPlaying (! engine.isPlaying());
        }

        engine.play();
        const auto target = std::int64_t { 123456 };
        engine.seek (target);
        const auto finalRender = renderOffline (engine, 8000, 480);
        expectCleanStartAt (finalRender, target, "after the barrage");
        expectEquals (engine.getReadFailureCount(), 0);
    }

    void testPauseResume()
    {
        beginTest ("Transport: pause and resume neither skip nor repeat a frame, and hold the position");
        PlaybackEngine engine;
        expect (loadMarkers (engine));
        engine.play();
        Rendered rendered;

        for (int round = 0; round < 6; ++round)
        {
            append (rendered, renderOffline (engine, 3000 + round * 211, 500));
            engine.pause();
            append (rendered, renderOffline (engine, 1500, 500));
            expect (! engine.isAudible());
            const auto held = engine.getPositionFrames();
            append (rendered, renderOffline (engine, 3000, 500));
            expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (held));
            engine.play();
        }

        append (rendered, renderOffline (engine, 4000, 500));

        // The audible frames, in order, are song frames 0, 1, 2, ... each at some fade gain.
        std::int64_t song = 0;
        bool intact = true;

        for (size_t i = 0; i < rendered.left.size() && intact; ++i)
        {
            if (rendered.left[i] == 0.0f)
                continue;

            intact = isFadedCopy (rendered.left[i], expectedUnitySum (song, 0), ramp)
                  && isFadedCopy (rendered.right[i], expectedUnitySum (song, 1), ramp);

            if (intact)
                ++song;
        }

        expect (intact, "frame " + juce::String (song) + " is where the order broke");
        expectEquals (engine.getUnderrunCount(), 0);
    }

    void testStop()
    {
        beginTest ("Transport: stop returns to the start and stays silent; play starts from there");
        PlaybackEngine engine;
        expect (loadMarkers (engine));
        engine.play();
        renderOffline (engine, 30000, 512);
        expect (engine.getPositionFrames() > 20000);

        engine.stop();
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (0));
        expectEquals (engine.getPositionSeconds(), 0.0);
        renderOffline (engine, 4000, 512);
        expect (! engine.isAudible() && ! engine.isPlaying());
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (0));

        engine.play();
        const auto rendered = renderOffline (engine, 4000, 512);
        expectCleanStartAt (rendered, 0, "after stop");
        expectWithinAbsoluteError (engine.getDurationSeconds(), static_cast<double> (songLength) / rate, 1.0e-9);
    }

    void testLoop()
    {
        beginTest ("Transport: a looping song wraps at the producer with a short dip and no gap");
        const std::int64_t length = 20037;
        PlaybackEngine engine;
        expect (loadMarkers (engine, length));
        engine.setLooping (true);
        engine.play();
        const auto frames = length * 7 / 2;
        auto rendered = renderOffline (engine, frames, 700);
        expect (! rendered.timedOut);
        expect (engine.isPlaying() && ! engine.hasReachedEnd());
        expectEquals (engine.getUnderrunCount(), 0);

        bool intact = true;
        std::int64_t firstBad = -1;

        for (std::int64_t i = 0; i < frames && intact; ++i)
        {
            const auto song = i % length;
            const auto value = rendered.left[static_cast<size_t> (i)];
            const bool nearSeam = song < seam || song >= length - seam;
            const bool inRamp = i < ramp;
            const auto expected = expectedUnitySum (song, 0);

            if (nearSeam || inRamp)
                intact = value != 0.0f && (isFadedCopy (value, expected, seam) || isFadedCopy (value, expected, ramp));
            else
                intact = value == expected && rendered.right[static_cast<size_t> (i)] == expectedUnitySum (song, 1);

            if (! intact)
                firstBad = i;
        }

        expect (intact,
                "output frame " + juce::String (firstBad) + " is not song frame " + juce::String (firstBad % length));
        const auto position = engine.getPositionFrames();
        expect (position >= 0 && position < length);

        beginTest ("Transport: turning the loop off lets the current lap finish");
        engine.setLooping (false);
        append (rendered, renderOffline (engine, length * 2, 700));
        expect (engine.hasReachedEnd() && ! engine.isPlaying());
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (length));
    }

    void testLoopSeamIsClickFree()
    {
        beginTest ("Transport: the loop seam of a resampled tone has no click");
        // A length that is not a whole number of tone periods: the seam jumps in phase.
        const std::int64_t length = rate / 2 + 37;
        PlaybackEngine engine;
        engine.prepare (48000.0, 512);
        expect (engine.load (makeToneSources (static_cast<int> (rate), length)));
        engine.setLooping (true);
        engine.play();
        const auto rendered = renderOffline (engine, 48000 * 3, 480);
        expect (! rendered.timedOut);
        const auto step = largestStep (rendered.left, 100);
        // A 0.8 amplitude 1 kHz tone at 48 kHz moves at most 0.105 per sample.
        logMessage ("largest sample step across 5 loop seams: " + juce::String (step, 4));
        expect (step < 0.12, "largest step " + juce::String (step));
    }

    void testSeeksAreClickFree()
    {
        beginTest ("Transport: seek, pause and resume of a tone never step more than the tone itself");
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (makeToneSources (static_cast<int> (rate), rate * 8)));
        engine.play();
        Rendered rendered = renderOffline (engine, 6000, 512);
        juce::Random random (3);

        for (int round = 0; round < workload (20, 40); ++round)
        {
            if (round % 5 == 4)
            {
                engine.pause();
                append (rendered, renderOffline (engine, 700, 100));
                engine.play();
            }

            engine.seek (random.nextInt (static_cast<int> (rate * 7)));
            append (rendered, renderOffline (engine, 1500 + random.nextInt (4000), 128 + random.nextInt (512)));
        }

        const auto step = largestStep (rendered.left);
        logMessage ("largest sample step over the seeks and pauses: " + juce::String (step, 4));
        expect (step < 0.12, "largest step " + juce::String (step));
        expectEquals (engine.getUnderrunCount(), 0);
    }

    void testConcurrentControl()
    {
        beginTest ("Transport: control calls racing a running audio callback leave a consistent engine");
        PlaybackEngine engine;
        expect (loadMarkers (engine));
        engine.play();
        std::atomic<bool> stop { false };

        std::thread audio (
            [&]
            {
                std::vector<float> left (256), right (256);
                float* channels[2] = { left.data(), right.data() };

                while (! stop.load())
                {
                    engine.process (channels, 2, 256);
                    juce::Thread::sleep (0);
                }
            });

        juce::Random random (5);
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + workload (600.0, 1500.0);

        while (juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            switch (random.nextInt (5))
            {
                case 0:
                    engine.seek (random.nextInt (static_cast<int> (songLength)));
                    break;
                case 1:
                    engine.pause();
                    break;
                case 2:
                    engine.play();
                    break;
                case 3:
                    engine.setLooping (random.nextBool());
                    break;
                default:
                    engine.seek (songLength - random.nextInt (50));
                    break;
            }

            juce::Thread::sleep (static_cast<juce::uint32> (random.nextInt (4)));
        }

        stop = true;
        audio.join();

        // Single-threaded again: the engine must still play any position correctly.
        engine.setLooping (false);
        engine.pause();
        renderOffline (engine, 2000, 256);
        engine.seek (200000);
        engine.play();
        const auto rendered = renderOffline (engine, 8000, 480);
        expectCleanStartAt (rendered, 200000, "after the race");
    }
};

TransportTests transportTests;
} // namespace
} // namespace snds
