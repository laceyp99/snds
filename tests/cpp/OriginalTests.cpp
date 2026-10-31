#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "DspHeavy.h"
#include "PlaybackTestKit.h"
#include "TestHelpers.h"
#include "audio/FrameSource.h"
#include "audio/Mp3Gapless.h"
#include "audio/OriginalAlignment.h"
#include "audio/PlaybackEngine.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace snds
{
namespace
{
using namespace snds::audio;
using namespace snds::test;

constexpr int rate = 44100;
const int ramp = rampFrames (44100.0);

juce::File fixtureMp3()
{
    return juce::File (SNDS_TESTDATA_DIR).getChildFile ("tone-stereo-44100.mp3");
}

/** The signal testdata/generate_mp3_fixture.py encoded: 440 Hz left, 660 Hz right, 20 ms fades. */
float fixtureTone (std::int64_t frame, int channel)
{
    const auto frames = rate / 2;
    const double t = static_cast<double> (frame) / rate;
    const double last = static_cast<double> (frames - 1) / rate;
    const double fade = std::min (1.0, std::min (t / 0.02, (last - t) / 0.02));
    return static_cast<float> (0.4 * std::sin (2.0 * std::numbers::pi * (channel == 0 ? 440.0 : 660.0) * t) * fade);
}

/** Deterministic noise in [-0.5, 0.5): the same (seed, frame, channel) always gives the same value. */
float noise (int seed, std::int64_t frame, int channel)
{
    auto x = static_cast<std::uint64_t> (frame) * 0x9E3779B97F4A7C15ULL
           + static_cast<std::uint64_t> (seed) * 0xD1B54A32D192ED03ULL
           + static_cast<std::uint64_t> (channel) * 0x8CB92BA72F3D8DD7ULL;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<float> (static_cast<double> (x >> 11) / 9007199254740992.0) - 0.5f;
}

/** Four noise stems; their sum is the "mixture". */
float stemNoise (int stem, std::int64_t frame, int channel)
{
    return 0.2f * noise (stem + 1, frame, channel);
}

float mixtureNoise (std::int64_t frame, int channel)
{
    return ((stemNoise (0, frame, channel) + stemNoise (1, frame, channel)) + stemNoise (2, frame, channel))
         + stemNoise (3, frame, channel);
}

struct NoiseStems
{
    std::array<std::unique_ptr<SyntheticSource>, 4> sources;

    explicit NoiseStems (std::int64_t frames, float level = 1.0f)
    {
        for (int stem = 0; stem < 4; ++stem)
            sources[static_cast<size_t> (stem)] = std::make_unique<SyntheticSource> (
                rate,
                frames,
                [stem, level] (std::int64_t frame, int channel) { return level * stemNoise (stem, frame, channel); });
    }

    std::array<FrameSource*, 4> pointers()
    {
        return { sources[0].get(), sources[1].get(), sources[2].get(), sources[3].get() };
    }
};

std::unique_ptr<SyntheticSource> noiseOriginal (std::int64_t frames,
                                                std::int64_t shift,
                                                float level = 1.0f,
                                                int seedOffset = 0)
{
    // original(g) = mixture(g - shift): the original lags the stems by `shift` frames.
    return std::make_unique<SyntheticSource> (rate,
                                              frames,
                                              [=] (std::int64_t frame, int channel)
                                              {
                                                  const auto source = frame - shift;
                                                  return source < 0
                                                           ? 0.0f
                                                           : (seedOffset == 0
                                                                  ? level * mixtureNoise (source, channel)
                                                                  : level * noise (seedOffset, source, channel) * 0.4f);
                                              });
}

/** Writes a stereo or mono 24-bit WAV of a function. */
juce::File writeWav (const juce::File& file,
                     int sampleRate,
                     int channels,
                     std::int64_t frames,
                     const SyntheticSource::Generator& generator)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (
        stream,
        juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (channels).withBitsPerSample (24));
    juce::AudioBuffer<float> buffer (channels, static_cast<int> (frames));

    for (int channel = 0; channel < channels; ++channel)
        for (int i = 0; i < frames; ++i)
            buffer.setSample (channel, i, generator (i, channel));

    writer->writeFromAudioSampleBuffer (buffer, 0, static_cast<int> (frames));
    return file;
}

//==============================================================================
class OriginalAlignmentTests final : public juce::UnitTest
{
public:
    OriginalAlignmentTests()
        : juce::UnitTest ("OriginalAlignment", "snds")
    {
    }

    void runTest() override
    {
        testMp3Header();
        testMp3FixtureOffset();
        testMeasurement();
        testPlans();
        testMp3EndToEnd();
    }

private:
    void testMp3Header()
    {
        beginTest ("OriginalAlignment: the LAME tag of the MP3 fixture gives encoder delay and padding");
        const auto info = parseMp3 (fixtureMp3());
        expect (info.valid && info.hasInfoFrame && info.hasLameTag);
        expectEquals (info.encoderDelay, 576);
        expectEquals (info.encoderPadding, 1566);
        expectEquals (info.sampleRate, 44100);
        expectEquals (info.channels, 2);
        expectEquals (info.samplesPerFrame, 1152);
        expectEquals (predictedLeadingSkip (info), 576 + standardDecoderDelay);
        logMessage ("fixture: delay " + juce::String (info.encoderDelay) + ", padding "
                    + juce::String (info.encoderPadding) + ", header frames " + juce::String (info.headerFrames)
                    + ", predicted skip " + juce::String (predictedLeadingSkip (info)));

        beginTest ("OriginalAlignment: an ID3v2 prefix does not hide the tag; no tag means no skip; junk is invalid");
        juce::MemoryBlock bytes;
        expect (fixtureMp3().loadFileAsData (bytes));

        juce::MemoryBlock withId3;
        const unsigned char id3[10] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 100 };
        withId3.append (id3, 10);
        juce::MemoryBlock padding (100);
        padding.fillWith (0);
        withId3.append (padding.getData(), padding.getSize());
        withId3.append (bytes.getData(), bytes.getSize());
        const auto prefixed = parseMp3 (withId3.getData(), withId3.getSize());
        expect (prefixed.hasLameTag);
        expectEquals (prefixed.encoderDelay, 576);

        juce::MemoryBlock noTag (bytes);
        auto* raw = static_cast<char*> (noTag.getData());

        for (size_t i = 0; i + 4 < noTag.getSize() && i < 200; ++i)
            if (std::memcmp (raw + i, "Info", 4) == 0 || std::memcmp (raw + i, "Xing", 4) == 0)
                std::memcpy (raw + i, "Xxxx", 4);

        const auto untagged = parseMp3 (noTag.getData(), noTag.getSize());
        expect (untagged.valid && ! untagged.hasInfoFrame && ! untagged.hasLameTag);
        expectEquals (predictedLeadingSkip (untagged), 0);

        const char junk[] = "this is not an mp3 file at all, just text padding the buffer out";
        expect (! parseMp3 (junk, sizeof (junk)).valid);
        expect (! parseMp3 (juce::File ("C:\\does\\not\\exist.mp3")).valid);
    }

    void testMp3FixtureOffset()
    {
        beginTest (
            "OriginalAlignment: JUCE's MP3 decode lines up with the ideal signal at delay + 529, and only there");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        juce::String error;
        const auto skip = predictedLeadingSkip (parseMp3 (fixtureMp3()));
        double errorAt[3] = {};

        for (int i = 0; i < 3; ++i)
        {
            auto source = openFrameSource (fixtureMp3(), formats, error, skip - 1 + i, rate / 2);
            expect (source != nullptr, error);

            if (source == nullptr)
                return;

            expectEquals (static_cast<juce::int64> (source->getLengthInFrames()), static_cast<juce::int64> (rate / 2));
            std::vector<float> left (rate / 2), right (rate / 2);
            float* channels[2] = { left.data(), right.data() };
            expect (source->read (0, channels, rate / 2));
            double sum = 0.0;

            for (int n = 0; n < rate / 2; ++n)
            {
                const double d = left[static_cast<size_t> (n)] - fixtureTone (n, 0);
                const double e = right[static_cast<size_t> (n)] - fixtureTone (n, 1);
                sum += d * d + e * e;
            }

            errorAt[i] = std::sqrt (sum / (rate / 2.0));
        }

        logMessage ("rms error vs the ideal tone at skip -1/0/+1: " + juce::String (errorAt[0], 5) + " / "
                    + juce::String (errorAt[1], 5) + " / " + juce::String (errorAt[2], 5));
        expect (errorAt[1] < 0.003, "aligned: only MP3 coding error remains");
        expect (errorAt[0] > 4 * errorAt[1] && errorAt[2] > 4 * errorAt[1], "one sample off is clearly worse");
    }

    void testMeasurement()
    {
        beginTest ("OriginalAlignment: cross-correlation recovers the lag, in either direction");

        // A small positive and a near-the-range-edge negative lag by default (zero lag is the usable-original
        // test below); SNDS_HEAVY_TESTS=1 adds zero, -213, 1 and 2257.
        std::vector<std::int64_t> shifts { 37, -4000 };

        if (heavyTests())
            shifts.insert (shifts.end(), { 0, -213, 1, 2257 });

        for (const auto shift : shifts)
        {
            const std::int64_t frames = rate * 6; // the window is 5 s at most
            NoiseStems stems (frames);
            auto original = noiseOriginal (frames, shift);
            const auto measured = measureAlignment (*original, stems.pointers(), frames, rate);
            expect (measured.measurable);
            expectEquals (measured.lagFrames, static_cast<int> (shift));
            expect (measured.correlation > 0.99, "correlation " + juce::String (measured.correlation));
        }

        beginTest ("OriginalAlignment: a mixture that is only roughly the stem sum still finds the lag");
        {
            const std::int64_t frames = rate * 6;
            NoiseStems stems (frames);
            // The original is the mixture plus an unrelated component at about -20 dB, as stems never sum exactly.
            SyntheticSource original (rate,
                                      frames,
                                      [] (std::int64_t frame, int channel)
                                      {
                                          const auto source = frame - 641;
                                          return source < 0
                                                   ? 0.0f
                                                   : mixtureNoise (source, channel) + 0.1f * noise (99, frame, channel);
                                      });
            const auto measured = measureAlignment (original, stems.pointers(), frames, rate);
            expectEquals (measured.lagFrames, 641);
            expect (measured.correlation > 0.85 && measured.correlation < 1.0);
        }

        beginTest ("OriginalAlignment: unrelated audio correlates poorly; silence is not measurable");
        {
            const std::int64_t frames = rate * 6;
            NoiseStems stems (frames);
            auto unrelated = noiseOriginal (frames, 0, 1.0f, 7);
            const auto measured = measureAlignment (*unrelated, stems.pointers(), frames, rate);
            expect (measured.measurable && measured.correlation < 0.3,
                    "correlation " + juce::String (measured.correlation));

            NoiseStems quiet (frames, 0.0f);
            const auto silent = measureAlignment (*unrelated, quiet.pointers(), frames, rate);
            expect (! silent.measurable);
        }

        beginTest ("OriginalAlignment: a short song and a cancelled check are handled");
        {
            NoiseStems stems (3000);
            auto original = noiseOriginal (3000, 5);
            expectEquals (measureAlignment (*original, stems.pointers(), 3000, rate).lagFrames, 5);
            expect (! measureAlignment (*original, stems.pointers(), 3000, rate, [] { return true; }).measurable);
        }
    }

    void testPlans()
    {
        ScopedTempDirectory temp;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::int64_t frames = workload<std::int64_t> (rate * 3, rate * 6);

        beginTest ("OriginalAlignment: a WAV original equal to the stem sum is usable at skip 0");
        {
            const auto file = writeWav (temp.child ("original.wav"),
                                        rate,
                                        2,
                                        frames,
                                        [] (std::int64_t f, int c) { return mixtureNoise (f, c); });
            NoiseStems stems (frames);
            auto plan = planOriginal (file, "wav", formats, frames, rate, stems.pointers());
            expect (plan.usable, plan.reason);
            expectEquals (plan.leadingSkip, 0);
            expectEquals (plan.measured.lagFrames, 0);
            expect (plan.source != nullptr);
            logMessage (plan.summary);
        }

        beginTest ("OriginalAlignment: a mono WAV original is duplicated to both channels");
        {
            auto mono = writeWav (
                temp.child ("mono.wav"), rate, 1, frames, [] (std::int64_t f, int) { return mixtureNoise (f, 0); });
            SyntheticSource::Generator same = [] (std::int64_t f, int) { return stemNoise (0, f, 0); };
            std::array<std::unique_ptr<SyntheticSource>, 4> monoStems;

            for (int i = 0; i < 4; ++i)
                monoStems[static_cast<size_t> (i)] = std::make_unique<SyntheticSource> (
                    rate, frames, [i] (std::int64_t f, int) { return stemNoise (i, f, 0); });

            auto plan =
                planOriginal (mono,
                              "wav",
                              formats,
                              frames,
                              rate,
                              { monoStems[0].get(), monoStems[1].get(), monoStems[2].get(), monoStems[3].get() });
            expect (plan.usable, plan.reason);

            if (plan.source != nullptr)
            {
                std::vector<float> left (100), right (100);
                float* channels[2] = { left.data(), right.data() };
                plan.source->read (5000, channels, 100);
                expect (left == right && left[10] != 0.0f);
            }
        }

        beginTest ("OriginalAlignment: wrong length, wrong rate, unrelated audio, and unreadable files disable A/B "
                   "with a reason");
        {
            NoiseStems stems (frames);
            const auto shortFile = writeWav (temp.child ("short.wav"),
                                             rate,
                                             2,
                                             frames - 500,
                                             [] (std::int64_t f, int c) { return mixtureNoise (f, c); });
            auto plan = planOriginal (shortFile, "wav", formats, frames, rate, stems.pointers());
            expect (! plan.usable && plan.reason.contains ("frames"), plan.reason);

            const auto wrongRate = writeWav (
                temp.child ("rate.wav"), 48000, 2, frames, [] (std::int64_t f, int c) { return mixtureNoise (f, c); });
            plan = planOriginal (wrongRate, "wav", formats, frames, rate, stems.pointers());
            expect (! plan.usable && plan.reason.contains ("Hz"), plan.reason);

            const auto other = writeWav (temp.child ("other.wav"),
                                         rate,
                                         2,
                                         frames,
                                         [] (std::int64_t f, int c) { return 0.4f * noise (55, f, c); });
            plan = planOriginal (other, "wav", formats, frames, rate, stems.pointers());
            expect (! plan.usable && plan.reason.contains ("does not match"), plan.reason);

            plan = planOriginal (temp.child ("missing.wav"), "wav", formats, frames, rate, stems.pointers());
            expect (! plan.usable && plan.reason.contains ("cannot be read"), plan.reason);
        }

        beginTest ("OriginalAlignment: a WAV that is offset from the stems is refused, not played misaligned");
        {
            NoiseStems stems (frames);
            const auto shifted =
                writeWav (temp.child ("shifted.wav"),
                          rate,
                          2,
                          frames,
                          [] (std::int64_t f, int c) { return f < 300 ? 0.0f : mixtureNoise (f - 300, c); });
            const auto plan = planOriginal (shifted, "wav", formats, frames, rate, stems.pointers());
            expect (! plan.usable && plan.reason.contains ("300 samples"), plan.reason);
            expectEquals (plan.measured.lagFrames, 300);
        }
    }

    void testMp3EndToEnd()
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::int64_t frames = rate / 2;

        auto makeStems = [frames] (int delay)
        {
            std::array<std::unique_ptr<SyntheticSource>, 4> stems;

            for (int i = 0; i < 4; ++i)
                stems[static_cast<size_t> (i)] = std::make_unique<SyntheticSource> (
                    rate,
                    frames,
                    [i, delay] (std::int64_t f, int c)
                    { return i == 0 && f >= delay ? fixtureTone (f - delay, c) : 0.0f; });

            return stems;
        };

        beginTest ("OriginalAlignment: the MP3 fixture plans to skip 1105 and verifies sample-exactly");
        {
            auto stems = makeStems (0);
            const auto plan = planOriginal (fixtureMp3(),
                                            "mp3",
                                            formats,
                                            frames,
                                            rate,
                                            { stems[0].get(), stems[1].get(), stems[2].get(), stems[3].get() });
            logMessage (plan.summary);
            expect (plan.usable, plan.reason);
            expectEquals (plan.leadingSkip, 576 + 529);
            expectEquals (plan.measured.lagFrames, 0);
            expect (plan.measured.correlation > 0.95, "correlation " + juce::String (plan.measured.correlation));
        }

        beginTest ("OriginalAlignment: stems that do not line up with the MP3's gapless offset disable A/B");
        {
            auto stems = makeStems (100); // as if the worker's decode were 100 samples later than JUCE's
            const auto plan = planOriginal (fixtureMp3(),
                                            "mp3",
                                            formats,
                                            frames,
                                            rate,
                                            { stems[0].get(), stems[1].get(), stems[2].get(), stems[3].get() });
            logMessage (plan.summary);
            expect (! plan.usable && plan.reason.contains ("not explain"), plan.reason);
        }
    }
};

