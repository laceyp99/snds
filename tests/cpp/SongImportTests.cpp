#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "model/SongImport.h"

#include <cmath>
#include <vector>

namespace snds
{
namespace
{
struct WavSpec
{
    int formatTag = 1; // 1 PCM, 3 float, 2 ADPCM, 0xFFFE extensible
    int subFormatTag = 1;
    int channels = 2;
    int sampleRate = 44100;
    int bits = 16;
    int frames = 100;
    bool riff = true;
};

void put16 (juce::MemoryOutputStream& out, int value)
{
    out.writeShort (static_cast<short> (value));
}

void put32 (juce::MemoryOutputStream& out, juce::uint32 value)
{
    out.writeInt (static_cast<int> (value));
}

/** A byte-exact WAV: a header per the spec and a ramp of the requested sample size. */
juce::MemoryBlock makeWav (const WavSpec& spec)
{
    const auto bytesPerSample = spec.bits / 8;
    const auto blockAlign = spec.channels * bytesPerSample;
    const auto dataBytes = static_cast<juce::uint32> (spec.frames * blockAlign);
    const bool extensible = spec.formatTag == 0xFFFE;

    juce::MemoryOutputStream out;
    out.write (spec.riff ? "RIFF" : "JUNK", 4);
    put32 (out, 4 + 8 + (extensible ? 40u : 16u) + 8 + dataBytes);
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    put32 (out, extensible ? 40u : 16u);
    put16 (out, spec.formatTag);
    put16 (out, spec.channels);
    put32 (out, static_cast<juce::uint32> (spec.sampleRate));
    put32 (out, static_cast<juce::uint32> (spec.sampleRate * blockAlign));
    put16 (out, blockAlign);
    put16 (out, spec.bits);

    if (extensible)
    {
        put16 (out, 22);
        put16 (out, spec.bits);
        put32 (out, 3);
        put16 (out, spec.subFormatTag);
        const unsigned char guidTail[14] { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                           0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        out.write (guidTail, sizeof (guidTail));
    }

    out.write ("data", 4);
    put32 (out, dataBytes);

    for (juce::uint32 i = 0; i < dataBytes; ++i)
        out.writeByte (static_cast<char> (i % 97));

    return out.getMemoryBlock();
}

class SongImportTests final : public juce::UnitTest
{
public:
    SongImportTests()
        : juce::UnitTest ("Song import", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        const test::ScopedTempDirectory temp;
        directory = temp.get();

        runAcceptedFormats();
        runRejections();
        runMp3();
    }

private:
    juce::File directory;

    juce::File write (const juce::String& fileName, const juce::MemoryBlock& bytes)
    {
        const auto file = directory.getChildFile (fileName);
        expect (bytes.getSize() == 0 ? file.deleteFile() && file.create().wasOk()
                                     : file.replaceWithData (bytes.getData(), bytes.getSize()));
        return file;
    }

    ImportResult validateWav (const WavSpec& spec, const juce::String& fileName = "test.wav")
    {
        return validateSourceFile (write (fileName, makeWav (spec)));
    }

    void expectRejected (const ImportResult& result, ImportProblem problem, const juce::String& messageContains)
    {
        expect (! result.accepted());
        expect (result.problem == problem, result.message);
        expect (result.message.containsIgnoreCase (messageContains), result.message);
    }

    void runAcceptedFormats()
    {
        beginTest ("WAV PCM 16, 24 and 32-bit and float32 are accepted with their properties");
        {
            for (const auto& [tag, bits] :
                 std::vector<std::pair<int, int>> { { 1, 16 }, { 1, 24 }, { 1, 32 }, { 3, 32 } })
            {
                WavSpec spec;
                spec.formatTag = tag;
                spec.bits = bits;
                spec.channels = 2;
                spec.sampleRate = 48000;
                spec.frames = 4800;
                const auto result = validateWav (spec);
                expect (result.accepted(), result.message);

                if (result.accepted())
                {
                    expectEquals (result.info.format, juce::String ("wav"));
                    expectEquals (result.info.channels, 2);
                    expectWithinAbsoluteError (result.info.sampleRate, 48000.0, 1.0e-6);
                    expect (result.info.frames == 4800);
                    expectWithinAbsoluteError (result.info.durationSeconds, 0.1, 1.0e-9);
                    expectEquals (result.info.bitsPerSample, bits);
                    expect (result.info.floatingPoint == (tag == 3));
                    expect (result.info.sizeBytes == result.info.file.getSize());
                }
            }
        }

        beginTest ("Mono, 192 kHz, extensible headers, and upper-case extensions are accepted");
        {
            WavSpec mono;
            mono.channels = 1;
            expectEquals (validateWav (mono).info.channels, 1);

            WavSpec fast;
            fast.sampleRate = 192000;
            expect (validateWav (fast).accepted());

            WavSpec extensible;
            extensible.formatTag = 0xFFFE;
            extensible.subFormatTag = 3;
            extensible.bits = 32;
            const auto result = validateWav (extensible);
            expect (result.accepted(), result.message);
            expect (result.info.floatingPoint);

            expect (validateWav (WavSpec {}, "LOUD.WAV").accepted());
        }

        beginTest ("A WAV written by JUCE is accepted");
        {
            const auto file = directory.getChildFile ("juce.wav");
            {
                juce::WavAudioFormat format;
                std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer = format.createWriterFor (
                    stream,
                    juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (
                        24));
                expect (writer != nullptr);

                if (writer != nullptr)
                {
                    juce::AudioBuffer<float> buffer (2, 1000);
                    buffer.clear();
                    writer->writeFromAudioSampleBuffer (buffer, 0, 1000);
                }
            }
            const auto result = validateSourceFile (file);
            expect (result.accepted(), result.message);
            expect (result.info.frames == 1000);
        }
    }

    void runRejections()
    {
        beginTest ("The 30-minute source limit is inclusive");
        for (int extraFrames : { 0, 1 })
        {
            WavSpec spec;
            spec.channels = 1;
            spec.sampleRate = 8000;
            const auto file = write ("duration.wav", makeWav (spec));
            const auto frames = 8000 * 30 * 60 + extraFrames;
            const auto dataBytes = frames * 2;
            {
                juce::FileOutputStream out (file);
                expect (out.openedOk());
                out.setPosition (4);
                out.writeInt (36 + dataBytes);
                out.setPosition (40);
                out.writeInt (dataBytes);
                out.setPosition (44 + dataBytes - 1);
                out.writeByte (0);
            }
            const auto result = validateSourceFile (file);
            if (extraFrames == 0)
                expect (result.accepted(), result.message);
            else
                expectRejected (result, ImportProblem::tooLong, "30 minutes");
        }

        beginTest ("Multichannel, very high sample rates, and zero length are rejected");
        {
            WavSpec surround;
            surround.channels = 6;
            expectRejected (validateWav (surround), ImportProblem::tooManyChannels, "6 channels");

            WavSpec huge;
            huge.sampleRate = 384000;
            expectRejected (validateWav (huge), ImportProblem::sampleRateTooHigh, "384.0 kHz");

            for (const int lowRate : { 4000, 7999 })
            {
                WavSpec low;
                low.sampleRate = lowRate;
                expectRejected (validateWav (low), ImportProblem::sampleRateTooLow, "8 kHz");
            }
            WavSpec minimumRate;
            minimumRate.sampleRate = 8000;
            expect (validateWav (minimumRate).accepted(), "8 kHz is supported at the inclusive lower boundary");

            WavSpec silent;
            silent.frames = 0;
            expectRejected (validateWav (silent), ImportProblem::zeroLength, "empty");
        }

        beginTest ("Compressed WAV codecs and unsupported bit depths are rejected");
        {
            WavSpec adpcm;
            adpcm.formatTag = 2;
            adpcm.bits = 4;
            expectRejected (validateWav (adpcm), ImportProblem::unsupportedCodec, "ADPCM");

            WavSpec alaw;
            alaw.formatTag = 6;
            alaw.bits = 8;
            expectRejected (validateWav (alaw), ImportProblem::unsupportedCodec, "A-law");

            WavSpec extensibleAdpcm;
            extensibleAdpcm.formatTag = 0xFFFE;
            extensibleAdpcm.subFormatTag = 0x11;
            extensibleAdpcm.bits = 16;
            expectRejected (validateWav (extensibleAdpcm), ImportProblem::unsupportedCodec, "IMA ADPCM");

            WavSpec eightBit;
            eightBit.bits = 8;
            expectRejected (validateWav (eightBit), ImportProblem::unsupportedBitDepth, "8-bit PCM");

            WavSpec float64;
            float64.formatTag = 3;
            float64.bits = 64;
            expectRejected (validateWav (float64), ImportProblem::unsupportedBitDepth, "64-bit float");

            WavSpec float16;
            float16.formatTag = 3;
            float16.bits = 16;
            expectRejected (validateWav (float16), ImportProblem::unsupportedBitDepth, "16-bit float");
        }

        beginTest ("Unreadable, empty, missing, and other-format files are rejected with advice");
        {
            const auto truncated = makeWav (WavSpec {});
            expectRejected (validateSourceFile (write ("short.wav", juce::MemoryBlock (truncated.getData(), 20))),
                            ImportProblem::unreadable,
                            "valid WAV");

            WavSpec notRiff;
            notRiff.riff = false;
            expectRejected (validateWav (notRiff, "junk.wav"), ImportProblem::unreadable, "valid WAV");

            expectRejected (validateSourceFile (write ("zero.wav", {})), ImportProblem::empty, "empty");
            expectRejected (
                validateSourceFile (directory.getChildFile ("nope.wav")), ImportProblem::notFound, "not found");

            juce::MemoryBlock text ("hello", 5);
            expectRejected (
                validateSourceFile (write ("song.flac", text)), ImportProblem::unsupportedExtension, "WAV and MP3");
            expectRejected (
                validateSourceFile (write ("noext", text)), ImportProblem::unsupportedExtension, "WAV and MP3");
            expectRejected (validateSourceFile (write ("garbage.mp3", text)), ImportProblem::unreadable, "damaged");
            expectRejected (validateSourceFile (directory), ImportProblem::notFound, "not found");
        }
    }

    void runMp3()
    {
        beginTest ("The synthetic MP3 fixture is accepted; JUCE's decoded length is recorded");
        const juce::File fixture (juce::String (juce::CharPointer_UTF8 (SNDS_TESTDATA_DIR)).replace ("/", "\\"));
        const auto mp3 = fixture.getChildFile ("tone-stereo-44100.mp3");
        expect (mp3.existsAsFile(), mp3.getFullPathName());

        const auto result = validateSourceFile (mp3);
        expect (result.accepted(), result.message);

        if (! result.accepted())
            return;

        expectEquals (result.info.format, juce::String ("mp3"));
        expectEquals (result.info.channels, 2);
        expectWithinAbsoluteError (result.info.sampleRate, 44100.0, 1.0e-6);

        // libsndfile 1.2.0 reports 22050 frames for this file (it applies the LAME gapless header).
        // The difference below is the encoder delay/padding the Phase 3+ alignment must account for.
        constexpr juce::int64 libsndfileFrames = 22050;
        logMessage ("MP3 fixture: JUCE decoded " + juce::String (result.info.frames) + " frames, libsndfile "
                    + juce::String (libsndfileFrames) + " (difference "
                    + juce::String (result.info.frames - libsndfileFrames) + ")");

        expect (std::abs (result.info.frames - libsndfileFrames) < 4608, "within four MPEG frames of libsndfile");

        // Decoding the whole file yields the length the header scan promised.
        juce::AudioFormatManager formats;
        formats.registerFormat (new juce::MP3AudioFormat(), true);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (mp3));
        expect (reader != nullptr);

        if (reader != nullptr)
        {
            juce::AudioBuffer<float> buffer (2, static_cast<int> (reader->lengthInSamples));
            expect (reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true));
            expect (buffer.getMagnitude (0, buffer.getNumSamples()) > 0.1f, "the decoded audio is not silent");
        }
    }
};

SongImportTests songImportTests;
} // namespace
} // namespace snds
