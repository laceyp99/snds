#include "app/SongImport.h"

#include "audio/SourceFormats.h"

#include <cmath>
#include <cstring>

namespace snds
{

using model::SourceFormat;
using model::SourceInfo;

namespace
{
constexpr double probeSeconds = 1.0;

ImportResult fail (ImportErrorCode code, juce::String message, juce::String action)
{
    ImportResult result;
    result.error = { code, std::move (message), std::move (action) };
    return result;
}

juce::String quoted (const juce::File& file)
{
    return "\"" + file.getFileName() + "\"";
}

/** Names a container recognised from its first bytes, or returns an empty string. */
juce::String sniffContainer (const juce::File& file)
{
    juce::uint8 head[12] {};

    if (auto stream = file.createInputStream(); stream == nullptr || stream->read (head, sizeof (head)) < 4)
        return {};

    const auto startsWith = [&head] (const char* magic, int offset = 0)
    { return std::memcmp (head + offset, magic, std::strlen (magic)) == 0; };

    if ((startsWith ("RIFF") || startsWith ("RF64")) && startsWith ("WAVE", 8))
        return "WAV";

    if (startsWith ("ID3") || (head[0] == 0xff && (head[1] & 0xe0) == 0xe0))
        return "MP3";

    if (startsWith ("fLaC"))
        return "FLAC";

    if (startsWith ("OggS"))
        return "Ogg";

    if (startsWith ("FORM"))
        return "AIFF";

    if (startsWith ("ftyp", 4))
        return "MPEG-4 (AAC/ALAC)";

    return {};
}

ImportResult failWrongExtension (const juce::File& file, const juce::String& actual)
{
    return fail (ImportErrorCode::formatMismatch,
                 quoted (file) + " is a " + actual + " file with a ." + file.getFileExtension().substring (1)
                     + " extension.",
                 "Rename it to ." + actual.toLowerCase() + " and open it again.");
}

ImportResult failNotThisFormat (const juce::File& file, SourceFormat expected)
{
    const auto expectedName = juce::String (model::toString (expected));
    const auto actual = sniffContainer (file);

    if ((actual == "WAV" || actual == "MP3") && actual != expectedName)
        return failWrongExtension (file, actual);

    if (actual.isNotEmpty() && actual != expectedName)
        return fail (ImportErrorCode::formatMismatch,
                     quoted (file) + " contains " + actual + " audio, not " + expectedName + ".",
                     "SNDS alpha opens WAV and MP3 only. Convert the song to WAV or MP3 first.");

    return fail (ImportErrorCode::corrupt,
                 quoted (file) + " is not a readable " + expectedName + " file.",
                 "It may be damaged or incomplete. Export or download it again, then retry.");
}

juce::String describeWavCodec (juce::uint16 tag)
{
    switch (tag)
    {
        case audio::wavFormatTag::adpcm:
            return "Microsoft ADPCM";
        case 0x0011:
            return "IMA ADPCM";
        case audio::wavFormatTag::alaw:
            return "A-law";
        case audio::wavFormatTag::mulaw:
            return "mu-law";
        case audio::wavFormatTag::mpeg:
            return "MPEG";
        case audio::wavFormatTag::mpegLayer3:
            return "MP3";
        default:
            break;
    }

    return "format 0x" + juce::String::toHexString (static_cast<int> (tag)).paddedLeft ('0', 4).toUpperCase();
}

std::optional<ImportResult> checkChannelsAndRate (const juce::File& file, int numChannels, double sampleRate)
{
    if (numChannels <= 0 || sampleRate <= 0.0)
        return fail (ImportErrorCode::corrupt,
                     quoted (file) + " has an invalid audio header (no channels or no sample rate).",
                     "The file is damaged. Export it again, then retry.");

    if (numChannels > SourceLimits::maxChannels)
        return fail (ImportErrorCode::tooManyChannels,
                     quoted (file) + " has " + juce::String (numChannels)
                         + " channels. SNDS alpha supports mono and stereo only.",
                     "Downmix it to stereo in your audio editor, then open the stereo file.");

    if (sampleRate > SourceLimits::maxSampleRate)
        return fail (ImportErrorCode::sampleRateOutOfRange,
                     quoted (file) + " uses a " + model::formatSampleRate (sampleRate)
                         + " sample rate. SNDS supports up to 192 kHz.",
                     "Resample it to 192 kHz or lower (44.1 or 48 kHz is plenty for separation), then retry.");

    if (sampleRate < SourceLimits::minSampleRate)
        return fail (ImportErrorCode::sampleRateOutOfRange,
                     quoted (file) + " uses a " + juce::String (sampleRate, 0)
                         + " Hz sample rate. SNDS needs at least 8 kHz.",
                     "Resample it to 44.1 or 48 kHz, then retry.");

    return std::nullopt;
}

/** Decodes up to the first second and checks the samples are finite. */
std::optional<ImportResult> probeDecode (const juce::File& file, juce::AudioFormatReader& reader)
{
    const auto frames = static_cast<int> (
        juce::jmin<juce::int64> (reader.lengthInSamples, static_cast<juce::int64> (reader.sampleRate * probeSeconds)));
    juce::AudioBuffer<float> buffer (static_cast<int> (reader.numChannels), frames);

    if (! reader.read (&buffer, 0, frames, 0, true, true))
        return fail (ImportErrorCode::corrupt,
                     "SNDS could not decode the start of " + quoted (file) + ".",
                     "The file is damaged or uses an unusual encoding. Export it again as WAV or MP3.");

    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto* samples = buffer.getReadPointer (channel);

        for (auto i = 0; i < frames; ++i)
            if (! std::isfinite (samples[i]))
                return fail (ImportErrorCode::corrupt,
                             quoted (file) + " contains invalid samples (NaN or infinity).",
                             "Export it again from your audio editor, then retry.");
    }

