#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>

namespace snds::audio
{
/** The one shared seek command for a loaded song.

    A generation (24 bits, wrapping) and a source-rate frame position (40 bits) live in a single
    atomic word, so the audio thread and the prefetch thread both see a coherent
    (generation, position) pair. Every stream reads this same word: a seek cannot reach some
    streams and miss others.
*/
class SeekTarget
{
public:
    struct Value
    {
        std::uint32_t generation = 0;
        std::int64_t frame = 0;

        bool operator== (const Value&) const = default;
    };

    static constexpr std::int64_t maxFrame = (std::int64_t { 1 } << 40) - 1;

    explicit SeekTarget (std::int64_t startFrame = 0) noexcept { word.store (pack ({ 0, startFrame })); }

    Value load() const noexcept { return unpack (word.load (std::memory_order_acquire)); }

    /** Publishes a new position under a new generation. Safe from several threads (the control
        thread, or the audio thread once a seek's fade-out has finished). */
    Value publish (std::int64_t frame) noexcept
    {
        auto current = word.load (std::memory_order_acquire);

        for (;;)
        {
            const auto previous = unpack (current);
            const Value next { (previous.generation + 1) & generationMask, frame };

            if (word.compare_exchange_weak (current, pack (next), std::memory_order_release, std::memory_order_acquire))
                return next;
        }
    }

private:
    static constexpr std::uint32_t generationMask = 0xFFFFFF;

    static std::uint64_t pack (Value value) noexcept
    {
        const auto frame = static_cast<std::uint64_t> (juce::jlimit<std::int64_t> (0, maxFrame, value.frame));
        return (static_cast<std::uint64_t> (value.generation & generationMask) << 40) | frame;
    }

    static Value unpack (std::uint64_t packed) noexcept
    {
        return { static_cast<std::uint32_t> (packed >> 40),
                 static_cast<std::int64_t> (packed & static_cast<std::uint64_t> (maxFrame)) };
    }

    std::atomic<std::uint64_t> word { 0 };
};
} // namespace snds::audio
