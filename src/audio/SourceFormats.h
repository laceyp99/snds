#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <optional>

/**
    Low-level inspection of the two alpha source containers, free of UI and import policy:

    - a RIFF/RF64 WAVE chunk walker that reports the format chunk, the data chunk, and an embedded
      "id3 " chunk, including details JUCE's reader does not expose (format tag, truncation);
    - an MPEG audio frame scanner for MP3 files (ID3v2 skipping, frame-chain validation, Xing/Info
      and LAME headers) that yields an exact audio frame count;
    - readers JUCE lacks: 64-bit float WAV, and a wrapper that gives JUCE's MP3 reader (which
      estimates length from the file size) an exact length.

    Everything here blocks on file I/O: call it off the message thread, never from the audio callback.
*/
namespace snds::audio
{

/** WAVE format tags that SNDS distinguishes (WAVE_FORMAT_* values). */
namespace wavFormatTag
{
inline constexpr juce::uint16 pcm = 0x0001;
inline constexpr juce::uint16 adpcm = 0x0002;
inline constexpr juce::uint16 ieeeFloat = 0x0003;
inline constexpr juce::uint16 alaw = 0x0006;
inline constexpr juce::uint16 mulaw = 0x0007;
inline constexpr juce::uint16 mpeg = 0x0050;
inline constexpr juce::uint16 mpegLayer3 = 0x0055;
inline constexpr juce::uint16 extensible = 0xfffe;
} // namespace wavFormatTag

/** Layout of a WAVE file as declared by its chunks. */
struct WavLayout
{
    bool isRf64 = false;
    bool hasFormatChunk = false;
    juce::uint16 formatTag = 0; ///< Effective tag: WAVE_FORMAT_EXTENSIBLE is resolved to its sub-format.
    bool extensible = false;
    bool knownSubFormat = true; ///< False for an extensible sub-format outside the standard GUID base.
    int numChannels = 0;
    juce::uint32 sampleRate = 0;
    int bitsPerSample = 0; ///< Container size of one sample.
    int validBitsPerSample = 0;
    int blockAlign = 0;

    bool hasDataChunk = false;
    juce::int64 dataOffset = 0;
    juce::int64 dataBytes = 0;  ///< As declared.
    bool dataTruncated = false; ///< The declared data extends past the end of the file.

    juce::int64 id3Offset = -1; ///< Payload offset of an "id3 "/"ID3 " chunk, or -1.
    juce::int64 id3Bytes = 0;   ///< Payload size available in the file.

    juce::int64 getBytesPerFrame() const noexcept;
    juce::int64 getNumFrames() const noexcept;
};

/** Walks the chunks of a RIFF/RF64 WAVE stream. Returns nullopt when it is not RIFF/RF64 WAVE. */
std::optional<WavLayout> readWavLayout (juce::InputStream& input);

//==============================================================================
enum class MpegVersion
{
    mpeg1,
    mpeg2,
    mpeg25,
};

const char* toString (MpegVersion version) noexcept;

struct MpegFrameHeader
{
    MpegVersion version = MpegVersion::mpeg1;
    int layer = 3;
    int bitrateKbps = 0;
    int sampleRate = 0;
    int numChannels = 0;
    int frameBytes = 0;
    int samplesPerFrame = 0;
    bool crcProtected = false;
};

/** Decodes a big-endian 32-bit MPEG audio frame header. Free-format and reserved values are rejected. */
std::optional<MpegFrameHeader> parseMpegFrameHeader (juce::uint32 header) noexcept;

/**
    Total size of an ID3v2 tag (header, body, and footer) from its first 10 bytes; 0 when the bytes
    do not start a plausible ID3v2 tag.
*/
juce::int64 getId3v2TagSize (const juce::uint8* header10) noexcept;

/** Result of walking the MPEG frame chain of an MP3 file. */
struct Mp3Scan
{
    juce::int64 id3v2Bytes = 0;        ///< Leading ID3v2 tag bytes, including repeated tags.
    juce::int64 firstFrameOffset = -1; ///< -1 when no chain of valid frames was found.
    MpegFrameHeader firstFrame;
    juce::int64 audioFrameCount = 0; ///< Frames yielding audio (a leading Xing/Info frame is excluded).
    juce::int64 audioBytes = 0;      ///< Bytes of those frames.
    bool hasXingHeader = false;
    juce::int64 xingFrameCount = -1; ///< Frame count stored in the Xing/Info header, or -1.
    int encoderDelay = -1;           ///< From a LAME tag; -1 when absent.
    int encoderPadding = -1;
    bool variableBitrate = false;    ///< Frame bitrates differ.
    int resyncCount = 0;             ///< Chain breaks that were recovered by searching for the next frame.
    bool inconsistentFrames = false; ///< Later frames changed version, layer, rate, or channel count.

    bool foundAudio() const noexcept { return firstFrameOffset >= 0 && audioFrameCount > 0; }
    juce::int64 getDecodedLength() const noexcept { return audioFrameCount * firstFrame.samplesPerFrame; }
    int getAverageBitrateKbps() const noexcept;
};

/** Scans a whole MP3 stream (reads it once, header by header). */
Mp3Scan scanMp3 (juce::InputStream& input);

//==============================================================================
/** Reader for 64-bit IEEE float WAV data, which JUCE's WAV reader refuses. Samples are narrowed to float. */
std::unique_ptr<juce::AudioFormatReader> createFloat64WavReader (std::unique_ptr<juce::InputStream> input,
                                                                 const WavLayout& layout);

/**
    Wraps a reader, replacing its lengthInSamples. Reads never go past the new length; reads within
    it are forwarded unchanged. Used to give JUCE's MP3 reader the scanned, exact length.
*/
std::unique_ptr<juce::AudioFormatReader> withExactLength (std::unique_ptr<juce::AudioFormatReader> reader,
                                                          juce::int64 lengthInSamples);

} // namespace snds::audio
