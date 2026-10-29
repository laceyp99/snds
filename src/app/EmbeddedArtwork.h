#pragma once

#include "model/SongSource.h"

#include <juce_graphics/juce_graphics.h>

#include <optional>
#include <vector>

/**
    Artwork discovery for the import flow: embedded pictures from ID3v2 tags (MP3 files and WAV
    "id3 " chunks), user-chosen replacement images, and guarded image decoding.

    All functions block on file I/O and image decoding: call them off the message thread. Decoded
    images are software images, so they can be handed to the message thread afterwards.
*/
namespace snds
{

/** One picture found in an ID3v2 tag (APIC in v2.3/v2.4, PIC in v2.2). */
struct Id3Picture
{
    int pictureType = 0;   ///< ID3 picture type: 3 = front cover, 0 = other, ...
    juce::String mimeType; ///< e.g. "image/png"; v2.2 "PNG"/"JPG" are mapped to MIME types.
    juce::String description;
    juce::MemoryBlock data;
};

inline constexpr int id3FrontCoverType = 3;

/**
    Parses the pictures of one ID3v2.2/2.3/2.4 tag starting at tagData (the "ID3" header).

    Handles tag-level (v2.2/2.3) and frame-level (v2.4) unsynchronisation, extended headers, v2.4
    data length indicators and grouping bytes, zlib-compressed frames, and v2.4 tags written with
    v2.3-style frame sizes. Encrypted frames and linked ("-->") images are skipped. Malformed input
    yields whatever was parsed before the damage; it never reads out of bounds.
*/
std::vector<Id3Picture> parseId3Pictures (const void* tagData, size_t numBytes);

/** Limits that protect the app from hostile or absurd images. */
struct ArtworkLimits
{
    static constexpr size_t maxEncodedBytes = 32 * 1024 * 1024;
    static constexpr int maxSourceDimension = 12000;
    static constexpr juce::int64 maxSourcePixels = 64LL * 1000 * 1000;
    static constexpr int maxStoredDimension = 2048; ///< Larger images are downscaled after decoding.
};

struct DecodedImage
{
    juce::Image image;      ///< Invalid on failure.
    juce::String imageType; ///< "PNG", "JPEG", or "GIF".
    int sourceWidth = 0;    ///< Dimensions before any downscaling.
    int sourceHeight = 0;
    juce::String error; ///< Why decoding failed; empty on success.
};

/** Decodes PNG, JPEG, or GIF bytes into an ARGB software image no larger than maxStoredDimension. */
DecodedImage decodeArtworkImage (const void* data, size_t numBytes);

struct EmbeddedArtworkResult
{
    std::optional<model::Artwork> artwork;
    juce::String note; ///< What was (or was not) found, for the confirmation screen and logs.
};

/**
    Reads artwork from an accepted source: the leading ID3v2 tags of an MP3, or the "id3 "/"ID3 "
    chunk of a WAV. The front cover is preferred; other pictures are used in tag order if it is
    missing or cannot be decoded. Other WAV metadata (LIST/INFO, bext) never carries images.
*/
EmbeddedArtworkResult extractEmbeddedArtwork (const model::SourceInfo& source);

/** juce::FileChooser patterns for replacement artwork. */
inline constexpr const char* artworkFilePatterns = "*.png;*.jpg;*.jpeg";

struct ReplacementArtworkResult
{
    std::optional<model::Artwork> artwork;
    juce::String error; ///< User-facing reason when artwork is empty.
};

/** Loads a user-chosen PNG or JPEG as replacement artwork. */
ReplacementArtworkResult loadReplacementArtwork (const juce::File& imageFile);

} // namespace snds
