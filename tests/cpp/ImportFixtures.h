#pragma once

#include "TestUtilities.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <cstring>
#include <vector>

/**
    Runtime fixtures for the import tests: hand-built WAV files (every variant, including ones JUCE
    cannot write), ID3v2 tags, and small images. Committed MP3 fixtures live in testdata/import
    (see testdata/README.md).
*/
namespace snds::test
{

/** testdata/ in the source tree; SNDS_TESTDATA_DIR is defined by tests/cpp/CMakeLists.txt. */
inline juce::File getTestDataDirectory()
{
    return juce::File (juce::CharPointer_UTF8 (SNDS_TESTDATA_DIR));
}

inline juce::File getImportFixture (const char* name)
{
    return getTestDataDirectory().getChildFile ("import").getChildFile (name);
}

/** Test signal in [-0.5, 0.5]: a sine at 440 Hz on channel 0, +110 Hz per further channel. */
inline double testSignal (int channel, juce::int64 frame, double sampleRate)
{
    return 0.5
           * std::sin (juce::MathConstants<double>::twoPi * (440.0 + 110.0 * channel) * static_cast<double> (frame)
                       / sampleRate);
}

//==============================================================================
/** Little-endian byte assembly. */
class ByteWriter
{
public:
    ByteWriter& text (const char* fourOrMoreChars)
    {
        stream.write (fourOrMoreChars, std::strlen (fourOrMoreChars));
        return *this;
    }

    ByteWriter& u8 (int value)
    {
        stream.writeByte (static_cast<char> (value & 0xff));
        return *this;
    }

    ByteWriter& u16 (int value)
    {
        stream.writeShort (static_cast<short> (value & 0xffff));
        return *this;
    }

    ByteWriter& u24 (int value) { return u8 (value).u8 (value >> 8).u8 (value >> 16); }

    ByteWriter& u32 (juce::int64 value)
    {
        stream.writeInt (static_cast<int> (static_cast<juce::uint32> (value)));
        return *this;
    }

    ByteWriter& u64 (juce::int64 value)
    {
        stream.writeInt64 (value);
        return *this;
    }

    ByteWriter& f32 (float value)
    {
        stream.writeFloat (value);
        return *this;
    }

    ByteWriter& f64 (double value)
    {
        stream.writeDouble (value);
        return *this;
    }

    ByteWriter& bytes (const void* data, size_t size)
    {
        stream.write (data, size);
        return *this;
    }

    ByteWriter& bytes (const juce::MemoryBlock& block) { return bytes (block.getData(), block.getSize()); }

