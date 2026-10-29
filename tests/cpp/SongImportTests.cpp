#include "ImportFixtures.h"
#include "app/SongImport.h"
#include "audio/SourceFormats.h"

#include <limits>
#include <vector>

namespace snds::test
{

class SongImportTests final : public juce::UnitTest
{
public:
    SongImportTests()
        : juce::UnitTest ("Song import", testCategory)
    {
    }

    void runTest() override
    {
        const ScopedTempDirectory temp;

        beginTest ("WAV PCM and float variants are accepted and decode correctly");
        {
            struct Variant
            {
                juce::uint16 tag;
                int bits;
                bool extensible;
                const char* encoding;
                double tolerance;
            };

            const Variant variants[] = {
                { 1, 8, false, "8-bit PCM", 0.02 },       { 1, 16, false, "16-bit PCM", 1.0e-4 },
                { 1, 24, false, "24-bit PCM", 1.0e-6 },   { 1, 32, false, "32-bit PCM", 1.0e-6 },
                { 3, 32, false, "32-bit float", 1.0e-6 }, { 3, 64, false, "64-bit float", 1.0e-6 },
                { 1, 24, true, "24-bit PCM", 1.0e-6 },    { 3, 32, true, "32-bit float", 1.0e-6 },
                { 3, 64, true, "64-bit float", 1.0e-6 },
            };

            for (const auto& variant : variants)
            {
                for (const auto channels : { 1, 2 })
                {
                    WavSpec spec;
                    spec.formatTag = variant.tag;
                    spec.bitsPerSample = variant.bits;
                    spec.extensible = variant.extensible;
                    spec.numChannels = channels;
                    spec.sampleRate = 48000;
                    spec.numFrames = 4801;

                    const auto fileName = juce::String (variant.encoding).replace (" ", "_")
                                          + (variant.extensible ? "_ext" : "") + "_" + juce::String (channels)
                                          + "ch.wav";
                    const auto file = writeFile (temp, fileName, makeWav (spec));
                    expectAccepted (file, spec, variant.encoding, variant.tolerance);
                }
            }
        }

        beginTest ("RF64 WAV: accepted when JUCE can read it, otherwise rejected with guidance");
        {
            WavSpec spec;
            spec.rf64 = true;
            spec.rf64DataSizeMinusOne = false;
            spec.numFrames = 1000;
            auto result = inspectSource (writeFile (temp, "rf64.wav", makeWav (spec)));
            expect (result.wasOk(), result.error.message);

            if (result.wasOk())
                expect (result.source->encoding.contains ("RF64"), result.source->encoding);

            // A small RF64 whose data chunk declares 0xFFFFFFFF: JUCE's reader refuses it.
            spec.rf64DataSizeMinusOne = true;
            result = inspectSource (writeFile (temp, "rf64-minus-one.wav", makeWav (spec)));
            expectRejected (result, ImportErrorCode::unsupportedEncoding);
            expect (result.error.message.contains ("RF64"), result.error.message);

            // The SNDS 64-bit float reader follows ds64, so that variant reads either way.
            spec.formatTag = 3;
            spec.bitsPerSample = 64;
            result = inspectSource (writeFile (temp, "rf64-float64.wav", makeWav (spec)));
            expect (result.wasOk(), result.error.message);
        }

        beginTest ("JUCE-written WAVs from 8 kHz to 192 kHz are accepted");
        {
            for (const auto rate : { 8000.0, 22050.0, 44100.0, 96000.0, 176400.0, 192000.0 })
            {
                const auto file =
                    writeJuceWav (temp, "juce_" + juce::String (rate, 0) + ".wav", rate, 2, 24, false, 2000);
                const auto result = inspectSource (file);
                expect (result.wasOk(), result.error.message);

                if (result.wasOk())
                {
                    expectEquals (result.source->sampleRate, rate);
                    expectEquals (result.source->lengthInFrames, static_cast<juce::int64> (2000));
                }
            }

            const auto floatFile = writeJuceWav (temp, "juce_float.wav", 44100.0, 1, 32, true, 1000);
            const auto result = inspectSource (floatFile);
            expect (result.wasOk() && result.source->floatingPoint, result.error.message);
        }

        beginTest ("Multichannel WAV is rejected with a downmix hint");
        {
            for (const auto channels : { 3, 4, 6 })
            {
                WavSpec spec;
                spec.numChannels = channels;
                spec.extensible = channels > 2;
                const auto result =
                    inspectSource (writeFile (temp, "multi" + juce::String (channels) + ".wav", makeWav (spec)));
                expectRejected (result, ImportErrorCode::tooManyChannels);
                expect (result.error.message.contains (juce::String (channels) + " channels"), result.error.message);
                expect (result.error.action.contains ("stereo"), result.error.action);
            }

            const auto juceFourChannels = writeJuceWav (temp, "juce_4ch.wav", 48000.0, 4, 16, false, 500);
            expectRejected (inspectSource (juceFourChannels), ImportErrorCode::tooManyChannels);
        }

        beginTest ("Sample rates outside 8 kHz to 192 kHz are rejected");
        {
            for (const auto rate : { 192001u, 384000u, 4000u })
            {
                WavSpec spec;
                spec.sampleRate = rate;
                spec.numFrames = 100;
                const auto result =
                    inspectSource (writeFile (temp, "rate" + juce::String (rate) + ".wav", makeWav (spec)));
                expectRejected (result, ImportErrorCode::sampleRateOutOfRange);
            }

            const auto juce384 = writeJuceWav (temp, "juce_384k.wav", 384000.0, 2, 24, false, 1000);
            const auto result = inspectSource (juce384);
            expectRejected (result, ImportErrorCode::sampleRateOutOfRange);
            expect (result.error.message.contains ("384 kHz"), result.error.message);
            expect (result.error.action.contains ("192 kHz"), result.error.action);
        }

        beginTest ("Compressed and unusual WAV encodings are rejected");
        {
            WavSpec adpcm;
            adpcm.formatTag = 2;
            adpcm.bitsPerSample = 4;
            auto result = inspectSource (writeFile (temp, "adpcm.wav", makeWav (adpcm)));
            expectRejected (result, ImportErrorCode::unsupportedEncoding);
            expect (result.error.message.contains ("ADPCM"), result.error.message);

            WavSpec mp3InWav;
            mp3InWav.formatTag = 0x55;
            expectRejected (inspectSource (writeFile (temp, "mp3inwav.wav", makeWav (mp3InWav))),
                            ImportErrorCode::unsupportedEncoding);

            WavSpec twentyBit;
            twentyBit.bitsPerSample = 20;
            expectRejected (inspectSource (writeFile (temp, "pcm20.wav", makeWav (twentyBit))),
                            ImportErrorCode::unsupportedBitDepth);

            WavSpec float16;
            float16.formatTag = 3;
            float16.bitsPerSample = 16;
            expectRejected (inspectSource (writeFile (temp, "float16.wav", makeWav (float16))),
                            ImportErrorCode::unsupportedBitDepth);

            WavSpec unknownGuid;
            unknownGuid.extensible = true;
            unknownGuid.knownSubFormatGuid = false;
            expectRejected (inspectSource (writeFile (temp, "guid.wav", makeWav (unknownGuid))),
                            ImportErrorCode::unsupportedEncoding);

            WavSpec twentyInTwentyFour;
            twentyInTwentyFour.extensible = true;
            twentyInTwentyFour.bitsPerSample = 24;
            twentyInTwentyFour.validBitsPerSample = 20;
            result = inspectSource (writeFile (temp, "pcm20in24.wav", makeWav (twentyInTwentyFour)));
            expect (result.wasOk(), result.error.message);

            if (result.wasOk())
                expectEquals (result.source->encoding, juce::String ("20-bit PCM in 24-bit"));
        }

        beginTest ("Empty, truncated, and corrupt files are rejected");
        {
            WavSpec empty;
            empty.numFrames = 0;
            expectRejected (inspectSource (writeFile (temp, "zero.wav", makeWav (empty))), ImportErrorCode::noAudio);

            WavSpec truncated;
            truncated.declaredDataBytes = 10 * 1000 * 1000;
            auto result = inspectSource (writeFile (temp, "truncated.wav", makeWav (truncated)));
            expectRejected (result, ImportErrorCode::truncated);
            expect (result.error.message.contains ("incomplete"), result.error.message);

            expectRejected (inspectSource (writeFile (temp, "empty.wav", {})), ImportErrorCode::noAudio);
            expectRejected (inspectSource (writeFile (temp, "empty.mp3", {})), ImportErrorCode::noAudio);

            juce::MemoryBlock noise (64 * 1024);
            juce::Random random (1234);
            random.fillBitsRandomly (noise.getData(), noise.getSize());
            expectRejected (inspectSource (writeFile (temp, "noise.wav", noise)), ImportErrorCode::corrupt);
            expectRejected (inspectSource (writeFile (temp, "noise.mp3", noise)), ImportErrorCode::corrupt);

            // An MP3 cut off after its ID3 tag and a few bytes of the first frame.
            juce::MemoryBlock cut;
            expect (getImportFixture ("tone-stereo-44k1-apic.mp3").loadFileAsData (cut));
            cut.setSize (3912 + 100);
            expectRejected (inspectSource (writeFile (temp, "cut.mp3", cut)), ImportErrorCode::corrupt);

            WavSpec noFormat;
            auto wav = makeWav (noFormat);
            std::memcpy (static_cast<char*> (wav.getData()) + 12, "junk", 4); // rename the fmt chunk
            expectRejected (inspectSource (writeFile (temp, "nofmt.wav", wav)), ImportErrorCode::corrupt);
        }

        beginTest ("Missing files, folders, and other formats are rejected with guidance");
        {
            expectRejected (inspectSource (temp.get().getChildFile ("missing.wav")), ImportErrorCode::fileNotFound);

            const auto folder = temp.get().getChildFile ("folder.wav");
            folder.createDirectory();
            expectRejected (inspectSource (folder), ImportErrorCode::fileNotFound);

            auto result = inspectSource (writeFile (temp, "song.flac", juce::MemoryBlock ("fLaC\0\0\0\x22", 8)));
            expectRejected (result, ImportErrorCode::unsupportedFileType);
            expect (result.error.message.contains ("FLAC"), result.error.message);

            expectRejected (inspectSource (temp.createFile ("notes.txt", "hello")),
                            ImportErrorCode::unsupportedFileType);

            juce::MemoryBlock mp3;
            expect (getImportFixture ("tone-stereo-44k1.mp3").loadFileAsData (mp3));
            result = inspectSource (writeFile (temp, "actually-mp3.wav", mp3));
            expectRejected (result, ImportErrorCode::formatMismatch);
            expect (result.error.action.contains ("Rename it to .mp3"), result.error.action);

            result = inspectSource (writeFile (temp, "actually-wav.mp3", makeWav ({})));
            expectRejected (result, ImportErrorCode::formatMismatch);
            expect (result.error.action.contains (".wav"), result.error.action);

            for (auto code = 0; code <= static_cast<int> (ImportErrorCode::corrupt); ++code)
                expect (juce::String (toString (static_cast<ImportErrorCode> (code))) != "unknown");
        }

        beginTest ("MP3 fixtures are accepted with exact decoded lengths");
        {
            {
                const auto result = inspectSource (getImportFixture ("tone-stereo-44k1.mp3"));
                expect (result.wasOk(), result.error.message);

                if (result.wasOk())
                {
                    const auto& source = *result.source;
                    expect (source.format == model::SourceFormat::mp3);
                    expectEquals (source.sampleRate, 44100.0);
                    expectEquals (source.numChannels, 2);
                    expect (source.mp3.has_value());
                    expect (! source.mp3->hasXingHeader);
                    expectEquals (source.mp3->encoderDelay, -1);
                    expectEquals (source.mp3->bitrateKbps, 128);
                    expect (! source.mp3->variableBitrate);
                    expectEquals (source.lengthInFrames, source.mp3->audioFrameCount * 1152);
                    expect (source.lengthInFrames >= 44100 && source.lengthInFrames < 44100 + 3 * 1152,
                            juce::String (source.lengthInFrames));
                    expect (source.getSummary().startsWith ("MP3, MPEG-1 Layer III, 128 kbps, 44.1 kHz, stereo, 0:01"),
                            source.getSummary());
                    expectReadsToEnd (source);
                }
            }

            {
                // libsndfile/LAME output: Xing + LAME tag, 43 audio frames, delay 576, padding 960.
                const auto result = inspectSource (getImportFixture ("tone-stereo-48k-lametag.mp3"));
                expect (result.wasOk(), result.error.message);

                if (result.wasOk())
                {
                    const auto& mp3 = *result.source->mp3;
                    expect (mp3.hasXingHeader);
                    expectEquals (mp3.audioFrameCount, static_cast<juce::int64> (43));
                    expectEquals (result.source->lengthInFrames, static_cast<juce::int64> (43 * 1152));
                    expectEquals (mp3.encoderDelay, 576);
                    expectEquals (mp3.encoderPadding, 960);
                    expectEquals (result.source->lengthInFrames - mp3.encoderDelay - mp3.encoderPadding,
                                  static_cast<juce::int64> (48000));
                    expectReadsToEnd (*result.source);
                }
            }

            {
                const auto result = inspectSource (getImportFixture ("tone-mono-48k-v24-unsync.mp3"));
                expect (result.wasOk(), result.error.message);

                if (result.wasOk())
                {
                    expectEquals (result.source->numChannels, 1);
                    expectEquals (result.source->sampleRate, 48000.0);
                    expect (result.source->mp3->audioDataOffset > 0);
                    expectReadsToEnd (*result.source);
                }
            }
        }

        beginTest ("JUCE's MP3 decode lags the original by encoder delay + 529 samples");
        {
            // Pins decoder behaviour that Original A/B alignment depends on (see docs/spikes.md).
            // Fixture signals: 0.25 * sine, 10 ms linear fades, 16-bit, from testdata/make_fixtures.py.
            const auto tagged = inspectSource (getImportFixture ("tone-stereo-48k-lametag.mp3"));
            const auto tagless = inspectSource (getImportFixture ("tone-stereo-44k1.mp3"));
            expect (tagged.wasOk() && tagless.wasOk());

            if (tagged.wasOk() && tagless.wasOk())
            {
                auto taggedResidual = 0.0;
                auto taglessResidual = 0.0;
                const auto taggedLag = measureDecodeLag (*tagged.source, 220.0, 550.0, taggedResidual);
                const auto taglessLag = measureDecodeLag (*tagless.source, 440.0, 660.0, taglessResidual);
                logMessage ("    JUCE MP3 decode lag: LAME-tagged " + juce::String (taggedLag) + " (residual "
                            + juce::String (taggedResidual, 3) + "), tagless " + juce::String (taglessLag)
                            + " (residual " + juce::String (taglessResidual, 3) + ") samples");

                // JUCE ignores the LAME tag: both decode 576 (encoder) + 529 (decoder) samples late.
                expectEquals (taggedLag, 576 + 529);
                expectEquals (taglessLag, 576 + 529);
                expect (taggedResidual < 5.0 && taglessResidual < 5.0, "a clean alignment, not an alias");
            }
        }

        beginTest ("Low-sample-rate MP3 (MPEG-2) is rejected");
        {
            const auto result = inspectSource (getImportFixture ("tone-mono-22k05-mpeg2.mp3"));
            expectRejected (result, ImportErrorCode::unsupportedEncoding);
            expect (result.error.message.contains ("MPEG-2"), result.error.message);
            expect (result.error.action.contains ("44.1"), result.error.action);
        }

        beginTest ("MPEG frame headers");
        {
            const auto frame = audio::parseMpegFrameHeader (0xfffb9064u); // MPEG-1 L3 128k 44.1k stereo
            expect (frame.has_value());

            if (frame.has_value())
            {
                expectEquals (frame->bitrateKbps, 128);
                expectEquals (frame->sampleRate, 44100);
                expectEquals (frame->frameBytes, 417);
                expectEquals (frame->samplesPerFrame, 1152);
            }

            const auto lsf = audio::parseMpegFrameHeader (0xfff340c4u);
            expect (lsf.has_value() && lsf->version == audio::MpegVersion::mpeg2 && lsf->samplesPerFrame == 576);

            expect (! audio::parseMpegFrameHeader (0xfffb0064u).has_value(), "free format");
            expect (! audio::parseMpegFrameHeader (0xfffbf064u).has_value(), "bad bitrate");
            expect (! audio::parseMpegFrameHeader (0xfffb9c64u).has_value(), "reserved sample rate");
            expect (! audio::parseMpegFrameHeader (0x12345678u).has_value(), "no sync");
        }

        beginTest ("Durations and sample rates format for people");
        {
            expectEquals (model::formatDuration (0.4), juce::String ("0:00"));
            expectEquals (model::formatDuration (187.9), juce::String ("3:07"));
            expectEquals (model::formatDuration (3723.0), juce::String ("1:02:03"));
            expectEquals (model::formatSampleRate (44100.0), juce::String ("44.1 kHz"));
            expectEquals (model::formatSampleRate (48000.0), juce::String ("48 kHz"));
            expectEquals (model::formatSampleRate (11025.0), juce::String ("11.025 kHz"));
        }
    }

private:
    void expectRejected (const ImportResult& result, ImportErrorCode expected)
    {
        expect (! result.wasOk(), "expected rejection " + juce::String (toString (expected)));

        if (result.wasOk())
            return;

        expect (result.error.code == expected,
                juce::String ("expected ") + toString (expected) + ", got " + toString (result.error.code) + ": "
                    + result.error.message);
        expect (result.error.message.isNotEmpty() && result.error.action.isNotEmpty(), "errors must be actionable");
    }

