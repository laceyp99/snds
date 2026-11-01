#pragma once

#include <juce_graphics/juce_graphics.h>

#include <optional>

namespace snds
{
/** An encoded image: PNG, JPEG, or GIF bytes plus the file extension that matches them. */
struct ImageData
{
    juce::MemoryBlock bytes;
    juce::String extension; // "png", "jpg", or "gif"
    int pictureType = -1;   // ID3 APIC picture type when it came from a tag; 3 is the front cover
};

/** "png", "jpg", "gif", or an empty string, from the first bytes of an image. */
juce::String sniffImageExtension (const void* data, size_t size);

/** Finds the best cover picture in an ID3v2.2, 2.3, or 2.4 tag at the start of `data`: the front
    cover if there is one, else the first picture. Handles the unsynchronisation flag, an
    extended header, v2.4 data-length indicators and non-syncsafe sizes, grouping bytes, and
    padding; skips compressed or encrypted frames and picture URLs. Never reads out of bounds.
*/
std::optional<ImageData> findArtworkInId3 (const void* data, size_t size);

/** Cover art embedded in an MP3 (ID3v2 at the start) or a WAV (an `id3 ` or `ID3 ` chunk).
    Other files, and files without a usable picture, give nothing.
*/
std::optional<ImageData> extractEmbeddedArtwork (const juce::File& file);

/** A deterministic, procedurally drawn cover for a song with no art: a hue picked from `seed`,
    a gradient, soft circles, and a row of bars. */
juce::Image makeFallbackArtwork (const juce::String& seed, int size = 512);

juce::MemoryBlock encodePng (const juce::Image& image);

/** What the user decided about a song's artwork. */
struct ArtworkChoice
{
    enum class Origin
    {
        embedded,
        file,
        fallback
    };

    Origin origin = Origin::fallback;
    ImageData image;

    /** "embedded", "file", or "fallback": the `origin` in protocol.md. */
    juce::String originId() const;
};

ArtworkChoice makeEmbeddedChoice (ImageData image);
ArtworkChoice makeFallbackChoice (const juce::String& seed);

/** A picture the user picked. Fails with a message for a file that is missing, too large (over
    16 MB), or not a PNG, JPEG, or GIF that can be decoded.
*/
std::optional<ArtworkChoice> loadArtworkFile (const juce::File& file, juce::String& error);

/** Writes `artwork.<ext>` into jobDir (created if needed) and returns it, or a default File on
    failure. The worker moves it into the package. */
juce::File writeArtworkToJobDir (const ArtworkChoice& choice, const juce::File& jobDir);
} // namespace snds
