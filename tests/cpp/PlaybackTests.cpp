#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "DspHeavy.h"
#include "PlaybackTestKit.h"
#include "TestHelpers.h"
#include "audio/FrameSource.h"
#include "audio/PlaybackEngine.h"
#include "audio/PrefetchStream.h"
#include "audio/Resampler.h"
#include "audio/SeekTarget.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace snds
{
namespace
{
using namespace snds::audio;
using namespace snds::test;

constexpr double pi = std::numbers::pi;

float tone (double frequency, double rate, std::int64_t frame)
{
    return static_cast<float> (0.5 * std::sin (2.0 * pi * frequency * static_cast<double> (frame) / rate));
}

/** Pushes `input` through a Resampler in `pushSize` blocks, pulling `pullSize` at a time. */
std::vector<float> resampleMono (Resampler& resampler, const std::vector<float>& input, int pushSize, int pullSize)
{
    std::vector<float> output;
    std::vector<float> pulledLeft (static_cast<size_t> (pullSize)), pulledRight (static_cast<size_t> (pullSize));
    std::size_t pushed = 0;

    for (;;)
    {
        float* destination[2] = { pulledLeft.data(), pulledRight.data() };
        const auto count = resampler.pull (destination, pullSize);
        output.insert (output.end(), pulledLeft.begin(), pulledLeft.begin() + count);

        if (count == pullSize)
            continue;

        if (pushed >= input.size())
            break;

        const auto block =
            static_cast<int> (std::min<std::size_t> (static_cast<size_t> (pushSize), input.size() - pushed));
        const float* source[2] = { input.data() + pushed, input.data() + pushed };
        resampler.push (source, block);
        pushed += static_cast<size_t> (block);
    }

    return output;
}

double rms (const std::vector<float>& values, std::size_t from, std::size_t to)
{
    double sum = 0.0;

    for (auto i = from; i < to; ++i)
        sum += static_cast<double> (values[i]) * values[i];

    return std::sqrt (sum / static_cast<double> (to - from));
}

//==============================================================================
class StreamingTests final : public juce::UnitTest
{
public:
    StreamingTests()
        : juce::UnitTest ("Streaming", "snds")
    {
    }

    void runTest() override
    {
        testSeekTarget();
        testPrefetchOrder();
        testPrefetchSeekFlush();
        testBoundedMemory();
        testReaderFrameSource();
    }

private:
    static std::unique_ptr<FrameSource> makeRamp (int rate, std::int64_t frames, int delayMs = 0)
    {
        return std::make_unique<SyntheticSource> (
            rate,
            frames,
            [] (std::int64_t frame, int channel) { return static_cast<float> (frame * 2 + channel); },
            delayMs);
    }

    void testSeekTarget()
    {
        beginTest ("Streaming: the seek target packs a generation and a 40-bit position");
        SeekTarget target (1234);
        expect (target.load() == SeekTarget::Value { 0, 1234 });
        const auto next = target.publish (SeekTarget::maxFrame);
        expect (next == SeekTarget::Value { 1, SeekTarget::maxFrame });
        expect (target.load() == next);

        for (int i = 0; i < (1 << 24); i += 1 << 20)
            target.publish (i); // wraps without disturbing the position

        expect (target.load().frame < (1 << 24));
    }

    void testPrefetchOrder()
    {
        beginTest ("Streaming: frames arrive in order, whatever the read size");
        const std::int64_t length = 60000;
        SeekTarget seek;
        PrefetchStream stream (makeRamp (44100, length), seek, length, 8192, 1024);
        PrefetchThread thread ("test prefetch");
        thread.add (stream);
        thread.start();

        std::vector<float> left (1500), right (1500);
        float* channels[2] = { left.data(), right.data() };
        const int sizes[] = { 1, 7, 500, 1024, 1500, 3 };
        std::int64_t position = 0;
        int step = 0;
        bool ordered = true;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 20000;

        while (position < length && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            const auto want = static_cast<int> (std::min<std::int64_t> (sizes[step++ % 6], length - position));

            if (stream.available (0) < want)
            {
                thread.notify();
                juce::Thread::sleep (1);
                continue;
            }

            expect (stream.read (0, channels, want));

            for (int i = 0; i < want && ordered; ++i)
                ordered = left[static_cast<size_t> (i)] == static_cast<float> ((position + i) * 2)
                       && right[static_cast<size_t> (i)] == static_cast<float> ((position + i) * 2 + 1);

            position += want;
        }

        expectEquals (position, length);
        expect (ordered, "every frame matched the source");
        expect (stream.hasReachedLimit (0));
        expectEquals (stream.getReadFailures(), 0);
    }

    void testPrefetchSeekFlush()
    {
        beginTest ("Streaming: a seek flushes the ring; no stale frame is ever returned");
        const std::int64_t length = 200000;
        SeekTarget seek;
        PrefetchStream stream (makeRamp (44100, length), seek, length, 8192, 1024);
        PrefetchThread thread ("test prefetch");
        thread.add (stream);
        thread.start();

        std::vector<float> left (512), right (512);
        float* channels[2] = { left.data(), right.data() };
        std::int64_t position = 0;
        std::uint32_t generation = 0;
        int seeks = 0;
        bool correct = true;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000;

        const std::int64_t targets[] = { 150000, 3, 199900, 77777, 0, 120000 };

        while (seeks < 6 && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            // Read some frames, then jump while the producer has already buffered ahead.
            for (int round = 0; round < 6;)
            {
                const auto want = static_cast<int> (std::min<std::int64_t> (512, length - position));

                if (want <= 0)
                    break;

                if (stream.available (generation) < want)
                {
                    thread.notify();
                    juce::Thread::sleep (1);
                    continue;
                }

                if (! stream.read (generation, channels, want))
                    continue;

                for (int i = 0; i < want && correct; ++i)
                    correct = left[static_cast<size_t> (i)] == static_cast<float> ((position + i) * 2);

                position += want;
                ++round;
            }

            position = targets[seeks];
            generation = seek.publish (position).generation;
            thread.notify();
            ++seeks;
        }

        // After the last seek the stream continues from exactly that frame.
        while (stream.available (generation) < 512 && juce::Time::getMillisecondCounterHiRes() < deadline)
            juce::Thread::sleep (1);

        expect (stream.read (generation, channels, 512));
        expect (left[0] == static_cast<float> (position * 2), "first frame after the seek is the target frame");
        expect (correct, "all frames matched their song position");
    }

    void testBoundedMemory()
    {
        beginTest ("Streaming: reserved memory does not depend on the song length");
        SeekTarget seek;
        PrefetchStream shortStream (makeRamp (44100, 44100), seek, 44100, 88200, 4096);
        PrefetchStream longStream (makeRamp (44100, std::int64_t { 44100 } * 3600 * 10),
                                   seek,
                                   std::int64_t { 44100 } * 3600 * 10,
                                   88200,
                                   4096);
        expectEquals (static_cast<juce::int64> (shortStream.getReservedBytes()),
                      static_cast<juce::int64> (longStream.getReservedBytes()));
        expect (longStream.getReservedBytes() < 1000000);
    }

    void testReaderFrameSource()
    {
        beginTest ("Streaming: ReaderFrameSource reads WAV, duplicates mono, and zero-fills past the end");
        ScopedTempDirectory temp;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        juce::WavAudioFormat wav;

        // Mono 16-bit: 1000 frames of a ramp.
        {
            const auto file = temp.child ("mono.wav");
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            auto writer = wav.createWriterFor (
                stream,
                juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
            juce::AudioBuffer<float> buffer (1, 1000);

            for (int i = 0; i < 1000; ++i)
                buffer.setSample (0, i, static_cast<float> (i) / 2048.0f);

            expect (writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, 1000));
        }

        juce::String error;
        auto mono = openFrameSource (temp.child ("mono.wav"), formats, error);
        expect (mono != nullptr, error);

        if (mono != nullptr)
        {
            expectEquals (mono->getSampleRate(), 48000);
            expectEquals (static_cast<juce::int64> (mono->getLengthInFrames()), static_cast<juce::int64> (1000));
            std::vector<float> left (40, 9.0f), right (40, 9.0f);
            float* channels[2] = { left.data(), right.data() };
            expect (mono->read (990, channels, 40));

            for (int i = 0; i < 10; ++i)
            {
                expectWithinAbsoluteError (
                    left[static_cast<size_t> (i)], static_cast<float> (990 + i) / 2048.0f, 1.0e-4f);
                expect (left[static_cast<size_t> (i)] == right[static_cast<size_t> (i)], "mono is duplicated");
            }

            for (int i = 10; i < 40; ++i)
                expect (left[static_cast<size_t> (i)] == 0.0f && right[static_cast<size_t> (i)] == 0.0f,
                        "zeros past the end");

            expect (mono->read (-20, channels, 40));
            expect (left[0] == 0.0f && left[19] == 0.0f, "zeros before the start");
            expectWithinAbsoluteError (left[21], 1.0f / 2048.0f, 1.0e-4f);
        }

        beginTest ("Streaming: leading frames can be skipped and the length capped");
        auto trimmed = openFrameSource (temp.child ("mono.wav"), formats, error, 100, 50);
        expect (trimmed != nullptr, error);

        if (trimmed != nullptr)
        {
            expectEquals (static_cast<juce::int64> (trimmed->getLengthInFrames()), static_cast<juce::int64> (50));
            std::vector<float> left (60), right (60);
            float* channels[2] = { left.data(), right.data() };
            expect (trimmed->read (0, channels, 60));
            expectWithinAbsoluteError (left[0], 100.0f / 2048.0f, 1.0e-4f);
            expect (left[55] == 0.0f);
        }

        beginTest ("Streaming: a missing or garbage file is an error, not a crash");
        expect (openFrameSource (temp.child ("missing.wav"), formats, error) == nullptr);
        expect (error.isNotEmpty());
        temp.child ("garbage.wav").replaceWithText ("not audio");
        expect (openFrameSource (temp.child ("garbage.wav"), formats, error) == nullptr);

        beginTest ("Streaming: the MP3 fixture decodes through ReaderFrameSource");
        const auto mp3 = juce::File (SNDS_TESTDATA_DIR).getChildFile ("tone-stereo-44100.mp3");
        auto mp3Source = openFrameSource (mp3, formats, error);
        expect (mp3Source != nullptr, error);

        if (mp3Source != nullptr)
        {
            expectEquals (mp3Source->getSampleRate(), 44100);
            expect (mp3Source->getLengthInFrames() > 20000);
            std::vector<float> left (4096), right (4096);
            float* channels[2] = { left.data(), right.data() };
            expect (mp3Source->read (4096, channels, 4096));
            float peak = 0.0f;

            for (auto value : left)
                peak = std::max (peak, std::abs (value));

            expect (peak > 0.05f, "the decoded audio is not silent");
        }
    }
};

StreamingTests streamingTests;

//==============================================================================
class ResamplerTests final : public juce::UnitTest
{
public:
    ResamplerTests()
        : juce::UnitTest ("Resampler", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Resampler: equal rates copy frames exactly");
        {
            Resampler resampler;
            resampler.prepare (44100.0, 44100.0, 256);
            expect (resampler.isBypass());
            std::vector<float> input (5000);

            for (size_t i = 0; i < input.size(); ++i)
                input[i] = static_cast<float> (i) * 0.001f;

            const auto output = resampleMono (resampler, input, 256, 77);
            expect (output == input);
            expectEquals (resampler.getNextOutputTime(), 5000.0);
        }

        beginTest ("Resampler: output is independent of how input and output are chopped");
        {
            std::vector<float> input (9000);

            for (size_t i = 0; i < input.size(); ++i)
                input[i] = tone (1234.0, 44100.0, static_cast<std::int64_t> (i))
                         + tone (7777.0, 44100.0, static_cast<std::int64_t> (i));

            Resampler a, b;
            a.prepare (44100.0, 48000.0, 256);
            b.prepare (44100.0, 48000.0, 256);
            const auto first = resampleMono (a, input, 256, 512);
            const auto second = resampleMono (b, input, 100, 13);
            expectEquals (static_cast<int> (first.size()), static_cast<int> (second.size()));
            expect (first == second, "identical samples");
        }

        beginTest ("Resampler: tones are time-aligned and keep their level across rate pairs");

        for (const auto& pair : { std::pair { 44100.0, 48000.0 },
                                  std::pair { 48000.0, 44100.0 },
                                  std::pair { 96000.0, 48000.0 },
                                  std::pair { 44100.0, 96000.0 },
                                  std::pair { 192000.0, 48000.0 },
                                  std::pair { 48000.0, 192000.0 } })
        {
            const auto [source, destination] = pair;
            const double frequency = 3000.0;
            const auto frames = static_cast<size_t> (source * 0.25);
            std::vector<float> input (frames);

            for (size_t i = 0; i < frames; ++i)
                input[i] = tone (frequency, source, static_cast<std::int64_t> (i));

            Resampler resampler;
            resampler.prepare (source, destination, 256);
            const auto output = resampleMono (resampler, input, 256, 512);
            const auto ratio = source / destination;
            // The filter looks ahead, so the last outputs lack their future: compare the interior.
            const auto margin = static_cast<size_t> (resampler.getLookaheadFrames() / ratio) + 8;
            double worst = 0.0;

            for (auto k = margin; k + margin < output.size(); ++k)
                worst = std::max (
                    worst,
                    std::abs (static_cast<double> (output[k])
                              - 0.5 * std::sin (2.0 * pi * frequency * static_cast<double> (k) * ratio / source)));

            logMessage (juce::String (source) + " -> " + juce::String (destination) + " Hz: worst error "
                        + juce::String (20.0 * std::log10 (worst + 1.0e-12), 1) + " dBFS");
            expect (worst < 1.0e-3,
                    juce::String (source) + "->" + juce::String (destination) + " error " + juce::String (worst));
        }

        beginTest ("Resampler: content above the destination Nyquist is rejected, not aliased");
        {
            // 30 kHz in a 96 kHz song on a 48 kHz device would alias to 18 kHz without a filter.
            std::vector<float> input (96000);

            for (size_t i = 0; i < input.size(); ++i)
                input[i] = tone (30000.0, 96000.0, static_cast<std::int64_t> (i));

            Resampler resampler;
            resampler.prepare (96000.0, 48000.0, 256);
            const auto output = resampleMono (resampler, input, 256, 512);
            const auto level = rms (output, 2000, output.size() - 2000);
            const auto reference = 0.5 / std::sqrt (2.0);
            logMessage ("30 kHz at 96 kHz -> 48 kHz: "
                        + juce::String (20.0 * std::log10 (level / reference + 1.0e-12), 1) + " dB");
            expect (level < reference * 1.0e-3, "at least 60 dB down");
        }

        beginTest ("Resampler: the next output time tracks the ratio");
        {
            Resampler resampler;
            resampler.prepare (96000.0, 44100.0, 256);
            std::vector<float> input (20000, 0.25f);
            std::vector<float> left (1000), right (1000);
            float* destination[2] = { left.data(), right.data() };
            const float* source[2] = { input.data(), input.data() };

            expectEquals (resampler.getNextOutputTime(), 0.0);

            int produced = 0;

            while (produced < 1000)
            {
                float* target[2] = { left.data() + produced, right.data() + produced };
                produced += resampler.pull (target, 1000 - produced);
                resampler.push (source, 256);
            }

            expectEquals (produced, 1000);
            expectWithinAbsoluteError (resampler.getNextOutputTime(), 1000.0 * 96000.0 / 44100.0, 1.0e-6);
        }
    }
};

ResamplerTests resamplerTests;

//==============================================================================
class PlaybackTests final : public juce::UnitTest
{
public:
    PlaybackTests()
        : juce::UnitTest ("Playback", "snds")
    {
    }

    void runTest() override
    {
        testRefusesBadLoads();
        testBitExactUnitySum();
        testDurationAcrossRates();
        testUnderrun();
        testMemory();
        testFilesEndToEnd();
    }

private:
    void testRefusesBadLoads()
    {
        beginTest ("Playback: load fails before prepare, and for unusable sources");
        PlaybackEngine engine;
        expect (! engine.load (makeMarkerSources (44100, 44100)));
        engine.prepare (44100.0, 512);
        expect (engine.load (makeMarkerSources (44100, 44100)));

        auto mismatched = makeMarkerSources (44100, 44100);
        mismatched.stems[2] = std::make_unique<SyntheticSource> (48000, 44100, [] (std::int64_t, int) { return 0.1f; });
        expect (! engine.load (std::move (mismatched)));

        auto empty = makeMarkerSources (44100, 0);
        expect (! engine.load (std::move (empty)));
        expect (engine.isLoaded(), "a refused load leaves the current song alone");

        // Not playing: silence, and nothing is consumed.
        std::vector<float> left (256, 1.0f), right (256, 1.0f);
        float* channels[2] = { left.data(), right.data() };
        engine.process (channels, 2, 256);
        expect (left[0] == 0.0f && right[255] == 0.0f);
        expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (0));
    }

    void testBitExactUnitySum()
    {
        beginTest ("Playback: at matching rates the output is the exact stem sum, in any block size");
        const std::int64_t length = 44100 * 3 + 123;

        for (const int block : { 1, 37, 256, 512, 1000, 4096 })
        {
            PlaybackEngine engine;
            engine.prepare (44100.0, 4096);
            expect (engine.load (makeMarkerSources (44100, length)));
            engine.setPlaying (true);
            const auto frames = block == 1 ? std::int64_t { 3000 } : length + 1000;
            const auto rendered = renderOffline (engine, frames, block);
            expect (! rendered.timedOut);
            expectEquals (engine.getUnderrunCount(), 0);

            const auto ramp = rampFrames (44100.0);

            bool exact = true;
            const auto inSong = std::min (frames, length);

            for (std::int64_t i = 0; i < inSong && exact; ++i)
                exact = i < ramp
                          ? (isFadedCopy (rendered.left[static_cast<size_t> (i)], expectedUnitySum (i, 0), ramp)
                             && isFadedCopy (rendered.right[static_cast<size_t> (i)], expectedUnitySum (i, 1), ramp))
                          : (rendered.left[static_cast<size_t> (i)] == expectedUnitySum (i, 0)
                             && rendered.right[static_cast<size_t> (i)] == expectedUnitySum (i, 1));

            expect (exact, "block size " + juce::String (block) + " matches exactly");

            if (frames > length)
            {
                for (std::int64_t i = length; i < frames; ++i)
                    if (rendered.left[static_cast<size_t> (i)] != 0.0f)
                    {
                        expect (false, "silence after the end");
                        break;
                    }

                expect (engine.hasReachedEnd());
                expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (length));
            }
        }
    }

    void testDurationAcrossRates()
    {
        beginTest ("Playback: converted output has the exact duration, ends cleanly, and is band-correct");

        // Up and down by whole and fractional ratios; SNDS_HEAVY_TESTS=1 adds 48 -> 44.1 kHz (ValidateRateMatrix
        // covers that pair in the default run).
        std::vector<std::pair<int, double>> pairs { { 44100, 48000.0 }, { 96000, 48000.0 } };

        if (snds::test::heavyTests())
            pairs.emplace_back (48000, 44100.0);

        for (const auto& pair : pairs)
        {
            const auto [sourceRate, deviceRate] = pair;
            const std::int64_t length = sourceRate * 2 + 17;
            PlaybackEngine engine;
            engine.prepare (deviceRate, 512);

            PlaybackSources sources;
            sources.sampleRate = sourceRate;
            sources.lengthFrames = length;

            // Every stem carries the same 1 kHz tone at 0.1, so the unity sum is 0.4 amplitude.
            for (auto& stem : sources.stems)
                stem = std::make_unique<SyntheticSource> (
                    sourceRate,
                    length,
                    [sourceRate] (std::int64_t frame, int)
                    {
                        return 0.2f
                             * std::sin (
                                   static_cast<float> (2.0 * pi * 1000.0 * static_cast<double> (frame) / sourceRate));
                    });

            expect (engine.load (std::move (sources)));
            engine.setPlaying (true);
            const auto expected = expectedOutputFrames (length, sourceRate, deviceRate);
            const auto rendered = renderOffline (engine, expected + 2000, 480);
            expect (! rendered.timedOut);
            expectEquals (engine.getUnderrunCount(), 0);
            expect (engine.hasReachedEnd());
            expectEquals (static_cast<juce::int64> (engine.getPositionFrames()), static_cast<juce::int64> (length));

            // Count the audible frames: everything after `expected` must be exactly zero.
            std::int64_t lastNonZero = -1;

            for (std::int64_t i = 0; i < rendered.size(); ++i)
                if (rendered.left[static_cast<size_t> (i)] != 0.0f)
                    lastNonZero = i;

            expect (lastNonZero < expected,
                    "nothing after the song's end (last " + juce::String (lastNonZero) + " of "
                        + juce::String (expected) + ")");
            expect (lastNonZero >= expected - 3, "the song runs to its end");

            const auto level = rms (rendered.left, 4000, static_cast<size_t> (expected) - 4000) * std::sqrt (2.0);
            expectWithinAbsoluteError (level, 0.8, 0.01);
            logMessage (juce::String (sourceRate) + " -> " + juce::String (deviceRate) + ": " + juce::String (expected)
                        + " frames, level " + juce::String (level, 4));
        }
    }

    void testUnderrun()
    {
        beginTest ("Playback: an underrun is silence, is counted, and never skews the streams");
        const std::int64_t length = 44100 * 4;
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        // Stem 3 is slow: each 4096-frame read takes 4 ms (about 1 MS/s), far slower than a
        // render loop that does not wait.
        auto sources = makeMarkerSources (44100, length);
        sources.stems[3] = std::make_unique<SyntheticSource> (
            44100, length, [] (std::int64_t frame, int channel) { return stemMarker (3, frame, channel); }, 4);
        expect (engine.load (std::move (sources)));
        engine.setPlaying (true);

        // Wait for the first safe read-ahead, then drain without waiting.
        expect (engine.waitForReadAhead (512, 20000));
        std::vector<float> left (512), right (512);
        std::vector<float> audible;
        float* channels[2] = { left.data(), right.data() };
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 20000;
        int blocks = 0;

        while (! engine.hasReachedEnd() && juce::Time::getMillisecondCounterHiRes() < deadline && blocks < 20000000)
        {
            engine.process (channels, 2, 512);
            ++blocks;

            for (size_t i = 0; i < 512; ++i)
                if (left[i] != 0.0f)
                    audible.push_back (left[i]);
        }

        expect (engine.getUnderrunCount() > 0, "the slow stream underran");

        // Remove the silence: what is left must be the exact song, in order, nothing skipped.
        bool aligned = true;

        for (size_t i = 0; i < audible.size() && aligned; ++i)
            aligned =
                isFadedCopy (audible[i], expectedUnitySum (static_cast<std::int64_t> (i), 0), rampFrames (44100.0));

        expect (aligned, "the audible frames are the song in order after dropping silence");
        expectEquals (static_cast<juce::int64> (audible.size()), static_cast<juce::int64> (length));
        logMessage ("underruns: " + juce::String (engine.getUnderrunCount()) + " over " + juce::String (blocks)
                    + " blocks");
    }

    void testMemory()
    {
        beginTest ("Playback: reserved memory is the same for a 1 s song and a 10 h song");
        PlaybackEngine engine;
        engine.prepare (48000.0, 1024);
        expect (engine.load (makeMarkerSources (44100, 44100)));
        const auto shortBytes = engine.getReservedBytes();
        expect (engine.load (makeMarkerSources (44100, std::int64_t { 44100 } * 3600 * 10)));
        const auto longBytes = engine.getReservedBytes();
        expectEquals (static_cast<juce::int64> (shortBytes), static_cast<juce::int64> (longBytes));
        expect (longBytes < 8 * 1024 * 1024, "well under 8 MB for four stems at 44.1 kHz");
        logMessage ("reserved streaming memory: " + juce::String (static_cast<juce::int64> (longBytes)) + " bytes");

        engine.unload();
        expectEquals (static_cast<juce::int64> (engine.getReservedBytes()), static_cast<juce::int64> (0));
    }

    void testFilesEndToEnd()
    {
        beginTest ("Playback: four stem WAV files render bit-exactly through the engine");
        ScopedTempDirectory temp;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        juce::WavAudioFormat wav;
        const int frames = 30000;

        PlaybackSources sources;
        sources.sampleRate = 44100;
        sources.lengthFrames = frames;

        for (int stem = 0; stem < stemCount; ++stem)
        {
            const auto file = temp.child ("stem" + juce::String (stem) + ".wav");
            {
                std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
                auto writer = wav.createWriterFor (
                    stream,
                    juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (
                        32));
                expect (writer != nullptr);
                juce::AudioBuffer<float> buffer (2, frames);

                for (int channel = 0; channel < 2; ++channel)
                    for (int i = 0; i < frames; ++i)
                        buffer.setSample (channel, i, stemMarker (stem, i, channel));

                expect (writer->writeFromAudioSampleBuffer (buffer, 0, frames));
            }

            juce::String error;
            auto source = openFrameSource (file, formats, error);
            expect (source != nullptr, error);

            if (source == nullptr)
                return;

            sources.stems[static_cast<size_t> (stem)] = std::move (source);
        }

        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (std::move (sources)));
        engine.setPlaying (true);
        const auto rendered = renderOffline (engine, frames, 333);
        expect (! rendered.timedOut);
        const auto ramp = rampFrames (44100.0);
        bool exact = true;

        for (std::int64_t i = 0; i < frames && exact; ++i)
            exact = i < ramp ? (isFadedCopy (rendered.left[static_cast<size_t> (i)], expectedUnitySum (i, 0), ramp)
                                && isFadedCopy (rendered.right[static_cast<size_t> (i)], expectedUnitySum (i, 1), ramp))
                             : (rendered.left[static_cast<size_t> (i)] == expectedUnitySum (i, 0)
                                && rendered.right[static_cast<size_t> (i)] == expectedUnitySum (i, 1));

        expect (exact);
    }
};

PlaybackTests playbackTests;
} // namespace
} // namespace snds