    return std::nullopt;
}

std::unique_ptr<juce::AudioFormatReader> openWavReader (const juce::File& file, const audio::WavLayout& layout)
{
    auto stream = file.createInputStream();

    if (stream == nullptr)
        return nullptr;

    if (layout.bitsPerSample == 64)
        return audio::createFloat64WavReader (std::move (stream), layout);

    juce::WavAudioFormat wav;
    return std::unique_ptr<juce::AudioFormatReader> (wav.createReaderFor (stream.release(), true));
}

std::unique_ptr<juce::AudioFormatReader> openMp3Reader (const juce::File& file, juce::int64 exactLength)
{
    auto stream = file.createInputStream();

    if (stream == nullptr)
        return nullptr;

    juce::MP3AudioFormat mp3;
    std::unique_ptr<juce::AudioFormatReader> reader (mp3.createReaderFor (stream.release(), true));
    return audio::withExactLength (std::move (reader), exactLength);
}

ImportResult inspectWav (const juce::File& file, juce::InputStream& stream, SourceInfo info)
{
    const auto layout = audio::readWavLayout (stream);

    if (! layout.has_value())
        return failNotThisFormat (file, SourceFormat::wav);

    if (! layout->hasFormatChunk)
        return fail (ImportErrorCode::corrupt,
                     quoted (file) + " has no WAV format description.",
                     "The file is damaged. Export it again, then retry.");

    const auto tag = layout->formatTag;
    const auto bits = layout->bitsPerSample;

    if (layout->extensible && ! layout->knownSubFormat)
        return fail (ImportErrorCode::unsupportedEncoding,
                     quoted (file) + " uses a WAV sub-format that SNDS does not recognise.",
                     "Export it again as 16-bit or 24-bit PCM WAV, then retry.");

    if (tag == audio::wavFormatTag::pcm)
    {
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
            return fail (ImportErrorCode::unsupportedBitDepth,
                         quoted (file) + " stores " + juce::String (bits) + "-bit PCM samples, which SNDS can't read.",
                         "Export it again as 16-bit or 24-bit PCM WAV, then retry.");
    }
    else if (tag == audio::wavFormatTag::ieeeFloat)
    {
        if (bits != 32 && bits != 64)
            return fail (ImportErrorCode::unsupportedBitDepth,
                         quoted (file) + " stores " + juce::String (bits)
                             + "-bit float samples; SNDS reads 32-bit and 64-bit float.",
                         "Export it again as 32-bit float or 24-bit PCM WAV, then retry.");
    }
    else
    {
        return fail (ImportErrorCode::unsupportedEncoding,
                     quoted (file) + " is a compressed WAV (" + describeWavCodec (tag) + ").",
                     "Export it again as uncompressed PCM or float WAV, or as MP3, then retry.");
    }

    if (auto problem = checkChannelsAndRate (file, layout->numChannels, layout->sampleRate))
        return *problem;

    if (! layout->hasDataChunk)
        return fail (ImportErrorCode::noAudio,
                     quoted (file) + " has no audio data.",
                     "Choose a WAV file that contains audio.");

    if (layout->dataTruncated)
        return fail (ImportErrorCode::truncated,
                     quoted (file) + " is incomplete: its audio data stops before the length the file declares.",
                     "The copy is damaged or was still being written. Copy or export it again, then retry.");

    if (layout->getNumFrames() <= 0)
        return fail (ImportErrorCode::noAudio,
                     quoted (file) + " contains no audio (zero length).",
                     "Choose a WAV file that contains audio.");

    auto reader = openWavReader (file, *layout);

    const auto readable = reader != nullptr && static_cast<int> (reader->numChannels) == layout->numChannels
                          && reader->lengthInSamples > 0;

    // JUCE only reads RF64 whose data chunk declares its real size, or that really exceeds 4 GB.
    if (! readable && layout->isRf64)
        return fail (ImportErrorCode::unsupportedEncoding,
                     quoted (file) + " is an RF64 (large-file) WAV that SNDS can't read.",
                     "Export it again as a standard WAV (16/24-bit PCM or 32-bit float), then retry.");

    if (! readable)
        return fail (ImportErrorCode::corrupt,
                     "SNDS could not decode " + quoted (file) + ".",
                     "The file may be damaged. Export it again as 16-bit or 24-bit PCM WAV, then retry.");

    if (auto problem = probeDecode (file, *reader))
        return *problem;

    info.sampleRate = reader->sampleRate;
    info.numChannels = static_cast<int> (reader->numChannels);
    info.lengthInFrames = reader->lengthInSamples;
    info.bitsPerSample = bits;
    info.floatingPoint = tag == audio::wavFormatTag::ieeeFloat;

    if (info.floatingPoint)
        info.encoding = juce::String (bits) + "-bit float";
    else if (layout->validBitsPerSample > 0 && layout->validBitsPerSample < bits)
        info.encoding = juce::String (layout->validBitsPerSample) + "-bit PCM in " + juce::String (bits) + "-bit";
    else
        info.encoding = juce::String (bits) + "-bit PCM";

    if (layout->isRf64)
        info.encoding << " (RF64)";

    ImportResult result;
    result.source = std::move (info);
    return result;
}

ImportResult inspectMp3 (const juce::File& file, juce::InputStream& stream, SourceInfo info)
{
    const auto scan = audio::scanMp3 (stream);

    if (! scan.foundAudio())
        return failNotThisFormat (file, SourceFormat::mp3);

    const auto& frame = scan.firstFrame;

    if (frame.layer != 3)
        return fail (ImportErrorCode::unsupportedEncoding,
                     quoted (file) + " is MPEG Layer " + juce::String (frame.layer == 1 ? "I" : "II")
                         + " audio, not MP3 (Layer III).",
                     "Convert it to MP3 or WAV, then retry.");

    if (frame.version != audio::MpegVersion::mpeg1)
        return fail (ImportErrorCode::unsupportedEncoding,
                     quoted (file) + " is a low-sample-rate MP3 (" + audio::toString (frame.version) + ", "
                         + model::formatSampleRate (frame.sampleRate)
                         + "), which SNDS can't play with accurate timing.",
                     "Re-encode it as MP3 at 44.1 or 48 kHz, or convert it to WAV, then retry.");

    if (auto problem = checkChannelsAndRate (file, frame.numChannels, frame.sampleRate))
        return *problem;

    auto reader = openMp3Reader (file, scan.getDecodedLength());

    if (reader == nullptr || static_cast<int> (reader->numChannels) != frame.numChannels
        || juce::roundToInt (reader->sampleRate) != frame.sampleRate)
        return fail (ImportErrorCode::corrupt,
                     "SNDS could not decode " + quoted (file) + ".",
                     "The file may be damaged. Export it again as MP3 or WAV, then retry.");

    if (auto problem = probeDecode (file, *reader))
        return *problem;

    model::Mp3Details details;
    details.audioDataOffset = scan.firstFrameOffset;
    details.audioFrameCount = scan.audioFrameCount;
    details.variableBitrate = scan.variableBitrate;
    details.bitrateKbps = scan.variableBitrate ? scan.getAverageBitrateKbps() : frame.bitrateKbps;
    details.hasXingHeader = scan.hasXingHeader;
    details.encoderDelay = scan.encoderDelay;
    details.encoderPadding = scan.encoderPadding;
    details.resyncCount = scan.resyncCount;

    info.sampleRate = frame.sampleRate;
    info.numChannels = frame.numChannels;
    info.lengthInFrames = reader->lengthInSamples;
    info.encoding = juce::String (audio::toString (frame.version)) + " Layer III, "
                    + (details.variableBitrate ? "VBR ~" : "") + juce::String (details.bitrateKbps) + " kbps";
    info.mp3 = details;

    ImportResult result;
    result.source = std::move (info);
    return result;
}
} // namespace