    void expectAccepted (const juce::File& file, const WavSpec& spec, const juce::String& encoding, double tolerance)
    {
        const auto result = inspectSource (file);
        expect (result.wasOk(), file.getFileName() + ": " + result.error.message);

        if (! result.wasOk())
            return;

        const auto& source = *result.source;
        expect (source.format == model::SourceFormat::wav);
        expectEquals (source.encoding, encoding, file.getFileName());
        expectEquals (source.sampleRate, static_cast<double> (spec.sampleRate));
        expectEquals (source.numChannels, spec.numChannels);
        expectEquals (source.lengthInFrames, spec.numFrames);
        expectEquals (source.bitsPerSample, spec.bitsPerSample);
        expect (source.floatingPoint == (spec.formatTag == 3));
        expectEquals (source.fileSizeBytes, file.getSize());

        auto reader = createSourceReader (source);
        expect (reader != nullptr, "reader for " + file.getFileName());

        if (reader == nullptr)
            return;

        constexpr int frames = 256;
        juce::AudioBuffer<float> buffer (spec.numChannels, frames);
        expect (reader->read (&buffer, 0, frames, 1000, true, true));

        auto maxError = 0.0;

        for (auto channel = 0; channel < spec.numChannels; ++channel)
            for (auto i = 0; i < frames; ++i)
                maxError = juce::jmax (
                    maxError,
                    std::abs (buffer.getSample (channel, i) - testSignal (channel, 1000 + i, spec.sampleRate)));

        expect (maxError <= tolerance, file.getFileName() + " max error " + juce::String (maxError));
    }

