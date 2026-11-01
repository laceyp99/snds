// The owner-supplied music excerpt (testdata/asking42much-excerpt.wav and .mp3, 10 s of a real mix):
// what the committed files are, how they import, and how the MP3's encoder delay shows up in the
// A/B alignment. Real music matters here because synthetic tones and noise can hide a one-sample
// offset that a drum hit cannot. Nothing in this file needs the worker or the model.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "audio/FrameSource.h"
#include "audio/Mp3Gapless.h"
#include "audio/OriginalAlignment.h"
#include "model/SongImport.h"
#include "model/SourceFingerprint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace snds
{
namespace
{
using namespace snds::audio;

constexpr juce::int64 excerptFrames = 441000; // 10.000 s at 44.1 kHz
constexpr int excerptRate = 44100;

// Pinned to the committed bytes (also recorded in testdata/README.md). If the excerpt is ever
// regenerated these change on purpose; nothing else should.
const char* const wavSha256 = "5b859d9c2a3e321b5470aa5959283ec3c8c15e2426c5f650d0a8d4d422d267aa";
const char* const mp3Sha256 = "d8eeb337575c78ed99c3bfbf20c09aeb64d0e2a2d88105663465dc0b12902752";

juce::File fixture (const char* name)
{
    return juce::File (juce::String (juce::CharPointer_UTF8 (SNDS_TESTDATA_DIR)).replace ("/", "\\"))
        .getChildFile (name);
}

juce::String sha256Of (const juce::File& file)
{
    juce::MemoryBlock bytes;
    file.loadFileAsData (bytes);
    return Sha256::hash (bytes.getData(), bytes.getSize());
}

// A mono mix, decoded in full, with `skip` leading frames dropped (what ReaderFrameSource does).
std::vector<float> decodeMono (const juce::File& file,
                               juce::AudioFormatManager& formats,
                               juce::int64 skip,
                               juce::int64 frames)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    std::vector<float> mono;

    if (reader == nullptr)
        return mono;

    const auto available = std::max<juce::int64> (0, reader->lengthInSamples - skip);
    const auto count = static_cast<int> (std::min (frames, available));
    juce::AudioBuffer<float> buffer (2, count);
    reader->read (&buffer, 0, count, skip, true, true);
    mono.resize (static_cast<std::size_t> (count));

    for (int i = 0; i < count; ++i)
        mono[static_cast<std::size_t> (i)] =
            0.5f * (buffer.getSample (0, i) + buffer.getSample (reader->numChannels > 1 ? 1 : 0, i));

    return mono;
}

// Normalised correlation of a[n] with b[n + lag] over [from, from + length).
double correlationAt (
    const std::vector<float>& a, const std::vector<float>& b, int lag, std::size_t from, std::size_t length)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;

    for (std::size_t n = from; n < from + length; ++n)
    {
        const auto m = static_cast<std::int64_t> (n) + lag;

        if (m < 0 || m >= static_cast<std::int64_t> (b.size()) || n >= a.size())
            continue;

        const double x = a[n];
        const double y = b[static_cast<std::size_t> (m)];
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }

    return ab / std::sqrt (std::max (aa * bb, 1.0e-30));
}

class FixtureTests final : public juce::UnitTest
{
public:
    FixtureTests()
        : juce::UnitTest ("Fixture: owner music excerpt", "snds")
    {
    }

    void runTest() override
    {
        testFiles();
        testImport();
        testMp3Accounting();
        testMp3AlignsToTheSample();
        testPlanOriginal();
    }

private:
    void testFiles()
    {
        beginTest ("The committed excerpt files are the ones recorded in testdata/README.md");

        const auto wav = fixture ("asking42much-excerpt.wav");
        const auto mp3 = fixture ("asking42much-excerpt.mp3");
        expect (wav.existsAsFile() && mp3.existsAsFile());
        expectEquals (wav.getSize(), static_cast<juce::int64> (1764044)); // 44 byte header + 441000 x 2 x 2
        expect (mp3.getSize() > 200000 && mp3.getSize() < 300000);
        expectEquals (sha256Of (wav), juce::String (wavSha256));
        expectEquals (sha256Of (mp3), juce::String (mp3Sha256));

        // Small enough to live in the repository; the full track must never be here.
        expect (wav.getSize() < 2 * 1024 * 1024);
    }