    juce::MemoryBlock getBlock() const { return stream.getMemoryBlock(); }
    size_t getSize() const { return stream.getDataSize(); }

private:
    juce::MemoryOutputStream stream;
};

//==============================================================================
struct WavSpec
{
    juce::uint16 formatTag = 1; ///< Tag written in the fmt chunk (or the extensible sub-format).
    bool extensible = false;
    bool knownSubFormatGuid = true;
    bool rf64 = false;
    bool rf64DataSizeMinusOne = true; ///< RF64 data chunk declares 0xFFFFFFFF (EBU convention) or the real size.
    int numChannels = 2;
    juce::uint32 sampleRate = 44100;
    int bitsPerSample = 16;
    int validBitsPerSample = 0;
    juce::int64 numFrames = 4410;
    juce::int64 declaredDataBytes = -1; ///< Overrides the data chunk size (simulates truncation).
    juce::MemoryBlock id3Payload;       ///< Adds an ID3 chunk when non-empty.
    bool id3ChunkUpperCase = false;     ///< "ID3 " instead of "id3 ".
    bool id3BeforeData = false;
};

/** Encodes testSignal() in the spec's sample format (unknown formats get zero bytes). */
inline juce::MemoryBlock makeWavSamples (const WavSpec& spec)
{
    ByteWriter out;
    const auto isFloat = spec.formatTag == 3;

    for (juce::int64 frame = 0; frame < spec.numFrames; ++frame)
    {
        for (auto channel = 0; channel < spec.numChannels; ++channel)
        {
            const auto value = testSignal (channel, frame, spec.sampleRate);

            if (isFloat && spec.bitsPerSample == 32)
                out.f32 (static_cast<float> (value));
            else if (isFloat && spec.bitsPerSample == 64)
                out.f64 (value);
            else if (spec.formatTag == 1 && spec.bitsPerSample == 8)
                out.u8 (juce::roundToInt (value * 127.0) + 128);
            else if (spec.formatTag == 1 && spec.bitsPerSample == 16)
                out.u16 (juce::roundToInt (value * 32767.0));
            else if (spec.formatTag == 1 && spec.bitsPerSample == 24)
                out.u24 (juce::roundToInt (value * 8388607.0));
            else if (spec.formatTag == 1 && spec.bitsPerSample == 32)
                out.u32 (static_cast<juce::int64> (std::llround (value * 2147483647.0)));
            else
                for (auto i = 0; i < juce::jmax (1, spec.bitsPerSample / 8); ++i)
                    out.u8 (0);
        }
    }

    return out.getBlock();
}

/** Builds a complete RIFF (or RF64) WAVE file in memory. */
inline juce::MemoryBlock makeWav (const WavSpec& spec)
{
    const auto samples = makeWavSamples (spec);
    const auto dataBytes = static_cast<juce::int64> (samples.getSize());
    const auto blockAlign = spec.numChannels * spec.bitsPerSample / 8;

    ByteWriter fmt;
    fmt.u16 (spec.extensible ? 0xfffe : spec.formatTag)
        .u16 (spec.numChannels)
        .u32 (spec.sampleRate)
        .u32 (static_cast<juce::int64> (spec.sampleRate) * blockAlign)
        .u16 (blockAlign)
        .u16 (spec.bitsPerSample);

    if (spec.extensible)
    {
        const auto mask = spec.numChannels == 1 ? 0x4 : (1 << spec.numChannels) - 1;
        fmt.u16 (22).u16 (spec.validBitsPerSample > 0 ? spec.validBitsPerSample : spec.bitsPerSample).u32 (mask);
        fmt.u32 (spec.formatTag).u16 (0).u16 (spec.knownSubFormatGuid ? 0x10 : 0x11);
        const juce::uint8 tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
        fmt.bytes (tail, sizeof (tail));
    }

    ByteWriter body;
    body.text ("WAVE");

    if (spec.rf64)
        body.text ("ds64").u32 (28).u64 (0).u64 (dataBytes).u64 (spec.numFrames).u32 (0);

    body.text ("fmt ").u32 (static_cast<juce::int64> (fmt.getSize())).bytes (fmt.getBlock());

    const auto writeId3 = [&]
    {
        if (spec.id3Payload.isEmpty())
            return;

        body.text (spec.id3ChunkUpperCase ? "ID3 " : "id3 ").u32 (static_cast<juce::int64> (spec.id3Payload.getSize()));
        body.bytes (spec.id3Payload);

        if ((spec.id3Payload.getSize() & 1) != 0)
            body.u8 (0);
    };

    if (spec.id3BeforeData)
        writeId3();

    const auto declared = spec.declaredDataBytes >= 0 ? spec.declaredDataBytes : dataBytes;
    body.text ("data").u32 (spec.rf64 && spec.rf64DataSizeMinusOne ? 0xffffffff : declared).bytes (samples);

    if ((dataBytes & 1) != 0)
        body.u8 (0);

    if (! spec.id3BeforeData)
        writeId3();

    ByteWriter file;
    file.text (spec.rf64 ? "RF64" : "RIFF").u32 (spec.rf64 ? 0xffffffff : static_cast<juce::int64> (body.getSize()));
    file.bytes (body.getBlock());
    auto result = file.getBlock();

    if (spec.rf64) // the real RIFF size lives in ds64 (at byte 20)
    {
        const auto riffSize = static_cast<juce::int64> (body.getSize());
        std::memcpy (static_cast<char*> (result.getData()) + 20, &riffSize, sizeof (riffSize));
    }

    return result;
}

inline juce::File writeFile (const ScopedTempDirectory& temp, const juce::String& name, const juce::MemoryBlock& data)
{
    const auto file = temp.get().getChildFile (name);
    file.getParentDirectory().createDirectory();

    if (data.isEmpty())
        file.create(); // replaceWithData() deletes the file for empty data
    else
        file.replaceWithData (data.getData(), data.getSize());

    return file;
}

/** Writes a WAV through JUCE's own writer (the most common real-world producer here). */
inline juce::File writeJuceWav (const ScopedTempDirectory& temp,
                                const juce::String& name,
                                double sampleRate,
                                int numChannels,
                                int bitsPerSample,
                                bool floatingPoint,
                                int numFrames)
{
    const auto file = temp.get().getChildFile (name);
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (
        stream,
        juce::AudioFormatWriterOptions {}
            .withSampleRate (sampleRate)
            .withNumChannels (numChannels)
            .withBitsPerSample (bitsPerSample)
            .withSampleFormat (floatingPoint ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                             : juce::AudioFormatWriterOptions::SampleFormat::integral));

    if (writer == nullptr)
        return {};

    juce::AudioBuffer<float> buffer (numChannels, numFrames);

    for (auto channel = 0; channel < numChannels; ++channel)
        for (auto i = 0; i < numFrames; ++i)
            buffer.setSample (channel, i, static_cast<float> (testSignal (channel, i, sampleRate)));

    writer->writeFromAudioSampleBuffer (buffer, 0, numFrames);
    return file;
}

//==============================================================================
inline juce::MemoryBlock encodeImage (juce::ImageFileFormat& format, int width, int height, juce::Colour colour)
{
    juce::Image image (juce::Image::RGB, width, height, false, juce::SoftwareImageType());
    juce::Graphics g (image);
    g.fillAll (colour);
    // A little structure so the encoder output is not trivially uniform.
    g.setColour (colour.contrasting());
    g.fillRect (0, 0, juce::jmax (1, width / 4), juce::jmax (1, height / 4));

    juce::MemoryOutputStream out;
    format.writeImageToStream (image, out);
    return out.getMemoryBlock();
}

inline juce::MemoryBlock makePng (int width, int height, juce::Colour colour)
{
    juce::PNGImageFormat png;
    return encodeImage (png, width, height, colour);
}

inline juce::MemoryBlock makeJpeg (int width, int height, juce::Colour colour)
{
    juce::JPEGImageFormat jpeg;
    return encodeImage (jpeg, width, height, colour);
}

//==============================================================================
inline void writeSyncsafe (ByteWriter& out, juce::uint32 value)
{
    out.u8 (static_cast<int> ((value >> 21) & 0x7f))
        .u8 (static_cast<int> ((value >> 14) & 0x7f))
        .u8 (static_cast<int> ((value >> 7) & 0x7f))
        .u8 (static_cast<int> (value & 0x7f));
}

inline void writeBigEndian (ByteWriter& out, juce::uint32 value, int numBytes)
{
    for (auto shift = (numBytes - 1) * 8; shift >= 0; shift -= 8)
        out.u8 (static_cast<int> ((value >> shift) & 0xff));
}

/** ID3 unsynchronisation (inserts 0x00 after 0xFF when followed by 0x00/0xE0+ or at the end). */
inline juce::MemoryBlock unsynchronise (const juce::MemoryBlock& data)
{
    ByteWriter out;
    const auto* bytes = static_cast<const juce::uint8*> (data.getData());

    for (size_t i = 0; i < data.getSize(); ++i)
    {
        out.u8 (bytes[i]);

        if (bytes[i] == 0xff && (i + 1 == data.getSize() || bytes[i + 1] == 0x00 || bytes[i + 1] >= 0xe0))
            out.u8 (0);
    }

    return out.getBlock();
}

/** Body of an APIC (v2.3/2.4) frame with an ISO-8859-1 description. */
inline juce::MemoryBlock
apicBody (const char* mime, int pictureType, const char* description, const juce::MemoryBlock& image)
{
    ByteWriter out;
    out.u8 (0).text (mime).u8 (0).u8 (pictureType).text (description).u8 (0).bytes (image);
    return out.getBlock();
}

struct Id3Frame
{
    juce::String id;
    juce::MemoryBlock body;
    juce::uint16 flags = 0;
};

struct Id3TagOptions
{
    int majorVersion = 3;
    bool unsynchronise = false; ///< v2.3: whole tag; v2.4: sets the header flag (frames must be pre-encoded).
    bool extendedHeader = false;
    bool plainSizesInV24 = false; ///< v2.4 tag written with v2.3-style (non-syncsafe) frame sizes.
    int padding = 16;
};

/** Assembles an ID3v2 tag. Frame bodies are written as given (callers pre-apply v2.4 frame flags). */
inline juce::MemoryBlock makeId3Tag (const std::vector<Id3Frame>& frames, const Id3TagOptions& options = {})
{
    ByteWriter body;

    if (options.extendedHeader)
    {
        if (options.majorVersion == 3)
            body.bytes ("\0\0\0\x06\0\0\0\0\0\0", 10); // size 6 (excludes itself), no flags, no padding
        else
            body.bytes ("\0\0\0\x06\x01\0", 6); // syncsafe size 6 (includes itself), one flag byte
    }

    for (const auto& frame : frames)
    {
        body.text (frame.id.toRawUTF8());
        const auto size = static_cast<juce::uint32> (frame.body.getSize());

        if (options.majorVersion == 2)
            writeBigEndian (body, size, 3);
        else if (options.majorVersion == 4 && ! options.plainSizesInV24)
            writeSyncsafe (body, size);
        else
            writeBigEndian (body, size, 4);

        if (options.majorVersion > 2)
            writeBigEndian (body, frame.flags, 2);

        body.bytes (frame.body);
    }

    for (auto i = 0; i < options.padding; ++i)
        body.u8 (0);

    auto content = body.getBlock();

    if (options.unsynchronise && options.majorVersion < 4)
        content = unsynchronise (content);

    auto flags = 0;
    flags |= options.unsynchronise ? 0x80 : 0;
    flags |= options.extendedHeader ? 0x40 : 0;

    ByteWriter tag;
    tag.text ("ID3").u8 (options.majorVersion).u8 (0).u8 (flags);
    writeSyncsafe (tag, static_cast<juce::uint32> (content.getSize()));
    tag.bytes (content);
    return tag.getBlock();
}

/** Bytes that exercise unsynchronisation: false syncs, 0xFF 0x00 pairs, and a trailing 0xFF. */
inline juce::MemoryBlock unsyncTorturePayload()
{
    ByteWriter out;
    const juce::uint8 pattern[] = { 0x89, 'P',  'N',  'G',  0xff, 0x00, 0xff, 0xe0,
                                    0xff, 0xfb, 0x12, 0xff, 0xff, 0x00, 0xff };

    for (auto i = 0; i < 20; ++i)
        out.bytes (pattern, sizeof (pattern));

    return out.getBlock();
}

} // namespace snds::test
