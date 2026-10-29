#include "audio/SourceFormats.h"

#include <bit>
#include <cstring>

namespace snds::audio
{

namespace
{
constexpr int maxChunksToWalk = 4096;
constexpr int maxLeadingId3Tags = 4;
constexpr juce::int64 maxJunkBeforeFirstFrame = 128 * 1024;
constexpr juce::int64 maxResyncDistance = 64 * 1024;

bool readChunkId (juce::InputStream& input, char (&id)[4])
{
    return input.read (id, 4) == 4;
}

bool idEquals (const char (&id)[4], const char* expected) noexcept
{
    return std::memcmp (id, expected, 4) == 0;
}

/** The standard WAVE sub-format GUID base {xxxxxxxx-0000-0010-8000-00aa00389b71}. */
bool hasStandardGuidBase (juce::uint16 data2, juce::uint16 data3, const juce::uint8 (&data4)[8]) noexcept
{
    static constexpr juce::uint8 base[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
    return data2 == 0x0000 && data3 == 0x0010 && std::memcmp (data4, base, sizeof (base)) == 0;
}

void readFormatChunk (juce::InputStream& input, juce::int64 chunkSize, WavLayout& layout)
{
    if (chunkSize < 16)
        return;

    layout.hasFormatChunk = true;
    layout.formatTag = static_cast<juce::uint16> (input.readShort());
    layout.numChannels = static_cast<juce::uint16> (input.readShort());
    layout.sampleRate = static_cast<juce::uint32> (input.readInt());
    input.readInt(); // average bytes per second: derived, not trusted
    layout.blockAlign = static_cast<juce::uint16> (input.readShort());
    layout.bitsPerSample = static_cast<juce::uint16> (input.readShort());
    layout.validBitsPerSample = layout.bitsPerSample;

    if (layout.formatTag != wavFormatTag::extensible)
        return;

    layout.extensible = true;

    if (chunkSize < 40)
    {
        layout.knownSubFormat = false;
        return;
    }

    input.readShort(); // cbSize
    const auto validBits = static_cast<juce::uint16> (input.readShort());
    input.readInt(); // channel mask

    const auto data1 = static_cast<juce::uint32> (input.readInt());
    const auto data2 = static_cast<juce::uint16> (input.readShort());
    const auto data3 = static_cast<juce::uint16> (input.readShort());
    juce::uint8 data4[8] {};
    input.read (data4, sizeof (data4));

    if (validBits > 0)
        layout.validBitsPerSample = validBits;

    layout.knownSubFormat = hasStandardGuidBase (data2, data3, data4) && data1 <= 0xffff;
    layout.formatTag = layout.knownSubFormat ? static_cast<juce::uint16> (data1) : wavFormatTag::extensible;
}

juce::uint32 bigEndianWord (const juce::uint8* bytes) noexcept
{
    return (static_cast<juce::uint32> (bytes[0]) << 24) | (static_cast<juce::uint32> (bytes[1]) << 16)
           | (static_cast<juce::uint32> (bytes[2]) << 8) | static_cast<juce::uint32> (bytes[3]);
}

std::optional<MpegFrameHeader> readFrameHeaderAt (juce::InputStream& input, juce::int64 position)
{
    juce::uint8 bytes[4];

    if (! input.setPosition (position) || input.read (bytes, 4) != 4)
        return std::nullopt;

    return parseMpegFrameHeader (bigEndianWord (bytes));
}

bool isSameStream (const MpegFrameHeader& a, const MpegFrameHeader& b) noexcept
{
    return a.version == b.version && a.layer == b.layer && a.sampleRate == b.sampleRate
           && a.numChannels == b.numChannels;
}

/**
    True when a valid frame at position is followed by a consistent frame (or ends exactly at the end
    of the stream). Requiring two linked frames rejects the false syncs found in random data and in
    embedded images.
*/
bool isFrameChainAt (juce::InputStream& input, juce::int64 position, juce::int64 totalLength)
{
    const auto frame = readFrameHeaderAt (input, position);

    if (! frame.has_value())
        return false;

    const auto next = position + frame->frameBytes;

    if (next == totalLength)
        return true;

    const auto following = readFrameHeaderAt (input, next);
    return following.has_value() && isSameStream (*frame, *following);
}

/** Searches [from, from + maxDistance) for the start of a frame chain; returns -1 if none is found. */
juce::int64 findFrameChain (juce::InputStream& input, juce::int64 from, juce::int64 maxDistance, juce::int64 total)
{
    const auto end = juce::jmin (total - 4, from + maxDistance);
    constexpr int windowSize = 8192;
    juce::uint8 window[windowSize + 1];

    for (auto windowStart = from; windowStart < end; windowStart += windowSize)
    {
        if (! input.setPosition (windowStart))
            return -1;

        const auto wanted = static_cast<int> (juce::jmin<juce::int64> (windowSize + 1, total - windowStart));
        const auto got = input.read (window, wanted);

        for (auto i = 0; i + 1 < got && windowStart + i < end; ++i)
        {
            if (window[i] != 0xff || (window[i + 1] & 0xe0) != 0xe0)
                continue;

            const auto candidate = windowStart + i;

            if (isFrameChainAt (input, candidate, total))
                return candidate;
        }
    }

    return -1;
}

bool startsWithTrailingTag (const juce::uint8* bytes) noexcept
{
    return std::memcmp (bytes, "TAG", 3) == 0 || std::memcmp (bytes, "APET", 4) == 0
           || std::memcmp (bytes, "LYRI", 4) == 0 || std::memcmp (bytes, "ID3", 3) == 0;
}

/** Offset of a Xing/Info header inside the first frame: after the 4-byte header and the side info. */
int getXingOffset (const MpegFrameHeader& frame) noexcept
{
    const auto mono = frame.numChannels == 1;

    if (frame.version == MpegVersion::mpeg1)
        return 4 + (mono ? 17 : 32);

    return 4 + (mono ? 9 : 17);
}

void readXingAndLameTags (juce::InputStream& input,
                          juce::int64 frameOffset,
                          const MpegFrameHeader& frame,
                          Mp3Scan& scan)
{
    if (frame.layer != 3)
        return;

    juce::uint8 bytes[512] {};

    if (! input.setPosition (frameOffset))
        return;

    const auto available = input.read (bytes, juce::jmin (frame.frameBytes, static_cast<int> (sizeof (bytes))));
    const auto offset = getXingOffset (frame);

    if (offset + 8 > available)
        return;

    const auto* tag = bytes + offset;

    if (std::memcmp (tag, "Xing", 4) != 0 && std::memcmp (tag, "Info", 4) != 0)
        return;

    scan.hasXingHeader = true;
    const auto flags = bigEndianWord (tag + 4);
    auto cursor = offset + 8;

    if ((flags & 1) != 0 && cursor + 4 <= available)
    {
        scan.xingFrameCount = bigEndianWord (bytes + cursor);
        cursor += 4;
    }

    if ((flags & 2) != 0)
        cursor += 4;

    if ((flags & 4) != 0)
        cursor += 100;

    if ((flags & 8) != 0)
        cursor += 4;

    // LAME tag (also written by FFmpeg's libavformat): 9-byte encoder string, then gapless info at +21.
    if (cursor + 24 > available)
        return;

    const auto* lame = bytes + cursor;

    if (std::memcmp (lame, "LAME", 4) != 0 && std::memcmp (lame, "Lavf", 4) != 0 && std::memcmp (lame, "Lavc", 4) != 0)
        return;

    scan.encoderDelay = (lame[21] << 4) | (lame[22] >> 4);
    scan.encoderPadding = ((lame[22] & 0x0f) << 8) | lame[23];
}

//==============================================================================
class Float64WavReader final : public juce::AudioFormatReader
{
public:
    Float64WavReader (juce::InputStream* in, const WavLayout& layout)
        : AudioFormatReader (in, "WAV file"),
          dataOffset (layout.dataOffset)
    {
        sampleRate = layout.sampleRate;
        bitsPerSample = 64;
        numChannels = static_cast<unsigned int> (layout.numChannels);
        usesFloatingPointData = true;
        lengthInSamples = layout.getNumFrames();
        scratch.allocate (static_cast<size_t> (scratchFrames) * numChannels, true);
    }

    bool readSamples (int* const* destSamples,
                      int numDestChannels,
                      int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile,
                      int numSamples) override
    {
        clearSamplesBeyondAvailableLength (destSamples,
                                           numDestChannels,
                                           startOffsetInDestBuffer,
                                           startSampleInFile,
                                           numSamples,
                                           lengthInSamples);

        if (numSamples <= 0)
            return true;

        const auto channels = static_cast<int> (numChannels);
        const auto bytesPerFrame = channels * static_cast<int> (sizeof (juce::uint64));

        if (! input->setPosition (dataOffset + startSampleInFile * bytesPerFrame))
            return false;

        while (numSamples > 0)
        {
            const auto frames = juce::jmin (numSamples, scratchFrames);
            const auto wanted = frames * bytesPerFrame;
            const auto got = juce::jmax (0, input->read (scratch.getData(), wanted));

            if (got < wanted)
                std::memset (reinterpret_cast<char*> (scratch.getData()) + got, 0, static_cast<size_t> (wanted - got));

            for (auto channel = 0; channel < numDestChannels; ++channel)
            {
                if (destSamples[channel] == nullptr)
                    continue;

                auto* out = reinterpret_cast<float*> (destSamples[channel]) + startOffsetInDestBuffer;

                if (channel >= channels)
                {
                    std::fill (out, out + frames, 0.0f);
                    continue;
                }

                for (auto i = 0; i < frames; ++i)
                {
                    const auto bits = juce::ByteOrder::swapIfBigEndian (scratch[i * channels + channel]);
                    out[i] = static_cast<float> (std::bit_cast<double> (bits));
                }
            }

            startOffsetInDestBuffer += frames;
            numSamples -= frames;
        }

        return true;
    }

private:
    static constexpr int scratchFrames = 4096;

    const juce::int64 dataOffset;
    juce::HeapBlock<juce::uint64> scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Float64WavReader)
};

//==============================================================================
class ExactLengthReader final : public juce::AudioFormatReader
{
public:
    ExactLengthReader (std::unique_ptr<juce::AudioFormatReader> sourceIn, juce::int64 length)
        : AudioFormatReader (nullptr, sourceIn->getFormatName()),
          source (std::move (sourceIn))
    {
        sampleRate = source->sampleRate;
        bitsPerSample = source->bitsPerSample;
        numChannels = source->numChannels;
        usesFloatingPointData = source->usesFloatingPointData;
        metadataValues = source->metadataValues;
        lengthInSamples = length;
    }

