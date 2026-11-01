#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>

namespace snds::audio
{
/** A random-access, stereo, float source of audio at one sample rate. Everything the playback
    engine streams (stems and original) is a FrameSource, so tests can supply synthetic signals.

    read() is called from one thread at a time (the prefetch thread), never from the audio
    callback. It may block on disk.
*/
class FrameSource
{
public:
    virtual ~FrameSource() = default;

    virtual int getSampleRate() const noexcept = 0;

    /** Frames the source really has; reads past this are zero-filled. */
    virtual std::int64_t getLengthInFrames() const noexcept = 0;

    /** Writes `numFrames` frames of two channels starting at `startFrame`. Frames outside
        [0, length) are zeros. Returns false if the data could not be read (the destination is
        then zero-filled so the stream stays aligned). */
    virtual bool read (std::int64_t startFrame, float* const* destination, int numFrames) = 0;
};

/** Reads a WAV or MP3 through a JUCE AudioFormatReader. Mono is duplicated to both channels;
    channels beyond the second are ignored.

    `leadingFramesToSkip` drops that many decoded frames from the start (decoder delay of an
    MP3 whose decoder does not trim it); `usableFrames` (when >= 0) caps the length.
*/
class ReaderFrameSource final : public FrameSource
{
public:
    explicit ReaderFrameSource (std::unique_ptr<juce::AudioFormatReader> reader,
                                std::int64_t leadingFramesToSkip = 0,
                                std::int64_t usableFrames = -1);

    int getSampleRate() const noexcept override { return sampleRate; }
    std::int64_t getLengthInFrames() const noexcept override { return length; }
    bool read (std::int64_t startFrame, float* const* destination, int numFrames) override;

    int getSourceChannelCount() const noexcept { return channels; }

private:
    std::unique_ptr<juce::AudioFormatReader> reader;
    int sampleRate = 0;
    int channels = 0;
    std::int64_t skip = 0;
    std::int64_t length = 0;
};

/** Opens a WAV or MP3 as a FrameSource. Returns null (with `error` set) if it cannot be read. */
std::unique_ptr<ReaderFrameSource> openFrameSource (const juce::File& file,
                                                    juce::AudioFormatManager& formats,
                                                    juce::String& error,
                                                    std::int64_t leadingFramesToSkip = 0,
                                                    std::int64_t usableFrames = -1);
} // namespace snds::audio