OriginalAlignmentTests originalAlignmentTests;

//==============================================================================
/** A float of the unity sum, scaled; used when the original is a known multiple of the stems. */
std::int64_t findScaledRun (const Rendered& rendered, std::int64_t firstSongFrame, int runLength, float scale)
{
    for (std::int64_t i = 0; i + runLength <= rendered.size(); ++i)
    {
        if (rendered.left[static_cast<size_t> (i)] != scale * expectedUnitySum (firstSongFrame, 0))
            continue;

        bool all = true;

        for (int k = 0; k < runLength && all; ++k)
            all = rendered.left[static_cast<size_t> (i + k)] == scale * expectedUnitySum (firstSongFrame + k, 0)
               && rendered.right[static_cast<size_t> (i + k)] == scale * expectedUnitySum (firstSongFrame + k, 1);

        if (all)
            return i;
    }

    return -1;
}

PlaybackSources markerSourcesWithOriginal (std::int64_t frames, float originalScale, std::int64_t originalLength = -1)
{
    auto sources = makeMarkerSources (rate, frames);
    sources.original = std::make_unique<SyntheticSource> (rate,
                                                          originalLength < 0 ? frames : originalLength,
                                                          [originalScale] (std::int64_t f, int c)
                                                          { return originalScale * expectedUnitySum (f, c); });
    return sources;
}