const char* toString (ImportErrorCode code) noexcept
{
    switch (code)
    {
        case ImportErrorCode::fileNotFound:
            return "file-not-found";
        case ImportErrorCode::unreadable:
            return "unreadable";
        case ImportErrorCode::unsupportedFileType:
            return "unsupported-file-type";
        case ImportErrorCode::formatMismatch:
            return "format-mismatch";
        case ImportErrorCode::unsupportedEncoding:
            return "unsupported-encoding";
        case ImportErrorCode::unsupportedBitDepth:
            return "unsupported-bit-depth";
        case ImportErrorCode::tooManyChannels:
            return "too-many-channels";
        case ImportErrorCode::sampleRateOutOfRange:
            return "sample-rate-out-of-range";
        case ImportErrorCode::noAudio:
            return "no-audio";
        case ImportErrorCode::tooLong:
            return "too-long";
        case ImportErrorCode::truncated:
            return "truncated";
        case ImportErrorCode::corrupt:
            return "corrupt";
    }

    return "unknown";
}

bool hasSupportedSourceExtension (const juce::File& file)
{
    return file.hasFileExtension ("wav;wave;mp3");
}

ImportResult inspectSource (const juce::File& file)
{
    if (file.isDirectory())
        return fail (ImportErrorCode::fileNotFound,
                     quoted (file) + " is a folder, not an audio file.",
                     "Choose a WAV or MP3 file.");

    if (! file.existsAsFile())
        return fail (ImportErrorCode::fileNotFound,
                     quoted (file) + " no longer exists at " + file.getParentDirectory().getFullPathName() + ".",
                     "It may have been moved, renamed, or deleted. Choose the file again.");

    if (! hasSupportedSourceExtension (file))
    {
        const auto content = sniffContainer (file);
        return fail (ImportErrorCode::unsupportedFileType,
                     quoted (file) + " is not a WAV or MP3 file"
                         + (content.isNotEmpty() ? " (it contains " + content + " data)." : juce::String (".")),
                     content == "WAV" || content == "MP3"
                         ? "Rename it to ." + content.toLowerCase() + " and open it again."
                         : juce::String ("SNDS alpha opens .wav and .mp3 files only. Convert the song first."));
    }

    auto stream = file.createInputStream();

    if (stream == nullptr || stream->failedToOpen())
        return fail (ImportErrorCode::unreadable,
                     "SNDS could not open " + quoted (file) + " for reading.",
                     "Check that you have permission to read it and that no other program has it locked, "
                     "then retry.");

    SourceInfo info;
    info.file = file;
    info.fileSizeBytes = file.getSize();
    info.lastModified = file.getLastModificationTime();
    info.format = file.hasFileExtension ("mp3") ? SourceFormat::mp3 : SourceFormat::wav;

    if (info.fileSizeBytes == 0)
        return fail (ImportErrorCode::noAudio,
                     quoted (file) + " is empty (0 bytes).",
                     "The copy may have failed. Copy or export it again, then retry.");

    // A WAV named .mp3 (or the reverse) is common after careless renaming; say so directly.
    const auto content = sniffContainer (file);

    if ((info.format == SourceFormat::mp3 && content == "WAV")
        || (info.format == SourceFormat::wav && content == "MP3"))
        return failWrongExtension (file, content);

    auto result = info.format == SourceFormat::mp3 ? inspectMp3 (file, *stream, std::move (info))
                                                   : inspectWav (file, *stream, std::move (info));

    // Checked here, before artwork and hashing, so the worker never sees a source it would reject.
    if (result.wasOk()
        && static_cast<double> (result.source->lengthInFrames) > SourceLimits::maxSeconds * result.source->sampleRate)
        return fail (ImportErrorCode::tooLong,
                     quoted (file) + " is longer than " + juce::String (SourceLimits::maxSeconds / 60)
                         + " minutes. SNDS alpha separates songs up to that length.",
                     "Trim it or split it into shorter parts, then open one part.");

    return result;
}

std::unique_ptr<juce::AudioFormatReader> createSourceReader (const SourceInfo& source)
{
    if (source.format == SourceFormat::mp3)
    {
        if (! source.mp3.has_value())
            return nullptr;

        return openMp3Reader (source.file, source.lengthInFrames);
    }

    auto stream = source.file.createInputStream();

    if (stream == nullptr)
        return nullptr;

    const auto layout = audio::readWavLayout (*stream);

    if (! layout.has_value())
        return nullptr;

    return openWavReader (source.file, *layout);
}

} // namespace snds
