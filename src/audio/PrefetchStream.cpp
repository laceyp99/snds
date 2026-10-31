#include "audio/PrefetchStream.h"

#include <algorithm>
#include <cstring>

namespace snds::audio
{
PrefetchStream::PrefetchStream (std::unique_ptr<FrameSource> sourceToUse,
                                const SeekTarget& target,
                                std::int64_t limitFrames,
                                int ringSize,
                                int chunkSize,
                                const std::atomic<bool>* loopFlag)
    : source (std::move (sourceToUse)),
      seekTarget (target),
      limit (limitFrames),
      ringFrames (ringSize),
      chunkFrames (chunkSize),
      sampleRate (source->getSampleRate()),
      loop (loopFlag)
{
    jassert (chunkFrames > 0 && ringFrames >= 2 * chunkFrames);

    for (int channel = 0; channel < 2; ++channel)
    {
        ring[channel].assign (static_cast<size_t> (ringFrames), 0.0f);
        scratch[channel].assign (static_cast<size_t> (chunkFrames), 0.0f);
    }

    // The current generation at construction is the initial position; produce() starts filling it.
    const auto initial = seekTarget.load();
    producerGeneration = initial.generation;
    consumerGeneration = initial.generation;
    producerStart = initial.frame;
    publishedStart.store (producerStart);
    published.store (pack ({ producerGeneration, 0 }));
    consumed.store (pack ({ producerGeneration, 0 }));
}

std::size_t PrefetchStream::getReservedBytes() const noexcept
{
    return sizeof (float) * 2 * static_cast<std::size_t> (ringFrames + chunkFrames);
}

bool PrefetchStream::produce()
{
    const auto target = seekTarget.load();
    bool didWork = false;

    if (target.generation != producerGeneration)
    {
        // Invalidate first, then touch the ring: a consumer that copied stale frames checks
        // `published` after copying and rejects them.
        producerGeneration = target.generation;
        producerStart = target.frame;
        producerHead = 0;
        publishedStart.store (producerStart, std::memory_order_relaxed);
        published.store (pack ({ producerGeneration, 0 }), std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_seq_cst);
        didWork = true;
    }

    const auto consumerCounter = unpack (consumed.load (std::memory_order_acquire));
    const auto tail = consumerCounter.generation == producerGeneration ? consumerCounter.count : 0;
    const auto free = static_cast<std::int64_t> (ringFrames) - (producerHead - tail);
    // Past the limit, a looping stream carries on from frame 0 (chunks never straddle the wrap).
    const auto absolute = producerStart + producerHead;
    const bool wraps = absolute >= limit && loop != nullptr && loop->load (std::memory_order_relaxed);
    const auto sourceFrame = wraps ? absolute % limit : absolute;
    const auto remaining = absolute >= limit && ! wraps ? 0 : limit - sourceFrame;

    if (remaining <= 0 || free < std::min<std::int64_t> (chunkFrames, remaining))
        return didWork;

    const auto count = static_cast<int> (std::min<std::int64_t> ({ chunkFrames, free, remaining }));
    float* destinations[2] = { scratch[0].data(), scratch[1].data() };

    if (! source->read (sourceFrame, destinations, count))
        readFailures.fetch_add (1, std::memory_order_relaxed);

    // A slow read may have been overtaken by a seek: drop the chunk, the next call restarts.
    if (seekTarget.load().generation != producerGeneration)
        return true;

    const auto slot = static_cast<int> (producerHead % ringFrames);
    const auto firstPart = std::min (count, ringFrames - slot);

    for (int channel = 0; channel < 2; ++channel)
    {
        std::memcpy (
            ring[channel].data() + slot, scratch[channel].data(), sizeof (float) * static_cast<size_t> (firstPart));

        if (firstPart < count)
            std::memcpy (ring[channel].data(),
                         scratch[channel].data() + firstPart,
                         sizeof (float) * static_cast<size_t> (count - firstPart));
    }

    producerHead += count;
    published.store (pack ({ producerGeneration, producerHead }), std::memory_order_release);
    return true;
}

int PrefetchStream::available (std::uint32_t generation) noexcept
{
    if (generation != consumerGeneration)
    {
        consumerGeneration = generation;
        consumerTail = 0;
        consumed.store (pack ({ generation, 0 }), std::memory_order_release);
    }

    const auto head = unpack (published.load (std::memory_order_acquire));
    return head.generation == generation ? static_cast<int> (head.count - consumerTail) : 0;
}

bool PrefetchStream::read (std::uint32_t generation, float* const* destination, int numFrames) noexcept
{
    if (numFrames > available (generation))
        return false;

    const auto slot = static_cast<int> (consumerTail % ringFrames);
    const auto firstPart = std::min (numFrames, ringFrames - slot);

    for (int channel = 0; channel < 2; ++channel)
    {
        std::memcpy (
            destination[channel], ring[channel].data() + slot, sizeof (float) * static_cast<size_t> (firstPart));

        if (firstPart < numFrames)
            std::memcpy (destination[channel] + firstPart,
                         ring[channel].data(),
                         sizeof (float) * static_cast<size_t> (numFrames - firstPart));
    }

    // Seqlock check: if the producer moved to a newer generation meanwhile, it may have
    // overwritten what was just copied.
    std::atomic_thread_fence (std::memory_order_seq_cst);

    if (unpack (published.load (std::memory_order_relaxed)).generation != generation)
        return false;

    consumerTail += numFrames;
    consumed.store (pack ({ generation, consumerTail }), std::memory_order_release);
    return true;
}

int PrefetchStream::peekAvailable (std::uint32_t generation) const noexcept
{
    const auto head = unpack (published.load (std::memory_order_acquire));

    if (head.generation != generation)
        return 0;

    const auto tail = unpack (consumed.load (std::memory_order_acquire));
    return static_cast<int> (head.count - (tail.generation == generation ? tail.count : 0));
}

bool PrefetchStream::hasReachedLimit (std::uint32_t generation) const noexcept
{
    const auto head = unpack (published.load (std::memory_order_acquire));
    return head.generation == generation && publishedStart.load (std::memory_order_relaxed) + head.count >= limit;
}

//==============================================================================
PrefetchThread::PrefetchThread (juce::String name)
    : juce::Thread (std::move (name))
{
}

PrefetchThread::~PrefetchThread()
{
    stop();
}

void PrefetchThread::add (PrefetchStream& stream)
{
    streams.push_back (&stream);
}

void PrefetchThread::start()
{
    startThread (juce::Thread::Priority::high);
}

void PrefetchThread::stop()
{
    signalThreadShouldExit();
    wake.signal();
    stopThread (5000);
}

void PrefetchThread::run()
{
    while (! threadShouldExit())
    {
        bool didWork = false;

        for (auto* stream : streams)
            didWork = stream->produce() || didWork;

        if (! didWork)
            wake.wait (5);
    }
}
} // namespace snds::audio
