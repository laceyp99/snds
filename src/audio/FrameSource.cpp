#include "audio/FrameSource.h"

#include <algorithm>
#include <cstring>

namespace snds::audio
{
ReaderFrameSource::ReaderFrameSource (std::unique_ptr<juce::AudioFormatReader> readerToUse,
                                      std::int64_t leadingFramesToSkip,
                                      std::int64_t usableFrames)
    : reader (std::move (readerToUse)),
      sampleRate (reader != nullptr ? static_cast<int> (reader->sampleRate) : 0),
      channels (reader != nullptr ? static_cast<int> (reader->numChannels) : 0),
      skip (std::max<std::int64_t> (0, leadingFramesToSkip))
{
    const auto decoded = reader != nullptr ? std::max<std::int64_t> (0, reader->lengthInSamples - skip) : 0;
    length = usableFrames >= 0 ? std::min (decoded, usableFrames) : decoded;
}

bool ReaderFrameSource::read (std::int64_t startFrame, float* const* destination, int numFrames)
{
    for (int channel = 0; channel < 2; ++channel)
        std::memset (destination[channel], 0, sizeof (float) * static_cast<size_t> (numFrames));

    if (reader == nullptr || channels < 1)
        return false;

    // Only the part that overlaps [0, length) is read; the rest stays zero.
    const auto first = std::max<std::int64_t> (startFrame, 0);
    const auto last = std::min<std::int64_t> (startFrame + numFrames, length);

    if (last <= first)
        return true;

    const auto offset = static_cast<int> (first - startFrame);
    const auto count = static_cast<int> (last - first);
    float* targets[2] = { destination[0] + offset, destination[1] + offset };

    if (! reader->read (targets, std::min (channels, 2), skip + first, count))
        return false;

    if (channels == 1)
        std::memcpy (targets[1], targets[0], sizeof (float) * static_cast<size_t> (count));

    return true;
}

std::unique_ptr<ReaderFrameSource> openFrameSource (const juce::File& file,
                                                    juce::AudioFormatManager& formats,
                                                    juce::String& error,
                                                    std::int64_t leadingFramesToSkip,
                                                    std::int64_t usableFrames)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0 || reader->numChannels < 1)
    {
        error = "Cannot read audio from " + file.getFullPathName();
        return nullptr;
    }

    return std::make_unique<ReaderFrameSource> (std::move (reader), leadingFramesToSkip, usableFrames);
}
} // namespace snds::audio
