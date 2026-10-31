#pragma once

#include "audio/FrameSource.h"
#include "audio/StemMixStage.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace snds::audio
{
/** What to play: four stem sources (drums, bass, other, vocals) at one sample rate, all as long
    as the song. Streams shorter than `lengthFrames` are zero-padded. */
struct PlaybackSources
{
    std::array<std::unique_ptr<FrameSource>, stemCount> stems;
    /** The original recording, aligned to the stems (see planOriginal); null when missing or
        unusable, in which case Original A/B is unavailable. */
    std::unique_ptr<FrameSource> original;
    int sampleRate = 0;
    std::int64_t lengthFrames = 0;
};

struct PlaybackConfig
{
    int chunkFrames = 256;              // source frames per mix-stage call
    int ioChunkFrames = 4096;           // source frames per disk read
    double readAheadSeconds = 2.0;      // ring size per stream
    double safeReadAheadSeconds = 0.25; // buffered before playing (or after an underrun)
};

/** The streaming engine: four prefetched stems -> StemMixStage (source rate) -> one
    source-to-device resampler -> device output.

    One transport clock, in source-rate frames, is the only position authority: every stream
    is read for the same frame range in lockstep, and the position reported is the source-frame
    time of the next device frame (the resampler's output time).

    Threads:
      - process() is the audio callback. It does not allocate, lock (it only try-locks the
        short session swap), touch files, log, or call the worker or renderer.
      - everything else is for the message thread (or a test driving the engine offline).
      - the prefetch thread owned by the loaded song does the file reads.

    Underruns: if any stream cannot supply the next chunk, nothing is consumed from any
    stream, the block's remainder is silent, the underrun is counted, and the engine waits for
    safe read-ahead before continuing. Streams therefore stay frame-aligned; the audio is
    delayed by the silence, never skewed.
*/
class PlaybackEngine
{
public:
    explicit PlaybackEngine (PlaybackConfig config = {});
    ~PlaybackEngine();

    /** Makes the mix stage for each loaded song. Message thread; takes effect at next load(). */
    void setMixStageFactory (std::function<std::unique_ptr<StemMixStage>()> factory);

    /** Sets the device rate and largest callback block. Allocates; call before load() and
        again when the device's rate or block size changes. */
    void prepare (double deviceSampleRate, int maxBlockFrames);

    /** Replaces the loaded song (if any) and starts prefetching from `startFrame`. The engine
        is paused afterwards. Returns false if prepare() was not called or the sources are
        unusable. */
    bool load (PlaybackSources sources, std::int64_t startFrame = 0);
    void unload();
    bool isLoaded() const noexcept { return loaded.load (std::memory_order_acquire); }

    //==============================================================================
    // Transport (message thread). Changes are click-free: play and pause fade over ~10 ms,
    // and so does the start after a seek.

    /** Starts or resumes; at the end of the song it restarts from the beginning. */
    void play();
    void pause();

    /** Pauses and returns to frame 0. */
    void stop();

    /** Moves every stream to `frame` (source-rate frames, clamped to [0, length]). While playing
        the current audio fades out first; playback resumes once all streams hold safe read-ahead. */
    void seek (std::int64_t frame);

    /** Whole-track loop. The wrap is made by the prefetch streams (data is continuous) with a
        short dip (~5 ms) around the seam so the jump cannot click. */
    void setLooping (bool shouldLoop);
    bool isLooping() const noexcept { return looping.load (std::memory_order_acquire); }

    /** Low-level: what play() and pause() set. */
    void setPlaying (bool shouldPlay) noexcept { playing.store (shouldPlay, std::memory_order_release); }
    bool isPlaying() const noexcept { return playing.load (std::memory_order_acquire); }

    //==============================================================================
    // Original / Stem Mix A/B. The two are mutually exclusive: the output is the stem mix, the
    // original, or (for ~30 ms after a switch) a crossfade whose weights always sum to 1, so
    // the aligned signals never add up to more than either.

    bool hasOriginal() const noexcept { return originalPresent.load (std::memory_order_acquire); }

    /** Returns false (and stays on the stem mix) if there is no original. */
    bool setOriginalSelected (bool shouldPlayOriginal);
    bool isOriginalSelected() const noexcept { return originalSelected.load (std::memory_order_acquire); }

    /** The output device stopped or went away (any thread except the audio callback). Pauses the
        transport at its position without a fade, so nothing waits for a callback that will not
        come; seeks then take effect at once and a new prepare() restarts the streams at the
        playhead. Resume with play() when a device is running again. */
    void deviceStopped();

    /** True while any sound is still being produced (including a fade-out after pause). */
    bool isAudible() const noexcept { return audible.load (std::memory_order_acquire); }

    //==============================================================================
    /** The audio callback. Writes `numFrames` to the first two channels of `output` (a mono
        output gets the average); further channels are cleared. */
    void process (float* const* output, int numChannels, int numFrames) noexcept;

    //==============================================================================
    // Status, readable from any thread.

    /** The transport clock: source frames, clamped to [0, length]. */
    std::int64_t getPositionFrames() const noexcept { return positionFrames.load (std::memory_order_acquire); }
    std::int64_t getLengthFrames() const noexcept { return lengthFrames.load (std::memory_order_acquire); }
    int getSourceSampleRate() const noexcept { return sourceRate.load (std::memory_order_acquire); }
    double getPositionSeconds() const noexcept
    {
        const auto rate = getSourceSampleRate();
        return rate > 0 ? static_cast<double> (getPositionFrames()) / rate : 0.0;
    }
    double getDurationSeconds() const noexcept
    {
        const auto rate = getSourceSampleRate();
        return rate > 0 ? static_cast<double> (getLengthFrames()) / rate : 0.0;
    }
    double getDeviceSampleRate() const noexcept { return deviceRate; }

    /** True while the engine waits for read-ahead before (re)starting. */
    bool isBuffering() const noexcept { return buffering.load (std::memory_order_acquire); }
    bool hasReachedEnd() const noexcept { return reachedEnd.load (std::memory_order_acquire); }
    int getUnderrunCount() const noexcept { return underruns.load (std::memory_order_relaxed); }
    int getReadFailureCount() const noexcept;

    /** Memory held for streaming: independent of song length. */
    std::size_t getReservedBytes() const noexcept { return reservedBytes.load (std::memory_order_relaxed); }

    /** For offline rendering only (no device callback may be running): waits until the
        streams hold enough read-ahead for `process()` of `deviceFrames` not to underrun. */
    bool waitForReadAhead (int deviceFrames, int timeoutMs);

private:
    struct Session;

    void publishStatus (const Session& session) noexcept;
    void requestSeek (std::int64_t frame);

    const PlaybackConfig config;
    std::function<std::unique_ptr<StemMixStage>()> mixStageFactory;

    double deviceRate = 0.0;
    int maxBlock = 0;

    juce::SpinLock sessionLock; // guards only the swap of `session`; the audio thread try-locks it
    std::unique_ptr<Session> session;
    std::array<std::vector<float>, 2> monoScratch;

    std::atomic<bool> loaded { false };
    std::atomic<bool> playing { false };
    std::atomic<bool> looping { false };
    std::atomic<bool> audible { false };
    std::atomic<bool> originalPresent { false };
    std::atomic<bool> originalSelected { false };
    std::atomic<bool> buffering { false };
    std::atomic<bool> reachedEnd { false };
    std::atomic<std::int64_t> positionFrames { 0 };
    std::atomic<std::int64_t> lengthFrames { 0 };
    std::atomic<int> sourceRate { 0 };
    std::atomic<int> underruns { 0 };
    std::atomic<std::size_t> reservedBytes { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackEngine)
};
} // namespace snds::audio