    /** Reads the whole source through createSourceReader() and checks it holds audio throughout. */
    void expectReadsToEnd (const model::SourceInfo& source)
    {
        auto reader = createSourceReader (source);
        expect (reader != nullptr);

        if (reader == nullptr)
            return;

        expectEquals (reader->lengthInSamples, source.lengthInFrames);
        juce::AudioBuffer<float> buffer (source.numChannels, static_cast<int> (source.lengthInFrames));
        expect (reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true), "full read");

        const auto middle = buffer.getNumSamples() / 2;
        expect (buffer.getRMSLevel (0, middle - 2000, 4000) > 0.1f, "audible tone in the middle");
    }

    /** Lag (in samples) that best aligns the decoded MP3 with the known fixture signal. */
    int measureDecodeLag (const model::SourceInfo& source, double leftHz, double rightHz, double& residual)
    {
        residual = std::numeric_limits<double>::max();

        auto reader = createSourceReader (source);

        if (reader == nullptr)
            return -1;

        const auto rate = source.sampleRate;
        constexpr int window = 6000;
        constexpr int maxLag = 2400;
        juce::AudioBuffer<float> decoded (2, window + maxLag);
        reader->read (&decoded, 0, decoded.getNumSamples(), 0, true, true);

        const auto fade = static_cast<int> (rate) / 100;
        const auto frames = static_cast<int> (rate); // one second
        std::vector<float> expected[2];

        for (auto channel = 0; channel < 2; ++channel)
        {
            const auto hz = channel == 0 ? leftHz : rightHz;

            for (auto n = 0; n < window; ++n)
            {
                const auto gain =
                    juce::jmin (1.0, static_cast<double> (n) / fade, static_cast<double> (frames - 1 - n) / fade);
                const auto value =
                    0.25 * 32767.0 * gain * std::sin (juce::MathConstants<double>::twoPi * hz * n / rate);
                expected[channel].push_back (static_cast<float> (std::round (value) / 32768.0));
            }
        }

        auto bestLag = -1;
        auto bestError = std::numeric_limits<double>::max();

        for (auto lag = 0; lag < maxLag; ++lag)
        {
            auto error = 0.0;

            for (auto channel = 0; channel < 2; ++channel)
            {
                const auto* samples = decoded.getReadPointer (channel, lag);

                for (auto n = 0; n < window; ++n)
                {
                    const auto difference =
                        static_cast<double> (samples[n] - expected[channel][static_cast<size_t> (n)]);
                    error += difference * difference;
                }
            }

            if (error < bestError)
            {
                bestError = error;
                bestLag = lag;
            }
        }

        residual = bestError;
        return bestLag;
    }
};

static SongImportTests songImportTests;

} // namespace snds::test
