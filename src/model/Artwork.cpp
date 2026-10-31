#include "model/Artwork.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace snds
{
namespace
{
using Bytes = std::vector<unsigned char>;

constexpr size_t maxId3TagBytes = 64 * 1024 * 1024;
constexpr juce::int64 maxUserImageBytes = 16 * 1024 * 1024;
constexpr juce::uint32 maxWavChunkBytes = 64 * 1024 * 1024;

juce::uint32 readBe32 (const unsigned char* p)
{
    return (static_cast<juce::uint32> (p[0]) << 24) | (static_cast<juce::uint32> (p[1]) << 16)
         | (static_cast<juce::uint32> (p[2]) << 8) | static_cast<juce::uint32> (p[3]);
}

juce::uint32 readBe24 (const unsigned char* p)
{
    return (static_cast<juce::uint32> (p[0]) << 16) | (static_cast<juce::uint32> (p[1]) << 8)
         | static_cast<juce::uint32> (p[2]);
}

bool isSyncsafe (const unsigned char* p)
{
    return (p[0] | p[1] | p[2] | p[3]) < 0x80;
}

juce::uint32 readSyncsafe (const unsigned char* p)
{
    return (static_cast<juce::uint32> (p[0]) << 21) | (static_cast<juce::uint32> (p[1]) << 14)
         | (static_cast<juce::uint32> (p[2]) << 7) | static_cast<juce::uint32> (p[3]);
}

/** FF 00 becomes FF. */
Bytes removeUnsynchronisation (const unsigned char* data, size_t size)
{
    Bytes out;
    out.reserve (size);

    for (size_t i = 0; i < size; ++i)
    {
        out.push_back (data[i]);

        if (data[i] == 0xFF && i + 1 < size && data[i + 1] == 0x00)
            ++i;
    }

    return out;
}

/** Index just past the terminator of a text field, or npos if there is none. Encodings 1 and 2
    are UTF-16 with a two-byte terminator on a two-byte boundary from `start`. */
size_t skipTextField (const Bytes& bytes, size_t start, int encoding)
{
    if (encoding == 1 || encoding == 2)
    {
        for (size_t i = start; i + 1 < bytes.size(); i += 2)
            if (bytes[i] == 0 && bytes[i + 1] == 0)
                return i + 2;

        return std::string::npos;
    }

    for (size_t i = start; i < bytes.size(); ++i)
        if (bytes[i] == 0)
            return i + 1;

    return std::string::npos;
}

/** The picture in an APIC (v2.3/2.4) or PIC (v2.2) frame body. */
std::optional<ImageData> parsePictureFrame (const Bytes& body, bool isV22)
{
    if (body.size() < 5)
        return std::nullopt;

    const int encoding = body[0];
    size_t index = 1;

    if (isV22)
    {
        index += 3; // image format: "PNG", "JPG"
    }
    else
    {
        // The MIME type is always Latin-1 and null-terminated. "-->" means a URL: no picture.
        const auto end = skipTextField (body, index, 0);

        if (end == std::string::npos)
            return std::nullopt;

        if (end - index == 4 && std::memcmp (&body[index], "-->", 3) == 0)
            return std::nullopt;

        index = end;
    }

    if (index >= body.size())
        return std::nullopt;

    const int pictureType = body[index++];
    const auto pictureStart = skipTextField (body, index, encoding);

    if (pictureStart == std::string::npos || pictureStart >= body.size())
        return std::nullopt;

    const auto* picture = body.data() + pictureStart;
    const auto pictureSize = body.size() - pictureStart;
    const auto extension = sniffImageExtension (picture, pictureSize);

    if (extension.isEmpty())
        return std::nullopt;

    ImageData image;
    image.bytes.append (picture, pictureSize);
    image.extension = extension;
    image.pictureType = pictureType;
    return image;
}

bool isPictureFrame (const char* id, bool isV22)
{
    return isV22 ? std::memcmp (id, "PIC", 3) == 0 : std::memcmp (id, "APIC", 4) == 0;
}
} // namespace

//==============================================================================
juce::String sniffImageExtension (const void* data, size_t size)
{
    const auto* b = static_cast<const unsigned char*> (data);
    static const unsigned char png[] { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    if (size >= sizeof (png) && std::memcmp (b, png, sizeof (png)) == 0)
        return "png";

    if (size >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return "jpg";

    if (size >= 6 && (std::memcmp (b, "GIF87a", 6) == 0 || std::memcmp (b, "GIF89a", 6) == 0))
        return "gif";

    return {};
}

std::optional<ImageData> findArtworkInId3 (const void* data, size_t size)
{
    const auto* d = static_cast<const unsigned char*> (data);

    if (d == nullptr || size < 10 || std::memcmp (d, "ID3", 3) != 0)
        return std::nullopt;

    const int major = d[3];
    const int flags = d[5];

    if (major < 2 || major > 4 || ! isSyncsafe (d + 6))
        return std::nullopt;

    if (major == 2 && (flags & 0x40) != 0)
        return std::nullopt; // v2.2 compression is undefined; skip the tag

    const size_t declared = readSyncsafe (d + 6);

    if (declared > maxId3TagBytes)
        return std::nullopt;

    const auto available = std::min (declared, size - 10);
    Bytes body (d + 10, d + 10 + available);

    // Before v2.4 the unsynchronisation flag covers the whole tag.
    if ((flags & 0x80) != 0 && major < 4)
        body = removeUnsynchronisation (body.data(), body.size());

    size_t pos = 0;

    if (major >= 3 && (flags & 0x40) != 0) // extended header
    {
        if (body.size() < 4)
            return std::nullopt;

        const size_t extended =
            major == 4 ? readSyncsafe (body.data()) : static_cast<size_t> (readBe32 (body.data())) + 4;

        if (extended < 4 || extended > body.size())
            return std::nullopt;

        pos = extended;
    }

    const bool isV22 = major == 2;
    const size_t headerSize = isV22 ? 6 : 10;
    std::optional<ImageData> first;

    while (pos + headerSize <= body.size() && body[pos] != 0)
    {
        const auto* frame = body.data() + pos;
        const char* id = reinterpret_cast<const char*> (frame);
        size_t frameSize = 0;
        int frameFlags = 0;

        if (isV22)
        {
            frameSize = readBe24 (frame + 3);
        }
        else
        {
            frameSize = major == 4 && isSyncsafe (frame + 4) ? readSyncsafe (frame + 4) : readBe32 (frame + 4);
            frameFlags = (frame[8] << 8) | frame[9];
        }

        const auto payloadStart = pos + headerSize;

        if (frameSize > body.size() - payloadStart)
            break; // truncated or corrupt: nothing reliable follows

        pos = payloadStart + frameSize;

        if (! isPictureFrame (id, isV22))
            continue;

        auto payload = Bytes (body.begin() + static_cast<std::ptrdiff_t> (payloadStart),
                              body.begin() + static_cast<std::ptrdiff_t> (payloadStart + frameSize));

        if (! isV22)
        {
            size_t skip = 0;
            bool unsynchronised = false;

            if (major == 3)
            {
                if ((frameFlags & 0x0080) != 0 || (frameFlags & 0x0040) != 0) // compressed / encrypted
                    continue;

                if ((frameFlags & 0x0020) != 0)
                    skip += 1; // grouping identity
            }
            else
            {
                if ((frameFlags & 0x0008) != 0 || (frameFlags & 0x0004) != 0)
                    continue;

                if ((frameFlags & 0x0040) != 0)
                    skip += 1;

                if ((frameFlags & 0x0001) != 0)
                    skip += 4; // data length indicator

                unsynchronised = (frameFlags & 0x0002) != 0 || (flags & 0x80) != 0;
            }

            if (skip > payload.size())
                continue;

            payload.erase (payload.begin(), payload.begin() + static_cast<std::ptrdiff_t> (skip));

            if (unsynchronised)
                payload = removeUnsynchronisation (payload.data(), payload.size());
        }

        if (auto picture = parsePictureFrame (payload, isV22))
        {
            if (picture->pictureType == 3)
                return picture;

            if (! first.has_value())
                first = std::move (picture);
        }
    }

    return first;
}

//==============================================================================
namespace
{
std::optional<ImageData> extractFromWav (const juce::File& file)
{
    juce::FileInputStream stream (file);

    if (stream.failedToOpen())
        return std::nullopt;

    unsigned char header[12];

    if (stream.read (header, sizeof (header)) != static_cast<int> (sizeof (header))
        || (std::memcmp (header, "RIFF", 4) != 0 && std::memcmp (header, "RF64", 4) != 0)
        || std::memcmp (header + 8, "WAVE", 4) != 0)
        return std::nullopt;

    const auto length = stream.getTotalLength();

    for (int chunks = 0; chunks < 4096 && stream.getPosition() + 8 <= length; ++chunks)
    {
        unsigned char chunk[8];

        if (stream.read (chunk, sizeof (chunk)) != static_cast<int> (sizeof (chunk)))
            break;

        const auto size = static_cast<juce::uint32> (chunk[4]) | (static_cast<juce::uint32> (chunk[5]) << 8)
                        | (static_cast<juce::uint32> (chunk[6]) << 16) | (static_cast<juce::uint32> (chunk[7]) << 24);
        const bool isId3 = std::memcmp (chunk, "id3 ", 4) == 0 || std::memcmp (chunk, "ID3 ", 4) == 0;

        if (isId3)
        {
            if (size > maxWavChunkBytes || stream.getPosition() + static_cast<juce::int64> (size) > length)
                return std::nullopt;

            Bytes body (size);

            if (stream.read (body.data(), static_cast<int> (size)) != static_cast<int> (size))
                return std::nullopt;

            return findArtworkInId3 (body.data(), body.size());
        }

        if (size == 0xFFFFFFFFu) // RF64 placeholder: sizes live in ds64; stop scanning
            break;

        if (! stream.setPosition (stream.getPosition() + static_cast<juce::int64> (size) + (size & 1u)))
            break;
    }

    return std::nullopt;
}

std::optional<ImageData> extractFromMp3 (const juce::File& file)
{
    juce::FileInputStream stream (file);

    if (stream.failedToOpen())
        return std::nullopt;

    unsigned char header[10];

    if (stream.read (header, sizeof (header)) != static_cast<int> (sizeof (header))
        || std::memcmp (header, "ID3", 3) != 0 || ! isSyncsafe (header + 6))
        return std::nullopt;

    const auto tagSize = static_cast<size_t> (readSyncsafe (header + 6));

    if (tagSize > maxId3TagBytes)
        return std::nullopt;

    Bytes tag (10 + tagSize);
    std::memcpy (tag.data(), header, sizeof (header));
    const auto got = stream.read (tag.data() + 10, static_cast<int> (tagSize));
    tag.resize (10 + static_cast<size_t> (std::max (got, 0)));
    return findArtworkInId3 (tag.data(), tag.size());
}
} // namespace

std::optional<ImageData> extractEmbeddedArtwork (const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();

    if (extension == ".mp3")
        return extractFromMp3 (file);

    if (extension == ".wav")
        return extractFromWav (file);

    return std::nullopt;
}

//==============================================================================
juce::Image makeFallbackArtwork (const juce::String& seed, int size)
{
    const auto hash = static_cast<juce::uint64> (seed.hashCode64());
    juce::Random random (static_cast<juce::int64> (hash));
    const auto hue = static_cast<float> (hash % 360u) / 360.0f;
    const auto sizeF = static_cast<float> (size);

    juce::Image image (juce::Image::ARGB, size, size, true);
    juce::Graphics g (image);

    g.setGradientFill (juce::ColourGradient (juce::Colour::fromHSV (hue, 0.55f, 0.55f, 1.0f),
                                             0.0f,
                                             0.0f,
                                             juce::Colour::fromHSV (std::fmod (hue + 0.12f, 1.0f), 0.65f, 0.18f, 1.0f),
                                             sizeF,
                                             sizeF,
                                             false));
    g.fillAll();

    for (int i = 0; i < 7; ++i)
    {
        const auto radius = sizeF * (0.10f + 0.22f * random.nextFloat());
        const auto x = sizeF * random.nextFloat();
        const auto y = sizeF * random.nextFloat() * 0.7f;
        g.setColour (
            juce::Colour::fromHSV (std::fmod (hue + 0.05f * static_cast<float> (i), 1.0f), 0.5f, 0.95f, 0.10f));
        g.fillEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f);
    }

    // A quiet equaliser: one bar per stem-ish column along the bottom.
    constexpr int bars = 16;
    const auto barWidth = sizeF / static_cast<float> (bars);

    for (int i = 0; i < bars; ++i)
    {
        const auto height = sizeF * (0.06f + 0.30f * random.nextFloat());
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillRoundedRectangle (static_cast<float> (i) * barWidth + barWidth * 0.18f,
                                sizeF - height - sizeF * 0.06f,
                                barWidth * 0.64f,
                                height,
                                barWidth * 0.2f);
    }

    return image;
}

juce::MemoryBlock encodePng (const juce::Image& image)
{
    juce::MemoryOutputStream out;
    juce::PNGImageFormat png;
    png.writeImageToStream (image, out);
    return out.getMemoryBlock();
}

juce::String ArtworkChoice::originId() const
{
    switch (origin)
    {
        case Origin::embedded:
            return "embedded";
        case Origin::file:
            return "file";
        case Origin::fallback:
            return "fallback";
    }

    return "fallback";
}

ArtworkChoice makeEmbeddedChoice (ImageData image)
{
    ArtworkChoice choice;
    choice.origin = ArtworkChoice::Origin::embedded;
    choice.image = std::move (image);
    return choice;
}

ArtworkChoice makeFallbackChoice (const juce::String& seed)
{
    ArtworkChoice choice;
    choice.origin = ArtworkChoice::Origin::fallback;
    choice.image.bytes = encodePng (makeFallbackArtwork (seed));
    choice.image.extension = "png";
    return choice;
}

std::optional<ArtworkChoice> loadArtworkFile (const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = file.getFileName() + " was not found.";
        return std::nullopt;
    }

    if (file.getSize() > maxUserImageBytes)
    {
        error = file.getFileName() + " is larger than 16 MB. Choose a smaller picture.";
        return std::nullopt;
    }

    juce::MemoryBlock bytes;

    if (! file.loadFileAsData (bytes))
    {
        error = file.getFileName() + " could not be read.";
        return std::nullopt;
    }

    const auto extension = sniffImageExtension (bytes.getData(), bytes.getSize());

    if (extension.isEmpty() || ! juce::ImageFileFormat::loadFrom (bytes.getData(), bytes.getSize()).isValid())
    {
        error = file.getFileName() + " is not a PNG, JPEG, or GIF picture that SNDS can read.";
        return std::nullopt;
    }

    ArtworkChoice choice;
    choice.origin = ArtworkChoice::Origin::file;
    choice.image.bytes = std::move (bytes);
    choice.image.extension = extension;
    return choice;
}

juce::File writeArtworkToJobDir (const ArtworkChoice& choice, const juce::File& jobDir)
{
    if (! jobDir.createDirectory().wasOk())
        return {};

    const auto file = jobDir.getChildFile ("artwork." + choice.image.extension);
    return file.replaceWithData (choice.image.bytes.getData(), choice.image.bytes.getSize()) ? file : juce::File();
}
} // namespace snds
