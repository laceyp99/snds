#pragma once

#include "dsp/MixParameters.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>

namespace snds::dsp
{
/** A consistent copy of every mix parameter, owned by the reader (the audio thread). */
struct ParameterSnapshot
{
    ParameterSnapshot() noexcept
    {
        for (int i = 0; i < numParameters; ++i)
            values[static_cast<std::size_t> (i)] = parameterSpec (i).defaultValue;
    }

    float operator[] (int index) const noexcept { return values[static_cast<std::size_t> (index)]; }

    float get (Stem stem, StemControl control) const noexcept { return (*this)[parameterIndex (stem, control)]; }
    float get (GlobalControl control) const noexcept { return (*this)[parameterIndex (control)]; }

    std::array<float, numParameters> values {};

    /** The store version this copy was taken at (0 = defaults, nothing written yet). */
    std::uint32_t version = 0;
};

/** Lock-free parameter hand-off from the message thread to the audio thread.

    Writers (message thread; serialised by a mutex the reader never touches) publish whole
    transactions through a seqlock: the reader either gets every value of one transaction or
    none of a half-written one. The reader never blocks or allocates: if it collides with a
    writer it retries a few times and otherwise keeps its previous snapshot until the next block.
    Values are stored as relaxed atomic floats, so there is no data race even mid-write.
*/
class ParameterStore
{
public:
    enum class ReadResult
    {
        unchanged, // nothing new since the snapshot's version
        updated,   // snapshot now holds a newer consistent state
        contended  // a write was in progress; snapshot left untouched, try next block
    };

    ParameterStore() noexcept;

    //==========================================================================
    // Writer side (any non-real-time thread).

    /** Clamps and stores one value. Returns false if it was already that value. */
    bool set (int index, float value);
    bool set (Stem stem, StemControl control, float value) { return set (parameterIndex (stem, control), value); }
    bool set (GlobalControl control, float value) { return set (parameterIndex (control), value); }

    /** Applies several values as one transaction, so the reader sees all of them or none. */
    void setMany (std::span<const std::pair<int, float>> changes);

    /** Restores every parameter to its default as one transaction. */
    void resetToDefaults();

    /** Restores a single parameter to its default. */
    bool resetToDefault (int index);

    /** The latest written value (writer side; not a consistency guarantee across parameters). */
    float get (int index) const noexcept;

    //==========================================================================
    // Reader side (the audio thread). Real-time safe: no locks, no allocation.

    ReadResult read (ParameterSnapshot& snapshot) const noexcept;

    /** Incremented by every transaction that changed something. */
    std::uint32_t version() const noexcept { return sequence.load (std::memory_order_acquire) >> 1; }

private:
    void beginWrite() noexcept;
    void endWrite() noexcept;

    std::array<std::atomic<float>, numParameters> values;
    std::atomic<std::uint32_t> sequence { 0 }; // odd while a write is in progress
    std::mutex writerMutex;
};
} // namespace snds::dsp
