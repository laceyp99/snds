#include "dsp/ParameterStore.h"

namespace snds::dsp
{
namespace
{
constexpr int maxReadAttempts = 4;
}

ParameterStore::ParameterStore() noexcept
{
    for (int i = 0; i < numParameters; ++i)
        values[static_cast<std::size_t> (i)].store (parameterSpec (i).defaultValue, std::memory_order_relaxed);
}

void ParameterStore::beginWrite() noexcept
{
    sequence.store (sequence.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);
}

void ParameterStore::endWrite() noexcept
{
    sequence.store (sequence.load (std::memory_order_relaxed) + 1, std::memory_order_release);
}

bool ParameterStore::set (int index, float value)
{
    const std::pair<int, float> change { index, value };
    const auto before = version();
    setMany (std::span<const std::pair<int, float>> (&change, 1));
    return version() != before;
}

void ParameterStore::setMany (std::span<const std::pair<int, float>> changes)
{
    const std::lock_guard lock (writerMutex);

    bool anyChange = false;
    for (const auto& [index, value] : changes)
        anyChange |= values[static_cast<std::size_t> (index)].load (std::memory_order_relaxed)
                  != clampParameterValue (index, value);

    if (! anyChange)
        return;

    beginWrite();
    for (const auto& [index, value] : changes)
        values[static_cast<std::size_t> (index)].store (clampParameterValue (index, value), std::memory_order_relaxed);
    endWrite();
}

void ParameterStore::resetToDefaults()
{
    const std::lock_guard lock (writerMutex);

    bool anyChange = false;
    for (int i = 0; i < numParameters; ++i)
        anyChange |=
            values[static_cast<std::size_t> (i)].load (std::memory_order_relaxed) != parameterSpec (i).defaultValue;

    if (! anyChange)
        return;

    beginWrite();
    for (int i = 0; i < numParameters; ++i)
        values[static_cast<std::size_t> (i)].store (parameterSpec (i).defaultValue, std::memory_order_relaxed);
    endWrite();
}

bool ParameterStore::resetToDefault (int index)
{
    return set (index, parameterSpec (index).defaultValue);
}

float ParameterStore::get (int index) const noexcept
{
    return values[static_cast<std::size_t> (index)].load (std::memory_order_relaxed);
}

ParameterStore::ReadResult ParameterStore::read (ParameterSnapshot& snapshot) const noexcept
{
    for (int attempt = 0; attempt < maxReadAttempts; ++attempt)
    {
        const auto before = sequence.load (std::memory_order_acquire);

        if ((before & 1u) != 0)
            continue; // a write is in progress

        if ((before >> 1) == snapshot.version)
            return ReadResult::unchanged;

        std::array<float, numParameters> copy;
        for (std::size_t i = 0; i < copy.size(); ++i)
            copy[i] = values[i].load (std::memory_order_relaxed);

        std::atomic_thread_fence (std::memory_order_acquire);

        if (sequence.load (std::memory_order_relaxed) == before)
        {
            snapshot.values = copy;
            snapshot.version = before >> 1;
            return ReadResult::updated;
        }
    }

    return ReadResult::contended;
}
} // namespace snds::dsp