    bool readSamples (int* const* destSamples,
                      int numDestChannels,
                      int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile,
                      int numSamples) override
    {
        clearSamplesBeyondAvailableLength (destSamples,
                                           numDestChannels,
                                           startOffsetInDestBuffer,
                                           startSampleInFile,
                                           numSamples,
                                           lengthInSamples);

        if (numSamples <= 0)
            return true;

        return source->readSamples (destSamples,
                                    numDestChannels,
                                    startOffsetInDestBuffer,
                                    startSampleInFile,
                                    numSamples);
    }

private:
    std::unique_ptr<juce::AudioFormatReader> source;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExactLengthReader)
};
} // namespace

//==============================================================================
juce::int64 WavLayout::getBytesPerFrame() const noexcept
{
    return static_cast<juce::int64> (numChannels) * bitsPerSample / 8;
}

juce::int64 WavLayout::getNumFrames() const noexcept
{
    const auto bytesPerFrame = getBytesPerFrame();
    return bytesPerFrame > 0 ? dataBytes / bytesPerFrame : 0;
}

std::optional<WavLayout> readWavLayout (juce::InputStream& input)
{
    const auto total = input.getTotalLength();
    char id[4] {};

    if (! input.setPosition (0) || ! readChunkId (input, id))
        return std::nullopt;

    WavLayout layout;

    if (idEquals (id, "RF64"))
        layout.isRf64 = true;
    else if (! idEquals (id, "RIFF"))
        return std::nullopt;

    input.readInt(); // RIFF size: often wrong in the wild; chunks are walked to the end of the file instead

    if (! readChunkId (input, id) || ! idEquals (id, "WAVE"))
        return std::nullopt;

    juce::int64 rf64DataBytes = -1;
    juce::int64 position = 12;

    for (auto chunk = 0; chunk < maxChunksToWalk && position + 8 <= total; ++chunk)
    {
        if (! input.setPosition (position) || ! readChunkId (input, id))
            break;

        const auto declared = static_cast<juce::uint32> (input.readInt());
        const auto body = position + 8;
        auto size = static_cast<juce::int64> (declared);

        if (idEquals (id, "ds64") && layout.isRf64 && size >= 24)
        {
            input.readInt64(); // RIFF size
            rf64DataBytes = input.readInt64();
        }
        else if (idEquals (id, "fmt "))
        {
            readFormatChunk (input, size, layout);
        }
        else if (idEquals (id, "data"))
        {
            if (layout.isRf64 && declared == 0xffffffffu && rf64DataBytes >= 0)
                size = rf64DataBytes;

            layout.hasDataChunk = true;
            layout.dataOffset = body;
            layout.dataBytes = size;

            if (size > total - body)
            {
                layout.dataTruncated = true;
                break;
            }
        }
        else if (idEquals (id, "id3 ") || idEquals (id, "ID3 "))
        {
            layout.id3Offset = body;
            layout.id3Bytes = juce::jmin (size, total - body);
        }

        const auto next = body + size + (size & 1);

        if (next <= position)
            break;

        position = next;
    }

    return layout;
}