    void testImport()
    {
        beginTest ("Both files are accepted by the importer with the format the worker expects");

        const auto wav = validateSourceFile (fixture ("asking42much-excerpt.wav"));
        expect (wav.accepted(), wav.message);

        if (wav.accepted())
        {
            expectEquals (wav.info.format, juce::String ("wav"));
            expectEquals (wav.info.channels, 2);
            expectWithinAbsoluteError (wav.info.sampleRate, 44100.0, 1.0e-6);
            expectEquals (wav.info.frames, excerptFrames);
            expectEquals (wav.info.bitsPerSample, 16);
            expect (! wav.info.floatingPoint);
            expectWithinAbsoluteError (wav.info.durationSeconds, 10.0, 1.0e-6);
        }

        const auto mp3 = validateSourceFile (fixture ("asking42much-excerpt.mp3"));
        expect (mp3.accepted(), mp3.message);

        if (mp3.accepted())
        {
            expectEquals (mp3.info.format, juce::String ("mp3"));
            expectEquals (mp3.info.channels, 2);
            expectWithinAbsoluteError (mp3.info.sampleRate, 44100.0, 1.0e-6);
            // libsndfile (the worker) trims the encoder delay and padding and reports exactly 441000
            // frames; JUCE does not trim, so its count is longer by a few MPEG frames at most.
            expect (mp3.info.frames >= excerptFrames && mp3.info.frames - excerptFrames < 4608);
        }
    }

    void testMp3Accounting()
    {
        beginTest ("The MP3's LAME tag explains the frames JUCE decodes beyond the worker's 441000");

        const auto file = fixture ("asking42much-excerpt.mp3");
        const auto info = parseMp3 (file);
        expect (info.valid && info.hasInfoFrame && info.hasLameTag, "the encoder tag is present");
        expectEquals (info.sampleRate, 44100);
        expectEquals (info.channels, 2);
        expectEquals (info.samplesPerFrame, 1152);

        juce::AudioFormatManager formats;
        formats.registerFormat (new juce::MP3AudioFormat(), true);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        expect (reader != nullptr);

        if (reader == nullptr)
            return;

        const auto juceFrames = reader->lengthInSamples;
        logMessage ("excerpt MP3: JUCE decodes " + juce::String (juceFrames) + " frames, LAME delay "
                    + juce::String (info.encoderDelay) + ", padding " + juce::String (info.encoderPadding)
                    + ", header frames " + juce::String (info.headerFrames) + ", predicted leading skip "
                    + juce::String (predictedLeadingSkip (info)));

        // The worker's decoder drops (delay + 529) at the start and the padding (less the 529) at the end.
        expectEquals (predictedLeadingSkip (info), info.encoderDelay + standardDecoderDelay);
        expectEquals (static_cast<juce::int64> (juceFrames) - predictedLeadingSkip (info)
                          - (info.encoderPadding - standardDecoderDelay),
                      excerptFrames,
                      "JUCE frames - leading skip - trimmed padding is the libsndfile length");
    }

    void testMp3AlignsToTheSample()
    {
        beginTest (
            "On real music, the JUCE decode lines up with the WAV to the sample once the leading skip is dropped");

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        formats.registerFormat (new juce::MP3AudioFormat(), true);

        const auto info = parseMp3 (fixture ("asking42much-excerpt.mp3"));
        const auto wav = decodeMono (fixture ("asking42much-excerpt.wav"), formats, 0, excerptFrames);
        const auto mp3 =
            decodeMono (fixture ("asking42much-excerpt.mp3"), formats, predictedLeadingSkip (info), excerptFrames);
        expectEquals (static_cast<int> (wav.size()), static_cast<int> (excerptFrames));
        expect (mp3.size() >= static_cast<std::size_t> (excerptFrames) - 2048);

        // A busy 3 s window in the middle (away from the codec's start and end effects).
        const std::size_t from = 150000, length = 132300;
        double best = -2.0;
        int bestLag = 99;

        for (int lag = -3; lag <= 3; ++lag)
        {
            const auto c = correlationAt (wav, mp3, lag, from, length);

            if (c > best)
            {
                best = c;
                bestLag = lag;
            }
        }

        logMessage ("excerpt MP3 vs WAV: best lag " + juce::String (bestLag) + ", correlation "
                    + juce::String (best, 5));
        expectEquals (bestLag, 0);
        expect (best > 0.99, "a 190 kbps MP3 of the same music correlates above 0.99: " + juce::String (best));

        // One sample off is clearly worse: the offset the delay would cause is detectable.
        expect (correlationAt (wav, mp3, 1, from, length) < best - 0.0005);
        expect (correlationAt (wav, mp3, -1, from, length) < best - 0.0005);

        // Without the skip the MP3 starts late by a known amount (the lag is not 0), which is exactly
        // what the alignment code exists to fix.
        const auto untrimmed = decodeMono (fixture ("asking42much-excerpt.mp3"), formats, 0, excerptFrames);
        double rawBest = -2.0;
        int rawLag = 0;
        for (int lag = -1500; lag <= 1500; lag += 1)
        {
            const auto c = correlationAt (wav, untrimmed, lag, from, 44100);

            if (c > rawBest)
            {
                rawBest = c;
                rawLag = lag;
            }
        }
        expectEquals (rawLag, predictedLeadingSkip (info));
    }

