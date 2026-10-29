#include "app/EmbeddedArtwork.h"

#include "audio/SourceFormats.h"

#include <algorithm>
#include <cstring>

namespace snds
{

namespace
{
using Bytes = std::vector<juce::uint8>;

constexpr int maxLeadingTags = 4;
constexpr size_t maxTagBytes = 64 * 1024 * 1024;

juce::uint32 readBigEndian (const juce::uint8* p, int numBytes) noexcept
{
    juce::uint32 value = 0;

    for (auto i = 0; i < numBytes; ++i)
        value = (value << 8) | p[i];

    return value;
}

juce::uint32 readSyncsafe (const juce::uint8* p) noexcept
{
    return (static_cast<juce::uint32> (p[0] & 0x7f) << 21) | (static_cast<juce::uint32> (p[1] & 0x7f) << 14)
           | (static_cast<juce::uint32> (p[2] & 0x7f) << 7) | static_cast<juce::uint32> (p[3] & 0x7f);
}

/** Reverses ID3 unsynchronisation: every 0xFF 0x00 pair becomes 0xFF. */
void removeUnsynchronisation (Bytes& bytes)
{
    size_t out = 0;

    for (size_t in = 0; in < bytes.size(); ++in)
    {
        bytes[out++] = bytes[in];

        if (bytes[in] == 0xff && in + 1 < bytes.size() && bytes[in + 1] == 0x00)
            ++in;
    }

    bytes.resize (out);
}

bool isFrameIdCharacter (juce::uint8 c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool isValidFrameId (const juce::uint8* p, size_t length) noexcept
{
    for (size_t i = 0; i < length; ++i)
        if (! isFrameIdCharacter (p[i]))
            return false;

    return true;
}

/** True when a frame (or padding, or the end of the tag) could start at position. */
bool isPlausibleFrameStart (const Bytes& body, size_t position) noexcept
{
    if (position == body.size())
        return true;

    if (position + 4 > body.size())
        return false;

    return body[position] == 0 || isValidFrameId (body.data() + position, 4);
}

bool inflateZlib (Bytes& bytes)
{
    juce::MemoryInputStream compressed (bytes.data(), bytes.size(), false);
    juce::GZIPDecompressorInputStream inflater (&compressed, false, juce::GZIPDecompressorInputStream::zlibFormat);
    juce::MemoryBlock inflated;

    const auto numRead =
        inflater.readIntoMemoryBlock (inflated, static_cast<juce::ssize_t> (ArtworkLimits::maxEncodedBytes));

    if (numRead <= 0 || inflater.isExhausted() == false)
        return false;

    const auto* start = static_cast<const juce::uint8*> (inflated.getData());
    bytes.assign (start, start + inflated.getSize());
    return true;
}

/** Strips per-frame extra bytes and undoes unsynchronisation/compression. False if the frame is unusable. */
bool decodeFrameData (int majorVersion, juce::uint16 frameFlags, bool tagUnsynchronised, Bytes& data)
{
    size_t skip = 0;
    auto compressed = false;
    auto unsynchronised = false;

    if (majorVersion == 3)
    {
        compressed = (frameFlags & 0x0080) != 0;

        if ((frameFlags & 0x0040) != 0) // encrypted
            return false;

        skip += compressed ? 4 : 0;                 // decompressed size
        skip += (frameFlags & 0x0020) != 0 ? 1 : 0; // group identifier
    }
    else if (majorVersion == 4)
    {
        compressed = (frameFlags & 0x0008) != 0;
        unsynchronised = tagUnsynchronised || (frameFlags & 0x0002) != 0;

        if ((frameFlags & 0x0004) != 0) // encrypted
            return false;

        skip += (frameFlags & 0x0040) != 0 ? 1 : 0; // group identifier
        skip += (frameFlags & 0x0001) != 0 ? 4 : 0; // data length indicator
    }

    if (skip > data.size())
        return false;

    data.erase (data.begin(), data.begin() + static_cast<std::ptrdiff_t> (skip));

    if (unsynchronised)
        removeUnsynchronisation (data);

    return ! compressed || inflateZlib (data);
}

/** Reads an encoded, terminated string starting at position; advances position past the terminator. */
juce::String readTerminatedString (const Bytes& data, size_t& position, juce::uint8 encoding)
{
    const auto wide = encoding == 1 || encoding == 2;
    const auto start = position;
    auto end = start;

    if (wide)
    {
        while (end + 1 < data.size() && (data[end] != 0 || data[end + 1] != 0))
            end += 2;

        position = juce::jmin (data.size(), end + 2);
    }
    else
    {
        while (end < data.size() && data[end] != 0)
            ++end;

        position = juce::jmin (data.size(), end + 1);
    }

    if (encoding == 0)
    {
        juce::String text;

        for (auto i = start; i < end; ++i)
            text += static_cast<juce::juce_wchar> (data[i]);

        return text;
    }

    if (encoding == 3)
        return juce::String::fromUTF8 (reinterpret_cast<const char*> (data.data() + start),
                                       static_cast<int> (end - start));

    // UTF-16: a BOM selects the byte order (encoding 1); encoding 2 is big-endian without a BOM.
    auto bigEndian = encoding == 2;
    auto i = start;

    if (encoding == 1 && i + 1 < end)
    {
        if (data[i] == 0xff && data[i + 1] == 0xfe)
            i += 2;
        else if (data[i] == 0xfe && data[i + 1] == 0xff)
        {
            bigEndian = true;
            i += 2;
        }
    }

    juce::String text;

    for (; i < end && i + 1 < data.size(); i += 2)
    {
        const auto unit = bigEndian ? static_cast<juce::juce_wchar> ((data[i] << 8) | data[i + 1])
                                    : static_cast<juce::juce_wchar> ((data[i + 1] << 8) | data[i]);

        if (unit < 0xd800 || unit > 0xdfff)
            text += unit;
    }

    return text;
}

std::optional<Id3Picture> parsePictureFrame (int majorVersion, const Bytes& data)
{
    if (data.size() < 4)
        return std::nullopt;

    const auto encoding = data[0];

    if (encoding > 3)
        return std::nullopt;

    Id3Picture picture;
    size_t position = 1;

    if (majorVersion == 2)
    {
        const auto format = juce::String::fromUTF8 (reinterpret_cast<const char*> (data.data() + 1), 3).toUpperCase();
        picture.mimeType = format == "PNG" ? "image/png" : (format == "JPG" ? "image/jpeg" : format);
        position = 4;
    }
    else
    {
        picture.mimeType = readTerminatedString (data, position, 0).toLowerCase();
    }

    if (position >= data.size())
        return std::nullopt;

    picture.pictureType = data[position++];
    picture.description = readTerminatedString (data, position, encoding);

    if (picture.mimeType == "-->" || position >= data.size())
        return std::nullopt; // a linked (URL) picture, or no image data

    picture.data.append (data.data() + position, data.size() - position);
    return picture;
}

//==============================================================================
struct ImageSize
{
    int width = 0;
    int height = 0;
};

juce::String identifyImageType (const juce::uint8* p, size_t size) noexcept
{
    if (size >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G')
        return "PNG";

    if (size >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff)
        return "JPEG";

    if (size >= 6 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F')
        return "GIF";

    return {};
}

/** Reads the declared dimensions without decoding, so absurd images are rejected before allocation. */
std::optional<ImageSize> probeImageSize (const juce::String& type, const juce::uint8* p, size_t size) noexcept
{
    if (type == "PNG" && size >= 24)
        return ImageSize { static_cast<int> (readBigEndian (p + 16, 4) & 0x7fffffff),
                           static_cast<int> (readBigEndian (p + 20, 4) & 0x7fffffff) };

    if (type == "GIF" && size >= 10)
        return ImageSize { p[6] | (p[7] << 8), p[8] | (p[9] << 8) };

    if (type == "JPEG")
    {
        size_t position = 2;

        while (position + 4 <= size)
        {
            if (p[position] != 0xff)
                return std::nullopt;

            const auto marker = p[position + 1];

            if (marker == 0xff)
            {
                ++position; // fill byte
                continue;
            }

            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd9))
            {
                position += 2; // standalone marker
                continue;
            }

            const auto length = readBigEndian (p + position + 2, 2);
            const auto isStartOfFrame =
                marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;

            if (isStartOfFrame && position + 9 <= size)
                return ImageSize { static_cast<int> (readBigEndian (p + position + 7, 2)),
                                   static_cast<int> (readBigEndian (p + position + 5, 2)) };

            position += 2 + length;
        }
    }

    return std::nullopt;
}

std::vector<Id3Picture> readMp3Pictures (const juce::File& file, juce::String& note)
{
    std::vector<Id3Picture> pictures;
    auto stream = file.createInputStream();

    if (stream == nullptr)
    {
        note = "The MP3 could not be reopened to read its tags.";
        return pictures;
    }

    juce::int64 position = 0;
    auto foundTag = false;

    for (auto tag = 0; tag < maxLeadingTags; ++tag)
    {
        juce::uint8 header[10];

        if (! stream->setPosition (position) || stream->read (header, 10) != 10)
            break;

        const auto tagSize = audio::getId3v2TagSize (header);

        if (tagSize == 0)
            break;

        foundTag = true;
        juce::MemoryBlock tagBytes;
        stream->setPosition (position);
        stream->readIntoMemoryBlock (tagBytes,
                                     static_cast<juce::ssize_t> (juce::jmin<juce::int64> (tagSize, maxTagBytes)));

        for (auto& picture : parseId3Pictures (tagBytes.getData(), tagBytes.getSize()))
            pictures.push_back (std::move (picture));

        position += tagSize;
    }

    if (! foundTag)
        note = "This MP3 has no ID3v2 tag, so it carries no embedded artwork.";
    else if (pictures.empty())
        note = "This MP3's ID3 tag contains no artwork.";

    return pictures;
}

std::vector<Id3Picture> readWavPictures (const juce::File& file, juce::String& note)
{
    auto stream = file.createInputStream();
    const auto layout = stream != nullptr ? audio::readWavLayout (*stream) : std::nullopt;

    if (! layout.has_value() || layout->id3Offset < 0 || layout->id3Bytes < 10)
    {
        note = "This WAV has no ID3 metadata chunk, so it carries no embedded artwork (common for WAV files).";
        return {};
    }

    juce::MemoryBlock tagBytes;
    stream->setPosition (layout->id3Offset);
    stream->readIntoMemoryBlock (tagBytes,
                                 static_cast<juce::ssize_t> (juce::jmin<juce::int64> (layout->id3Bytes, maxTagBytes)));

    auto pictures = parseId3Pictures (tagBytes.getData(), tagBytes.getSize());

    if (pictures.empty())
        note = "This WAV's ID3 chunk contains no artwork.";

    return pictures;
}

juce::String describeImage (const DecodedImage& decoded)
{
    return decoded.imageType + ", " + juce::String (decoded.sourceWidth) + " x " + juce::String (decoded.sourceHeight);
}
} // namespace

//==============================================================================
std::vector<Id3Picture> parseId3Pictures (const void* tagData, size_t numBytes)
{
    std::vector<Id3Picture> pictures;
    const auto* header = static_cast<const juce::uint8*> (tagData);

    if (header == nullptr || numBytes < 10)
        return pictures;

    const auto tagSize = audio::getId3v2TagSize (header);

    if (tagSize == 0)
        return pictures;

    const int majorVersion = header[3];
    const auto flags = header[5];
    const auto hasFooter = majorVersion == 4 && (flags & 0x10) != 0;
    const auto declaredBody = static_cast<size_t> (tagSize - 10 - (hasFooter ? 10 : 0));
    const auto bodySize = juce::jmin (declaredBody, numBytes - 10);
    const auto tagUnsynchronised = (flags & 0x80) != 0;

    if (majorVersion == 2 && (flags & 0x40) != 0)
        return pictures; // v2.2 "compression" was never defined

    Bytes body (header + 10, header + 10 + bodySize);

    if (tagUnsynchronised && majorVersion < 4)
        removeUnsynchronisation (body);

    size_t position = 0;

    if (majorVersion >= 3 && (flags & 0x40) != 0 && body.size() >= 4)
    {
        // v2.3: the size excludes its own 4 bytes; v2.4: syncsafe and includes itself.
        position = majorVersion == 3 ? 4 + readBigEndian (body.data(), 4) : readSyncsafe (body.data());
    }

    const size_t idLength = majorVersion == 2 ? 3 : 4;
    const size_t headerLength = majorVersion == 2 ? 6 : 10;

    while (position + headerLength <= body.size())
    {
        const auto* frame = body.data() + position;

        if (frame[0] == 0 || ! isValidFrameId (frame, idLength))
            break; // padding, or damage

        size_t size = 0;
        juce::uint16 frameFlags = 0;

        if (majorVersion == 2)
        {
            size = readBigEndian (frame + 3, 3);
        }
        else if (majorVersion == 3)
        {
            size = readBigEndian (frame + 4, 4);
            frameFlags = static_cast<juce::uint16> (readBigEndian (frame + 8, 2));
        }
        else
        {
            // v2.4 sizes are syncsafe, but some writers used plain v2.3 sizes: prefer whichever
            // interpretation lands on a plausible next frame.
            const size_t syncsafeSize = readSyncsafe (frame + 4);
            const size_t plainSize = readBigEndian (frame + 4, 4);
            const auto hasHighBits = ((frame[4] | frame[5] | frame[6] | frame[7]) & 0x80) != 0;
            const auto next = position + headerLength;

            size = syncsafeSize;

            if (hasHighBits
                || (syncsafeSize != plainSize && ! isPlausibleFrameStart (body, next + syncsafeSize)
                    && isPlausibleFrameStart (body, next + plainSize)))
                size = plainSize;

            frameFlags = static_cast<juce::uint16> (readBigEndian (frame + 8, 2));
        }

        const auto dataStart = position + headerLength;

        if (size > body.size() - dataStart)
            break; // truncated tag

        const auto isPicture =
            majorVersion == 2 ? std::memcmp (frame, "PIC", 3) == 0 : std::memcmp (frame, "APIC", 4) == 0;

        if (isPicture)
        {
            Bytes data (body.begin() + static_cast<std::ptrdiff_t> (dataStart),
                        body.begin() + static_cast<std::ptrdiff_t> (dataStart + size));

            if (decodeFrameData (majorVersion, frameFlags, tagUnsynchronised, data))
                if (auto picture = parsePictureFrame (majorVersion, data))
                    pictures.push_back (std::move (*picture));
        }

        position = dataStart + size;
    }

    return pictures;
}

DecodedImage decodeArtworkImage (const void* data, size_t numBytes)
{
    DecodedImage result;
    const auto* bytes = static_cast<const juce::uint8*> (data);

    if (bytes == nullptr || numBytes == 0)
    {
        result.error = "the image is empty";
        return result;
    }

    if (numBytes > ArtworkLimits::maxEncodedBytes)
    {
        result.error = "the image file is larger than 32 MB";
        return result;
    }

    result.imageType = identifyImageType (bytes, numBytes);

    if (result.imageType.isEmpty())
    {
        result.error = "it is not a PNG, JPEG, or GIF image";
        return result;
    }

    if (const auto size = probeImageSize (result.imageType, bytes, numBytes))
    {
        const auto pixels = static_cast<juce::int64> (size->width) * size->height;

        if (size->width > ArtworkLimits::maxSourceDimension || size->height > ArtworkLimits::maxSourceDimension
            || pixels > ArtworkLimits::maxSourcePixels)
        {
            result.error = "the image is too large (" + juce::String (size->width) + " x " + juce::String (size->height)
                           + " pixels)";
            return result;
        }
    }

    const auto decoded = juce::ImageFileFormat::loadFrom (data, numBytes);

    if (! decoded.isValid() || decoded.getWidth() <= 0 || decoded.getHeight() <= 0)
    {
        result.error = "the " + result.imageType + " data could not be decoded";
        return result;
    }

    result.sourceWidth = decoded.getWidth();
    result.sourceHeight = decoded.getHeight();

    // Decoders create native (on Windows, Direct2D-backed) images; keep a plain software copy that
    // any thread may read.
    auto image = juce::SoftwareImageType().convert (decoded).convertedToFormat (juce::Image::ARGB);
    const auto longest = juce::jmax (image.getWidth(), image.getHeight());

    if (longest > ArtworkLimits::maxStoredDimension)
    {
        const auto scale = static_cast<double> (ArtworkLimits::maxStoredDimension) / longest;
        image = image.rescaled (juce::jmax (1, juce::roundToInt (image.getWidth() * scale)),
                                juce::jmax (1, juce::roundToInt (image.getHeight() * scale)),
                                juce::Graphics::highResamplingQuality);
    }

    result.image = image;
    return result;
}

EmbeddedArtworkResult extractEmbeddedArtwork (const model::SourceInfo& source)
{
    EmbeddedArtworkResult result;
    auto pictures = source.format == model::SourceFormat::mp3 ? readMp3Pictures (source.file, result.note)
                                                              : readWavPictures (source.file, result.note);

    if (pictures.empty())
        return result;

    // Front covers first, then the remaining pictures in tag order.
    std::stable_partition (pictures.begin(),
                           pictures.end(),
                           [] (const Id3Picture& picture) { return picture.pictureType == id3FrontCoverType; });

    juce::StringArray failures;

    for (const auto& picture : pictures)
    {
        const auto decoded = decodeArtworkImage (picture.data.getData(), picture.data.getSize());

        if (! decoded.image.isValid())
        {
            failures.add (decoded.error);
            continue;
        }

        model::Artwork artwork;
        artwork.image = decoded.image;
        artwork.origin = model::ArtworkOrigin::embedded;
        artwork.description =
            juce::String (picture.pictureType == id3FrontCoverType ? "Embedded front cover" : "Embedded artwork") + " ("
            + describeImage (decoded) + ")";
        result.artwork = std::move (artwork);

        if (! failures.isEmpty())
            result.note = "Another embedded picture could not be decoded (" + failures[0] + ").";

        return result;
    }

    result.note = "Embedded artwork was found but could not be decoded (" + failures[0] + ").";
    return result;
}

ReplacementArtworkResult loadReplacementArtwork (const juce::File& imageFile)
{
    ReplacementArtworkResult result;
    const auto name = "\"" + imageFile.getFileName() + "\"";

    if (! imageFile.existsAsFile())
    {
        result.error = name + " no longer exists. Choose another image.";
        return result;
    }

    if (! imageFile.hasFileExtension ("png;jpg;jpeg"))
    {
        result.error = name + " is not a PNG or JPEG image. Choose a .png, .jpg, or .jpeg file.";
        return result;
    }

    if (imageFile.getSize() > static_cast<juce::int64> (ArtworkLimits::maxEncodedBytes))
    {
        result.error = name + " is larger than 32 MB. Choose a smaller image.";
        return result;
    }

    juce::MemoryBlock bytes;

    if (! imageFile.loadFileAsData (bytes))
    {
        result.error = "SNDS could not read " + name + ". Check that the file is not locked, then try again.";
        return result;
    }

    const auto decoded = decodeArtworkImage (bytes.getData(), bytes.getSize());

    if (! decoded.image.isValid())
    {
        result.error = "SNDS could not use " + name + ": " + decoded.error + ". Choose a PNG or JPEG image.";
        return result;
    }

    model::Artwork artwork;
    artwork.image = decoded.image;
    artwork.origin = model::ArtworkOrigin::replacement;
    artwork.file = imageFile;
    artwork.description = "Replacement image " + name + " (" + describeImage (decoded) + ")";
    result.artwork = std::move (artwork);
    return result;
}

} // namespace snds