//==============================================================================
const char* toString (MpegVersion version) noexcept
{
    switch (version)
    {
        case MpegVersion::mpeg1:
            return "MPEG-1";
        case MpegVersion::mpeg2:
            return "MPEG-2";
        case MpegVersion::mpeg25:
            return "MPEG-2.5";
    }

    return "MPEG";
}

std::optional<MpegFrameHeader> parseMpegFrameHeader (juce::uint32 header) noexcept
{
    if ((header & 0xffe00000u) != 0xffe00000u)
        return std::nullopt;

    const auto versionBits = (header >> 19) & 3;
    const auto layerBits = (header >> 17) & 3;
    const auto bitrateIndex = static_cast<int> ((header >> 12) & 15);
    const auto sampleRateIndex = static_cast<int> ((header >> 10) & 3);
    const auto emphasis = header & 3;

    if (versionBits == 1 || layerBits == 0 || bitrateIndex == 0 || bitrateIndex == 15 || sampleRateIndex == 3
        || emphasis == 2)
        return std::nullopt;

    static constexpr short bitrates[5][15] = {
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 }, // MPEG-1 Layer I
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },    // MPEG-1 Layer II
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },     // MPEG-1 Layer III
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },    // MPEG-2/2.5 Layer I
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },         // MPEG-2/2.5 Layer II/III
    };
    static constexpr int sampleRates[3][3] = {
        { 44100, 48000, 32000 },
        { 22050, 24000, 16000 },
        { 11025, 12000, 8000 },
    };

    MpegFrameHeader frame;
    frame.version =
        versionBits == 3 ? MpegVersion::mpeg1 : (versionBits == 2 ? MpegVersion::mpeg2 : MpegVersion::mpeg25);
    frame.layer = 4 - static_cast<int> (layerBits);
    frame.crcProtected = ((header >> 16) & 1) == 0;

    const auto isMpeg1 = frame.version == MpegVersion::mpeg1;
    const auto table = isMpeg1 ? frame.layer - 1 : (frame.layer == 1 ? 3 : 4);
    frame.bitrateKbps = bitrates[table][bitrateIndex];
    frame.sampleRate = sampleRates[isMpeg1 ? 0 : (frame.version == MpegVersion::mpeg2 ? 1 : 2)][sampleRateIndex];
    frame.numChannels = ((header >> 6) & 3) == 3 ? 1 : 2;

    const auto padding = static_cast<int> ((header >> 9) & 1);
    const auto bitsPerSecond = frame.bitrateKbps * 1000;

    if (frame.layer == 1)
    {
        frame.samplesPerFrame = 384;
        frame.frameBytes = (12 * bitsPerSecond / frame.sampleRate + padding) * 4;
    }
    else if (frame.layer == 2 || isMpeg1)
    {
        frame.samplesPerFrame = 1152;
        frame.frameBytes = 144 * bitsPerSecond / frame.sampleRate + padding;
    }
    else
    {
        frame.samplesPerFrame = 576;
        frame.frameBytes = 72 * bitsPerSecond / frame.sampleRate + padding;
    }

    if (frame.frameBytes < 4)
        return std::nullopt;

    return frame;
}

