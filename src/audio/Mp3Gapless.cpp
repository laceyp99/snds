#include "audio/Mp3Gapless.h"

#include <algorithm>
#include <cstring>

namespace snds::audio
{
namespace
{
struct FrameHeader
{
    bool valid = false;
    bool mpeg1 = false;
    bool crc = false;
    int sampleRate = 0;
    int channels = 0;
    int samplesPerFrame = 0;
};

FrameHeader parseFrameHeader (const std::uint8_t* p)
{
    FrameHeader header;
    const auto word = (static_cast<std::uint32_t> (p[0]) << 24) | (static_cast<std::uint32_t> (p[1]) << 16)
                    | (static_cast<std::uint32_t> (p[2]) << 8) | p[3];
    const auto version = (word >> 19) & 3; // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    const auto layer = (word >> 17) & 3;   // 1 = Layer III
    const auto bitrateIndex = (word >> 12) & 15;
    const auto rateIndex = (word >> 10) & 3;

    if (((word >> 21) & 0x7FF) != 0x7FF || version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15
        || rateIndex == 3)
        return header;

    static constexpr int rates[3][3] = { { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 } };
    const int row = version == 3 ? 0 : (version == 2 ? 1 : 2);
    header.valid = true;
    header.mpeg1 = version == 3;
    header.crc = ((word >> 16) & 1) == 0;
    header.sampleRate = rates[row][rateIndex];
    header.channels = ((word >> 6) & 3) == 3 ? 1 : 2;
    header.samplesPerFrame = header.mpeg1 ? 1152 : 576;
    return header;
}

std::uint32_t readBigEndian32 (const std::uint8_t* p)
{
    return (static_cast<std::uint32_t> (p[0]) << 24) | (static_cast<std::uint32_t> (p[1]) << 16)
         | (static_cast<std::uint32_t> (p[2]) << 8) | p[3];
}
} // namespace

Mp3Info parseMp3 (const void* data, std::size_t size)
{
    Mp3Info info;
    const auto* bytes = static_cast<const std::uint8_t*> (data);
    std::size_t position = 0;

    if (size >= 10 && std::memcmp (bytes, "ID3", 3) == 0)
    {
        const auto tagSize =
            (static_cast<std::size_t> (bytes[6] & 0x7F) << 21) | (static_cast<std::size_t> (bytes[7] & 0x7F) << 14)
            | (static_cast<std::size_t> (bytes[8] & 0x7F) << 7) | static_cast<std::size_t> (bytes[9] & 0x7F);
        position = 10 + tagSize + ((bytes[5] & 0x10) != 0 ? 10 : 0);
    }

    // Find the first frame (resync over padding between the tag and the audio).
    FrameHeader header;
    const auto searchLimit = std::min (size, position + 4096);

    for (; position + 4 <= searchLimit; ++position)
    {
        header = parseFrameHeader (bytes + position);

        if (header.valid)
            break;
    }

    if (! header.valid)
        return info;

    info.valid = true;
    info.sampleRate = header.sampleRate;
    info.channels = header.channels;
    info.samplesPerFrame = header.samplesPerFrame;

    // The Xing/Info tag sits after the header, optional CRC and side information.
    const std::size_t sideInfo = header.mpeg1 ? (header.channels == 1 ? 17 : 32) : (header.channels == 1 ? 9 : 17);
    const auto tag = position + 4 + (header.crc ? 2 : 0) + sideInfo;

    if (tag + 8 > size || (std::memcmp (bytes + tag, "Xing", 4) != 0 && std::memcmp (bytes + tag, "Info", 4) != 0))
        return info;

    info.hasInfoFrame = true;
    const auto flags = readBigEndian32 (bytes + tag + 4);
    auto cursor = tag + 8;

    if ((flags & 1) != 0 && cursor + 4 <= size)
    {
        info.headerFrames = static_cast<int> (readBigEndian32 (bytes + cursor));
        cursor += 4;
    }

    if ((flags & 2) != 0)
        cursor += 4;

    if ((flags & 4) != 0)
        cursor += 100;

    if ((flags & 8) != 0)
        cursor += 4;

    // LAME extension: 9-byte encoder string, then 12 bytes before the delay/padding triple.
    constexpr std::size_t delayOffset = 9 + 12;

    if (cursor + delayOffset + 3 > size)
        return info;

    const auto* version = bytes + cursor;
    const bool lameLike = std::memcmp (version, "LAME", 4) == 0 || std::memcmp (version, "Lavf", 4) == 0
                       || std::memcmp (version, "Lavc", 4) == 0 || std::memcmp (version, "GOGO", 4) == 0;

    if (! lameLike)
        return info;

    const auto* fields = bytes + cursor + delayOffset;
    const int delay = (fields[0] << 4) | (fields[1] >> 4);
    const int padding = ((fields[1] & 0x0F) << 8) | fields[2];

    // A well-formed tag keeps both well under two frames; anything else is not trusted.
    if (delay > 2 * header.samplesPerFrame || padding > 2 * header.samplesPerFrame)
        return info;

    info.hasLameTag = true;
    info.encoderDelay = delay;
    info.encoderPadding = padding;
    return info;
}

Mp3Info parseMp3 (const juce::File& file)
{
    juce::FileInputStream stream (file);

    if (! stream.openedOk())
        return {};

    // The tag lives in the first frame; 64 KiB covers a large ID3v2 header as well.
    juce::MemoryBlock block;
    stream.readIntoMemoryBlock (block, 65536);
    return parseMp3 (block.getData(), block.getSize());
}

int predictedLeadingSkip (const Mp3Info& info)
{
    if (! info.valid || ! info.hasLameTag)
        return 0;

    return info.encoderDelay + standardDecoderDelay;
}
} // namespace snds::audio
