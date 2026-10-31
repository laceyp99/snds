#pragma once

#include "audio/FrameSource.h"
#include "audio/PlaybackEngine.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <vector>

namespace snds::test
{
/** A synthetic FrameSource: a pure function of (frame, channel), with an optional delay per read
    to imitate slow storage. Nothing is stored, so "hours" of audio cost no memory. */
class SyntheticSource final : public audio::FrameSource
{
public:
    using Generator = std::function<float (std::int64_t frame, int channel)>;

    SyntheticSource (int rate, std::int64_t frames, Generator generatorToUse, int delayMs = 0)
        : sampleRate (rate),
          length (frames),
          generator (std::move (generatorToUse)),
          readDelayMs (delayMs)
    {
    }

    int getSampleRate() const noexcept override { return sampleRate; }
    std::int64_t getLengthInFrames() const noexcept override { return length; }

    bool read (std::int64_t startFrame, float* const* destination, int numFrames) override
    {
        if (readDelayMs > 0)
            juce::Thread::sleep (readDelayMs);

        for (int i = 0; i < numFrames; ++i)
        {
            const auto frame = startFrame + i;
            const bool inside = frame >= 0 && frame < length;
            destination[0][i] = inside ? generator (frame, 0) : 0.0f;
            destination[1][i] = inside ? generator (frame, 1) : 0.0f;
        }

        return true;
    }

private:
    const int sampleRate;
    const std::int64_t length;
    Generator generator;
    const int readDelayMs;
};

/** A never-zero, frame-unique (within 8191 frames) marker for stem `stem`. Multiples of 1/16384,
    so sums of the four stems are exact in float whatever the order. */
inline float stemMarker (int stem, std::int64_t frame, int channel)
{
    const auto value = (frame * (2 * stem + 1) + 17 * channel + 5 * stem) % 8191 + 1;
    return static_cast<float> (value) / 16384.0f;
}

inline float expectedUnitySum (std::int64_t frame, int channel)
{
    return ((stemMarker (0, frame, channel) + stemMarker (1, frame, channel)) + stemMarker (2, frame, channel))
         + stemMarker (3, frame, channel);
}

inline audio::PlaybackSources makeMarkerSources (int rate, std::int64_t frames, int readDelayMs = 0)
{
    audio::PlaybackSources sources;
    sources.sampleRate = rate;
    sources.lengthFrames = frames;

    for (int stem = 0; stem < audio::stemCount; ++stem)
        sources.stems[static_cast<size_t> (stem)] = std::make_unique<SyntheticSource> (
            rate,
            frames,
            [stem] (std::int64_t frame, int channel) { return stemMarker (stem, frame, channel); },
            readDelayMs);

    return sources;
}

struct Rendered
{
    std::vector<float> left, right;
    bool timedOut = false;

    std::int64_t size() const noexcept { return static_cast<std::int64_t> (left.size()); }
};

/** Drives the engine's process() the way a device would, but offline: no audio device, and
    before each block it waits for the prefetch thread so the render never underruns. */
inline Rendered renderOffline (audio::PlaybackEngine& engine,
                               std::int64_t deviceFrames,
                               int blockSize = 512,
                               bool waitForReadAhead = true)
{
    Rendered rendered;
    rendered.left.reserve (static_cast<size_t> (deviceFrames));
    rendered.right.reserve (static_cast<size_t> (deviceFrames));
    std::vector<float> left (static_cast<size_t> (blockSize)), right (static_cast<size_t> (blockSize));

    while (rendered.size() < deviceFrames)
    {
        const auto count = static_cast<int> (std::min<std::int64_t> (blockSize, deviceFrames - rendered.size()));

        if (waitForReadAhead && ! engine.waitForReadAhead (count, 20000))
        {
            rendered.timedOut = true;
            break;
        }

        float* channels[2] = { left.data(), right.data() };
        engine.process (channels, 2, count);
        rendered.left.insert (rendered.left.end(), left.begin(), left.begin() + count);
        rendered.right.insert (rendered.right.end(), right.begin(), right.begin() + count);
    }

    return rendered;
}

/** Frames of the 10 ms play / pause / seek fade at a device rate (as the engine computes it). */
inline int rampFrames (double deviceRate)
{
    return std::max (2, static_cast<int> (std::lround (deviceRate * 0.010)));
}

/** True if `out` is `expected` scaled by a fade gain k / `scale` (k an integer), or equal. The
    fades are linear in integer steps, so a skipped or misaligned frame will not fit. */
inline bool isFadedCopy (float out, float expected, int scale)
{
    if (out == expected)
        return true;

    const auto ratio = static_cast<double> (out) / static_cast<double> (expected);

    if (ratio < 0.0 || ratio > 1.0 + 1.0e-6)
        return false;

    const auto steps = ratio * scale;
    return std::abs (steps - std::round (steps)) < 1.0e-3;
}

/** Output frames a song of `length` source frames yields at the given rates. */
inline std::int64_t expectedOutputFrames (std::int64_t length, int sourceRate, double deviceRate)
{
    return static_cast<std::int64_t> (std::ceil (static_cast<double> (length) * deviceRate / sourceRate - 1.0e-6));
}
} // namespace snds::test
