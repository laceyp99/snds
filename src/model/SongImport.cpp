#include "model/SongImport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstring>

namespace snds
{
namespace
{
constexpr juce::uint16 wavePcm = 1;
constexpr juce::uint16 waveFloat = 3;
constexpr juce::uint16 waveExtensible = 0xFFFE;

ImportResult reject (const juce::File& file, ImportProblem problem, const juce::String& message)
{
    ImportResult result;
    result.problem = problem;
    result.message = file.getFileName() + ": " + message;
    return result;
}

struct WaveFormat
{
    juce::uint16 tag = 0;
    int channels = 0;
    int bits = 0;
};

juce::uint16 readLe16 (const unsigned char* bytes)
{
    return static_cast<juce::uint16> (bytes[0] | (bytes[1] << 8));
}

juce::uint32 readLe32 (const unsigned char* bytes)
{
    return static_cast<juce::uint32> (bytes[0]) | (static_cast<juce::uint32> (bytes[1]) << 8)
         | (static_cast<juce::uint32> (bytes[2]) << 16) | (static_cast<juce::uint32> (bytes[3]) << 24);
}

/** Reads the RIFF/RF64 container up to the `fmt ` chunk. Returns false if it is not a WAV. */
bool readWaveFormat (const juce::File& file, WaveFormat& format)
{
    juce::FileInputStream stream (file);

    if (stream.failedToOpen())
        return false;

    unsigned char header[12];

    if (stream.read (header, sizeof (header)) != static_cast<int> (sizeof (header))
        || (std::memcmp (header, "RIFF", 4) != 0 && std::memcmp (header, "RF64", 4) != 0)
        || std::memcmp (header + 8, "WAVE", 4) != 0)
        return false;

    // The fmt chunk is normally first, but tolerate others (JUNK, bext, ds64) before it.
    for (int chunks = 0; chunks < 64 && ! stream.isExhausted(); ++chunks)
    {
        unsigned char chunkHeader[8];

        if (stream.read (chunkHeader, sizeof (chunkHeader)) != static_cast<int> (sizeof (chunkHeader)))
            break;

        const auto size = static_cast<juce::int64> (readLe32 (chunkHeader + 4));

        if (std::memcmp (chunkHeader, "fmt ", 4) == 0)
        {
            unsigned char body[40] {};
            const auto toRead = static_cast<int> (juce::jmin<juce::int64> (size, sizeof (body)));

            if (toRead < 16 || stream.read (body, toRead) != toRead)
                return false;

            format.tag = readLe16 (body);
            format.channels = readLe16 (body + 2);
            format.bits = readLe16 (body + 14);

            // WAVE_FORMAT_EXTENSIBLE: the real codec is the first two bytes of the sub-format GUID.
            if (format.tag == waveExtensible && toRead >= 26)
                format.tag = readLe16 (body + 24);

            return true;
        }

        if (std::memcmp (chunkHeader, "data", 4) == 0)
            return false; // audio before any format description

        if (! stream.setPosition (stream.getPosition() + size + (size & 1)))
            break;
    }

    return false;
}

juce::String codecName (juce::uint16 tag)
{
    switch (tag)
    {
        case 2:
            return "Microsoft ADPCM";
        case 6:
            return "A-law";
        case 7:
            return "mu-law";
        case 0x11:
            return "IMA ADPCM";
        case 0x55:
            return "MP3-in-WAV";
        default:
            return "format code 0x" + juce::String::toHexString (static_cast<int> (tag));
    }
}

ImportResult checkStreamLimits (const juce::File& file, int channels, double sampleRate, juce::int64 frames)
{
    if (channels > importlimits::maxChannels)
        return reject (file,
                       ImportProblem::tooManyChannels,
                       "this file has " + juce::String (channels)
                           + " channels. SNDS handles mono or stereo; mix it down to stereo first.");

    if (sampleRate < importlimits::minSampleRate)
        return reject (file,
                       ImportProblem::sampleRateTooLow,
                       "the sample rate is below 8 kHz. Resample it to at least 8 kHz and try again.");

    if (sampleRate > importlimits::maxSampleRate)
        return reject (file,
                       ImportProblem::sampleRateTooHigh,
                       "the sample rate is " + juce::String (sampleRate / 1000.0, 1)
                           + " kHz. SNDS supports up to 192 kHz; resample it and try again.");

    if (frames <= 0)
        return reject (file, ImportProblem::zeroLength, "the audio is empty (zero length).");

    if (sampleRate > 0.0 && static_cast<double> (frames) / sampleRate > importlimits::maxDurationSeconds)
        return reject (
            file, ImportProblem::tooLong, "the audio exceeds 30 minutes. Trim it to 30 minutes or less and try again.");

    return {};
}
} // namespace

ImportResult validateSourceFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return reject (
            file, ImportProblem::notFound, "the file was not found. Check that it was not moved or deleted.");

    const auto extension = file.getFileExtension().toLowerCase();

    if (extension != ".wav" && extension != ".mp3")
        return reject (file,
                       ImportProblem::unsupportedExtension,
                       "SNDS imports WAV and MP3 files. Convert this file to WAV or MP3 and try again.");

    const auto size = file.getSize();

    if (size <= 0)
        return reject (file, ImportProblem::empty, "the file is empty.");

    SourceInfo info;
    info.file = file;
    info.sizeBytes = size;

    juce::AudioFormatManager formats;

    if (extension == ".wav")
    {
        info.format = "wav";
        WaveFormat wave;

        if (! readWaveFormat (file, wave))
            return reject (file,
                           ImportProblem::unreadable,
                           "this does not look like a valid WAV file (missing or damaged header). "
                           "Re-export it from your audio editor.");

        if (wave.tag != wavePcm && wave.tag != waveFloat)
            return reject (file,
                           ImportProblem::unsupportedCodec,
                           "this WAV is compressed (" + codecName (wave.tag)
                               + "). Export it as uncompressed PCM or float WAV.");

        const bool isFloat = wave.tag == waveFloat;

        if (isFloat ? wave.bits != 32 : (wave.bits != 16 && wave.bits != 24 && wave.bits != 32))
            return reject (file,
                           ImportProblem::unsupportedBitDepth,
                           juce::String (wave.bits) + "-bit " + (isFloat ? "float" : "PCM")
                               + " WAV is not supported. Use 16, 24, or 32-bit PCM, or 32-bit float.");

        if (wave.channels > importlimits::maxChannels)
            return checkStreamLimits (file, wave.channels, 0.0, 1);

        info.bitsPerSample = wave.bits;
        info.floatingPoint = isFloat;
        formats.registerFormat (new juce::WavAudioFormat(), true);
    }
    else
    {
        info.format = "mp3";
        formats.registerFormat (new juce::MP3AudioFormat(), true);
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return reject (file,
                       ImportProblem::unreadable,
                       "the audio could not be read. The file may be damaged; try exporting it again.");

    info.sampleRate = reader->sampleRate;
    info.channels = static_cast<int> (reader->numChannels);
    info.frames = reader->lengthInSamples;
    info.durationSeconds = info.sampleRate > 0.0 ? static_cast<double> (info.frames) / info.sampleRate : 0.0;

    if (auto problem = checkStreamLimits (file, info.channels, info.sampleRate, info.frames); ! problem.accepted())
        return problem;

    ImportResult result;
    result.info = info;
    return result;
}
} // namespace snds
