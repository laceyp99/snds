#pragma once

#include "audio/FrameSource.h"
#include "audio/SeekTarget.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>

namespace snds::audio
{
/** One stream of stereo frames, prefetched from a FrameSource into a bounded single-producer,
    single-consumer ring.

    Threads:
      - prefetch thread: produce() (the only caller of the FrameSource);
      - audio thread:    available() / read() (no allocation, no locks, no I/O);
      - any thread:      peekAvailable(), hasReachedLimit() and the counters.

    Positions are source-rate frames. Within one generation the ring holds song frames
    [seek.frame, limit) in order: frame `n` of the generation is song frame `seek.frame + n`.
    A new generation in the shared SeekTarget flushes the ring: the producer invalidates what it
    published, repositions and refills, and the consumer adopts the new generation on its next
    call. A read that races a flush is detected and rejected, so stale frames are never returned.

    Memory is fixed at construction: two rings of `ringFrames` plus one chunk of scratch,
    whatever the song length.
*/
class PrefetchStream
{
public:
    /** `limitFrames` is the song length every stream shares; frames the source lacks are zeros. */
    PrefetchStream (std::unique_ptr<FrameSource> source,
                    const SeekTarget& seekTarget,
                    std::int64_t limitFrames,
                    int ringFrames,
                    int chunkFrames,
                    const std::atomic<bool>* loopFlag = nullptr);

    //==============================================================================
    // Prefetch thread.

    /** Reads at most one chunk into the ring. Returns true if it did any work (a flush, a
        chunk, or a failed read), false if the ring is full or the song's end was reached. */
    bool produce();

    //==============================================================================
    // Audio thread.

    /** Frames readable now for generation `generation`; 0 while that generation is being
        refilled. Adopts the generation on first use. */
    int available (std::uint32_t generation) noexcept;

    /** Copies `numFrames` (<= available()) into `destination` (two channels) and consumes them.
        Returns false, consuming nothing, if a seek raced the copy. */
    bool read (std::uint32_t generation, float* const* destination, int numFrames) noexcept;

    //==============================================================================
    // Any thread.

    /** Like available() but does not adopt the generation; for offline harnesses and the UI. */
    int peekAvailable (std::uint32_t generation) const noexcept;

    /** True if every frame up to the song limit for this generation has been read into the
        ring (it may not all have been consumed yet). */
    bool hasReachedLimit (std::uint32_t generation) const noexcept;

    int getRingFrames() const noexcept { return ringFrames; }
    std::size_t getReservedBytes() const noexcept;
    int getSampleRate() const noexcept { return sampleRate; }
    int getReadFailures() const noexcept { return readFailures.load (std::memory_order_relaxed); }

private:
    struct Counter
    {
        std::uint32_t generation;
        std::int64_t count;
    };

    static std::uint64_t pack (Counter counter) noexcept
    {
        return (static_cast<std::uint64_t> (counter.generation) << 40) | static_cast<std::uint64_t> (counter.count);
    }

    static Counter unpack (std::uint64_t word) noexcept
    {
        return { static_cast<std::uint32_t> (word >> 40),
                 static_cast<std::int64_t> (word & ((std::uint64_t { 1 } << 40) - 1)) };
    }

    std::unique_ptr<FrameSource> source;
    const SeekTarget& seekTarget;
    const std::int64_t limit;
    const int ringFrames;
    const int chunkFrames;
    const int sampleRate;
    const std::atomic<bool>* const loop; // when set and true, reading wraps from the limit to frame 0

    std::vector<float> ring[2];
    std::vector<float> scratch[2];

    // Written by the producer, read by the consumer.
    std::atomic<std::uint64_t> published { 0 };     // frames written in the current generation
    std::atomic<std::int64_t> publishedStart { 0 }; // song frame of count 0 of that generation
    // Written by the consumer, read by the producer.
    std::atomic<std::uint64_t> consumed { 0 }; // frames consumed in the consumer's generation

    // Producer-only state.
    std::uint32_t producerGeneration = 0;
    std::int64_t producerStart = 0; // song frame of count 0
    std::int64_t producerHead = 0;  // frames written in this generation

    // Consumer-only state.
    std::uint32_t consumerGeneration = 0;
    std::int64_t consumerTail = 0;

    std::atomic<int> readFailures { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PrefetchStream)
};

/** One background thread that keeps a set of streams filled, round-robin, one chunk each per
    pass. Streams must outlive the thread (stop() or destroy it first). */
class PrefetchThread final : private juce::Thread
{
public:
    explicit PrefetchThread (juce::String name);
    ~PrefetchThread() override;

    /** Adds a stream before start(). */
    void add (PrefetchStream& stream);
    void start();
    void stop();

    /** Wakes the thread after a seek so it does not wait out its idle sleep. */
    void notify() { wake.signal(); }

private:
    void run() override;

    std::vector<PrefetchStream*> streams;
    juce::WaitableEvent wake;
};
} // namespace snds::audio