class OriginalABTests final : public juce::UnitTest
{
public:
    OriginalABTests()
        : juce::UnitTest ("OriginalAB", "snds")
    {
    }

    void runTest() override
    {
        testUnavailable();
        testIdenticalSignals();
        testExclusiveAndNoDoubling();
        testSeeksKeepOriginalAligned();
        testClickFree();
        testLoopAndMemory();
        testMp3Playback();
    }

private:
    static constexpr std::int64_t length = rate * 6 + 11;

    void testUnavailable()
    {
        beginTest ("OriginalAB: without an original A/B is unavailable and playback is unchanged");
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (makeMarkerSources (rate, 30000)));
        expect (! engine.hasOriginal());
        expect (! engine.setOriginalSelected (true));
        expect (! engine.isOriginalSelected());
        engine.play();
        const auto rendered = renderOffline (engine, 5000, 512);
        expect (findScaledRun (rendered, ramp - 1, 3000, 1.0f) >= 0
                || findScaledRun (rendered, ramp - 1, 3000, 1.0f) >= 0);
    }

    void testIdenticalSignals()
    {
        beginTest ("OriginalAB: an original equal to the stem sum gives identical output in both modes, through "
                   "toggles and seeks");
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (markerSourcesWithOriginal (length, 1.0f)));
        expect (engine.hasOriginal());
        engine.play();
        Rendered rendered;
        juce::Random random (21);
        std::int64_t song = 0; // song frame of the next audible output frame

        for (int round = 0; round < 30; ++round)
        {
            engine.setOriginalSelected (round % 2 == 1);
            const auto block = renderOffline (engine, 700 + random.nextInt (3000), 256 + random.nextInt (500));
            rendered.left.insert (rendered.left.end(), block.left.begin(), block.left.end());
            rendered.right.insert (rendered.right.end(), block.right.begin(), block.right.end());
        }

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

        expect (intact, "frame " + juce::String (song) + " differs between A and B");
        expectEquals (engine.getUnderrunCount(), 0);
    }

    void testExclusiveAndNoDoubling()
    {
        beginTest ("OriginalAB: only one source sounds at a time; the crossfade never exceeds either side");
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        // The original is half the stem sum, so any doubling or leakage shows in the level.
        expect (engine.load (markerSourcesWithOriginal (length, 0.5f)));
        engine.play();
        Rendered all = renderOffline (engine, 6000, 512);
        const auto stemsStart = all.size();
        expect (findScaledRun (all, ramp - 1, 4000, 1.0f) >= 0, "stem mix plays by default, exactly");

        engine.setOriginalSelected (true);
        expect (engine.isOriginalSelected());
        const auto toOriginal = renderOffline (engine, 6000, 512);
        engine.setOriginalSelected (false);
        const auto backToStems = renderOffline (engine, 6000, 512);
        (void) stemsStart;

        // Song position of output frame i is i - (no silence here): frames are consecutive.
        auto classify = [&] (const Rendered& block,
                             std::int64_t firstSongFrame,
                             int& stemFrames,
                             int& originalFrames,
                             int& blended,
                             bool& bounded)
        {
            stemFrames = originalFrames = blended = 0;
            bounded = true;

            for (std::int64_t i = 0; i < block.size(); ++i)
            {
                const auto e = expectedUnitySum (firstSongFrame + i, 0);
                const auto v = block.left[static_cast<size_t> (i)];

                if (v == e)
                    ++stemFrames;
                else if (v == 0.5f * e)
                    ++originalFrames;
                else
                    ++blended;

                bounded = bounded && v <= e + 1.0e-6f && v >= 0.5f * e - 1.0e-6f;
            }
        };

        int stemFrames, originalFrames, blended;
        bool bounded;
        classify (toOriginal, all.size(), stemFrames, originalFrames, blended, bounded);
        logMessage ("to original: " + juce::String (stemFrames) + " stem / " + juce::String (blended) + " blended / "
                    + juce::String (originalFrames) + " original frames");
        expect (bounded, "never above the stem mix or below the original");
        expect (std::abs (blended - 1323) <= 3, "crossfade lasts about 30 ms: " + juce::String (blended));
        expect (originalFrames > 4000 && stemFrames < 700);

        classify (backToStems, all.size() + toOriginal.size(), stemFrames, originalFrames, blended, bounded);
        expect (bounded && std::abs (blended - 1323) <= 3);
        expect (stemFrames > 4000 && originalFrames < 700);

        // For comparison: an equal-power law on these correlated signals peaks 3 dB high mid-fade.
        const auto equalPowerPeak = std::cos (std::numbers::pi / 4.0) + std::sin (std::numbers::pi / 4.0);
        logMessage ("equal-power crossfade of aligned identical signals would peak at x"
                    + juce::String (equalPowerPeak, 3) + "; the sum-to-one law used here stays at x1.000");
    }

    void testSeeksKeepOriginalAligned()
    {
        beginTest ("OriginalAB: after any seek the original is exactly the same song frame as the stems");
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (markerSourcesWithOriginal (length, 0.5f)));
        engine.play();
        juce::Random random (4);

        for (int round = 0; round < 16; ++round)
        {
            const bool original = round % 2 == 0;
            engine.setOriginalSelected (original);
            renderOffline (engine, 2500, 400); // let any crossfade finish
            const auto target = static_cast<std::int64_t> (random.nextInt (static_cast<int> (length - 20000)));
            engine.seek (target);
            const auto rendered = renderOffline (engine, 9000, 300 + round * 11);
            const auto start = findScaledRun (rendered, target + ramp - 1, 2000, original ? 0.5f : 1.0f);
            expect (start >= 0, juce::String (original ? "original" : "stems") + " from " + juce::String (target));
        }

        expectEquals (engine.getUnderrunCount(), 0);
    }

    void testClickFree()
    {
        beginTest ("OriginalAB: switching a tone between mixes does not click");
        PlaybackEngine engine;
        engine.prepare (48000.0, 512);

        PlaybackSources sources;
        sources.sampleRate = rate;
        sources.lengthFrames = rate * 8;

        for (auto& stem : sources.stems)
            stem = std::make_unique<SyntheticSource> (
                rate,
                rate * 8,
                [] (std::int64_t f, int)
                {
                    return 0.2f
                         * static_cast<float> (
                               std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (f) / rate));
                });

        // Aligned, but 6 dB quieter: the switch is a level change on the same waveform.
        sources.original = std::make_unique<SyntheticSource> (
            rate,
            rate * 8,
            [] (std::int64_t f, int)
            {
                return 0.4f
                     * static_cast<float> (std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (f) / rate));
            });

        expect (engine.load (std::move (sources)));
        engine.play();
        Rendered rendered = renderOffline (engine, 4000, 480);

        for (int round = 0; round < 12; ++round)
        {
            engine.setOriginalSelected (round % 2 == 0);
            const auto block = renderOffline (engine, 3000, 333);
            rendered.left.insert (rendered.left.end(), block.left.begin(), block.left.end());
        }

        double step = 0.0;

        for (size_t i = 1; i < rendered.left.size(); ++i)
            step = std::max (step, std::abs (static_cast<double> (rendered.left[i]) - rendered.left[i - 1]));

        logMessage ("largest sample step over 12 A/B switches: " + juce::String (step, 4));
        expect (step < 0.12, "step " + juce::String (step)); // the 0.8 tone itself moves 0.105 per sample
    }

    void testLoopAndMemory()
    {
        beginTest ("OriginalAB: looping works with the original selected, and memory is independent of length");
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (markerSourcesWithOriginal (20037, 0.5f)));
        engine.setOriginalSelected (true);
        engine.setLooping (true);
        engine.play();
        const auto rendered = renderOffline (engine, 20037 * 3, 700);
        expect (! rendered.timedOut && engine.isPlaying());
        expectEquals (engine.getUnderrunCount(), 0);
        bool intact = true;

        for (std::int64_t i = 3000; i < rendered.size() && intact; ++i) // past the fades, away from the seams
        {
            const auto song = i % 20037;

            if (song > 400 && song < 20037 - 400)
                intact = rendered.left[static_cast<size_t> (i)] == 0.5f * expectedUnitySum (song, 0);
        }

        expect (intact);

        const auto shortBytes = engine.getReservedBytes();
        expect (engine.load (markerSourcesWithOriginal (std::int64_t { rate } * 3600 * 5, 0.5f)));
        expectEquals (static_cast<juce::int64> (engine.getReservedBytes()), static_cast<juce::int64> (shortBytes));
        expect (engine.getReservedBytes() < 10 * 1024 * 1024, "five streams stay under 10 MB");
        logMessage ("reserved memory with an original: "
                    + juce::String (static_cast<juce::int64> (engine.getReservedBytes())) + " bytes");

        beginTest ("OriginalAB: an original that is shorter than the song is padded with silence");
        expect (engine.load (markerSourcesWithOriginal (30000, 0.5f, 20000)));
        engine.setOriginalSelected (true);
        engine.play();
        const auto tail = renderOffline (engine, 29000, 512);
        expect (! tail.timedOut);
        expect (tail.left[27000] == 0.0f);
    }

    void testMp3Playback()
    {
        beginTest ("OriginalAB: the MP3 fixture plays aligned with ideal-tone stems (Original vs Stem Mix)");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::int64_t frames = rate / 2;

        auto makeStem = [frames] (int index)
        {
            return std::make_unique<SyntheticSource> (
                rate, frames, [index] (std::int64_t f, int c) { return index == 0 ? fixtureTone (f, c) : 0.0f; });
        };

        std::array<std::unique_ptr<SyntheticSource>, 4> planningStems;

        for (int i = 0; i < 4; ++i)
            planningStems[static_cast<size_t> (i)] = makeStem (i);

        auto plan = planOriginal (
            fixtureMp3(),
            "mp3",
            formats,
            frames,
            rate,
            { planningStems[0].get(), planningStems[1].get(), planningStems[2].get(), planningStems[3].get() });
        expect (plan.usable, plan.reason);

        if (! plan.usable)
            return;

        // One engine per mode, so neither render carries a fade from the other.
        auto render = [&] (bool useOriginal)
        {
            auto again = planOriginal (
                fixtureMp3(),
                "mp3",
                formats,
                frames,
                rate,
                { planningStems[0].get(), planningStems[1].get(), planningStems[2].get(), planningStems[3].get() });
            PlaybackSources sources;
            sources.sampleRate = rate;
            sources.lengthFrames = frames;

            for (int i = 0; i < 4; ++i)
                sources.stems[static_cast<size_t> (i)] = makeStem (i);

            sources.original = std::move (again.source);
            PlaybackEngine engine;
            engine.prepare (44100.0, 512);
            expect (engine.load (std::move (sources)));
            expect (engine.setOriginalSelected (useOriginal));
            engine.play();
            return renderOffline (engine, rate / 2 - 100, 512);
        };

        const auto asOriginal = render (true);
        const auto asStems = render (false);

        double sum = 0.0;
        double peak = 0.0;
        int count = 0;

        for (std::int64_t i = 2000; i < asOriginal.size() - 2000; ++i)
        {
            const double d = asOriginal.left[static_cast<size_t> (i)] - asStems.left[static_cast<size_t> (i)];
            sum += d * d;
            peak = std::max (peak, std::abs (d));
            ++count;
        }

        const auto rms = std::sqrt (sum / count);
        logMessage ("Original vs Stem Mix on the MP3 fixture: rms difference " + juce::String (rms, 5) + ", peak "
                    + juce::String (peak, 4));
        expect (rms < 0.003 && peak < 0.02, "the two mixes are the same audio (only MP3 coding error differs)");
    }
};

OriginalABTests originalABTests;
} // namespace
} // namespace snds