    void testPlanOriginal()
    {
        beginTest (
            "planOriginal accepts the excerpt as an original for stems that sum to it, and rejects a shifted one");

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        formats.registerFormat (new juce::MP3AudioFormat(), true);

        // Stand-in stems: the excerpt as the first stem and silence for the other three, so their sum
        // is exactly the WAV. (A worker-prepared package is covered by the opt-in end-to-end test.)
        juce::String error;
        auto first = openFrameSource (fixture ("asking42much-excerpt.wav"), formats, error);
        expect (first != nullptr, error);

        if (first == nullptr)
            return;

        struct Silence final : FrameSource
        {
            int getSampleRate() const noexcept override { return excerptRate; }
            std::int64_t getLengthInFrames() const noexcept override { return excerptFrames; }
            bool read (std::int64_t, float* const* destination, int numFrames) override
            {
                std::fill_n (destination[0], numFrames, 0.0f);
                std::fill_n (destination[1], numFrames, 0.0f);
                return true;
            }
        };

        Silence silence1, silence2, silence3;
        const std::array<FrameSource*, 4> stems { first.get(), &silence1, &silence2, &silence3 };

        auto wavPlan =
            planOriginal (fixture ("asking42much-excerpt.wav"), "wav", formats, excerptFrames, excerptRate, stems);
        expect (wavPlan.usable, wavPlan.reason);
        expectEquals (wavPlan.leadingSkip, 0);
        expectEquals (wavPlan.measured.lagFrames, 0);
        expect (wavPlan.measured.correlation > 0.999, "identical audio");

        auto mp3Plan =
            planOriginal (fixture ("asking42much-excerpt.mp3"), "mp3", formats, excerptFrames, excerptRate, stems);
        logMessage ("MP3 plan: " + mp3Plan.summary);
        expect (mp3Plan.usable, mp3Plan.reason);
        expectEquals (mp3Plan.leadingSkip, predictedLeadingSkip (parseMp3 (fixture ("asking42much-excerpt.mp3"))));
        expectEquals (mp3Plan.measured.lagFrames, 0);
        expect (mp3Plan.measured.correlation > 0.95,
                "lossy copy of the same music: " + juce::String (mp3Plan.measured.correlation));
        expect (mp3Plan.source != nullptr);

        // The same stems 700 frames late: the original no longer lines up and A/B is switched off with
        // the measured lag in the reason.
        struct Late final : FrameSource
        {
            explicit Late (FrameSource& s)
                : inner (s)
            {
            }
            int getSampleRate() const noexcept override { return inner.getSampleRate(); }
            std::int64_t getLengthInFrames() const noexcept override { return inner.getLengthInFrames(); }
            bool read (std::int64_t startFrame, float* const* destination, int numFrames) override
            {
                return inner.read (startFrame - 700, destination, numFrames);
            }
            FrameSource& inner;
        };

        Late late (*first);
        const std::array<FrameSource*, 4> lateStems { &late, &silence1, &silence2, &silence3 };
        const auto shifted =
            planOriginal (fixture ("asking42much-excerpt.wav"), "wav", formats, excerptFrames, excerptRate, lateStems);
        expect (! shifted.usable, "an unexplained offset must disable A/B");
        logMessage ("shifted plan: " + shifted.reason);
        expect (shifted.reason.isNotEmpty());
    }
};

FixtureTests fixtureTests;
} // namespace
} // namespace snds