juce::int64 getId3v2TagSize (const juce::uint8* h) noexcept
{
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3' || h[3] < 2 || h[3] > 4 || h[4] == 0xff)
        return 0;

    if (((h[6] | h[7] | h[8] | h[9]) & 0x80) != 0)
        return 0;

    const auto size = (static_cast<juce::int64> (h[6]) << 21) | (static_cast<juce::int64> (h[7]) << 14)
                      | (static_cast<juce::int64> (h[8]) << 7) | static_cast<juce::int64> (h[9]);
    const auto hasFooter = h[3] == 4 && (h[5] & 0x10) != 0;
    return 10 + size + (hasFooter ? 10 : 0);
}

int Mp3Scan::getAverageBitrateKbps() const noexcept
{
    if (audioFrameCount <= 0 || firstFrame.sampleRate <= 0)
        return 0;

    const auto seconds = static_cast<double> (getDecodedLength()) / firstFrame.sampleRate;
    return juce::roundToInt (static_cast<double> (audioBytes) * 8.0 / seconds / 1000.0);
}

Mp3Scan scanMp3 (juce::InputStream& source)
{
    juce::BufferedInputStream input (&source, 64 * 1024, false);
    const auto total = input.getTotalLength();
    Mp3Scan scan;
    juce::int64 position = 0;

    for (auto tag = 0; tag < maxLeadingId3Tags; ++tag)
    {
        juce::uint8 header[10];

        if (! input.setPosition (position) || input.read (header, 10) != 10)
            break;

        const auto size = getId3v2TagSize (header);

        if (size == 0)
            break;

        position += size;
    }

    scan.id3v2Bytes = juce::jmin (position, total);
    scan.firstFrameOffset = findFrameChain (input, scan.id3v2Bytes, maxJunkBeforeFirstFrame, total);

    if (scan.firstFrameOffset < 0)
        return scan;

    scan.firstFrame = *readFrameHeaderAt (input, scan.firstFrameOffset);
    readXingAndLameTags (input, scan.firstFrameOffset, scan.firstFrame, scan);

    position = scan.firstFrameOffset;
    auto isFirstFrame = true;
    auto firstAudioBitrate = 0;

    while (position + 4 <= total)
    {
        juce::uint8 bytes[4];

        if (! input.setPosition (position) || input.read (bytes, 4) != 4)
            break;

        if (const auto frame = parseMpegFrameHeader (bigEndianWord (bytes)); frame.has_value())
        {
            if (isSameStream (*frame, scan.firstFrame))
            {
                if (position + frame->frameBytes > total)
                    break; // an incomplete final frame does not decode

                const auto isXingFrame = isFirstFrame && scan.hasXingHeader; // carries no audio

                if (! isXingFrame)
                {
                    if (firstAudioBitrate == 0)
                        firstAudioBitrate = frame->bitrateKbps;
                    else if (frame->bitrateKbps != firstAudioBitrate)
                        scan.variableBitrate = true;

                    ++scan.audioFrameCount;
                    scan.audioBytes += frame->frameBytes;
                }

                isFirstFrame = false;
                position += frame->frameBytes;
                continue;
            }

            scan.inconsistentFrames = true;
        }

        if (startsWithTrailingTag (bytes))
            break;

        const auto next = findFrameChain (input, position + 1, maxResyncDistance, total);

        if (next < 0)
            break;

        ++scan.resyncCount;
        position = next;
    }

    return scan;
}

//==============================================================================
std::unique_ptr<juce::AudioFormatReader> createFloat64WavReader (std::unique_ptr<juce::InputStream> input,
                                                                 const WavLayout& layout)
{
    if (input == nullptr || layout.numChannels <= 0 || layout.sampleRate == 0 || layout.bitsPerSample != 64)
        return nullptr;

    return std::make_unique<Float64WavReader> (input.release(), layout);
}

std::unique_ptr<juce::AudioFormatReader> withExactLength (std::unique_ptr<juce::AudioFormatReader> reader,
                                                          juce::int64 lengthInSamples)
{
    if (reader == nullptr)
        return nullptr;

    return std::make_unique<ExactLengthReader> (std::move (reader), lengthInSamples);
}

} // namespace snds::audio
