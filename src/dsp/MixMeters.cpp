#include "dsp/MixMeters.h"

#include <algorithm>
#include <cmath>

namespace snds::dsp
{
namespace
{
constexpr int maxReadAttempts = 8;
constexpr float flushBelow = 1.0e-9f; // keeps decaying meters out of denormals
} // namespace

void LevelFollower::update (const float* const* channels, int numFrames) noexcept
{
    float blockPeak = 0.0f;
    double sumSquares = 0.0;

    for (int c = 0; c < 2; ++c)
    {
        const float* x = channels[c];

        for (int i = 0; i < numFrames; ++i)
        {
            blockPeak = std::max (blockPeak, std::abs (x[i]));
            sumSquares += static_cast<double> (x[i]) * static_cast<double> (x[i]);
        }
    }

    const auto blockMeanSquare = static_cast<float> (sumSquares / (2.0 * std::max (1, numFrames)));
    const auto blend = static_cast<float> (1.0 - std::exp (-numFrames / (meterRmsSeconds * rate)));
    meanSquare += (blockMeanSquare - meanSquare) * blend;

    if (meanSquare < flushBelow * flushBelow)
        meanSquare = 0.0f;

    updatePeak (blockPeak, numFrames);
}

void LevelFollower::updatePeak (float blockPeak, int numFrames) noexcept
{
    const auto decay = static_cast<float> (std::exp (-numFrames / (meterPeakReleaseSeconds * rate)));
    peakValue = std::max (blockPeak, peakValue * decay);

    if (peakValue < flushBelow)
        peakValue = 0.0f;
}

float LevelFollower::rms() const noexcept
{
    return std::sqrt (meanSquare);
}

float reductionToDecibels (float gain) noexcept
{
    return gain >= 1.0f ? 0.0f : -20.0f * std::log10 (std::max (gain, 1.0e-6f));
}

//==============================================================================
MeterStore::MeterStore() noexcept
{
    for (auto& f : fields)
        f.store (0.0f, std::memory_order_relaxed);
}

void MeterStore::publish (const MeterSnapshot& snapshot) noexcept
{
    const auto start = sequence.load (std::memory_order_relaxed) + 1; // odd: write in progress
    sequence.store (start, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);

    std::size_t i = 0;
    for (const auto& stem : snapshot.stems)
    {
        fields[i++].store (stem.peak, std::memory_order_relaxed);
        fields[i++].store (stem.rms, std::memory_order_relaxed);
    }
    fields[i++].store (snapshot.masterPeak, std::memory_order_relaxed);
    fields[i++].store (snapshot.masterRms, std::memory_order_relaxed);
    fields[i++].store (snapshot.limiterInputPeak, std::memory_order_relaxed);
    fields[i++].store (snapshot.limiterReductionDb, std::memory_order_relaxed);

    sequence.store (start + 1, std::memory_order_release);
}

bool MeterStore::read (MeterSnapshot& out) const noexcept
{
    for (int attempt = 0; attempt < maxReadAttempts; ++attempt)
    {
        const auto before = sequence.load (std::memory_order_acquire);

        if ((before & 1u) != 0)
            continue;

        std::array<float, fieldCount> copy;
        for (std::size_t i = 0; i < fieldCount; ++i)
            copy[i] = fields[i].load (std::memory_order_relaxed);

        std::atomic_thread_fence (std::memory_order_acquire);

        if (sequence.load (std::memory_order_relaxed) != before)
            continue;

        std::size_t i = 0;
        for (auto& stem : out.stems)
        {
            stem.peak = copy[i++];
            stem.rms = copy[i++];
        }
        out.masterPeak = copy[i++];
        out.masterRms = copy[i++];
        out.limiterInputPeak = copy[i++];
        out.limiterReductionDb = copy[i++];
        out.version = before >> 1;
        return true;
    }

    return false;
}
} // namespace snds::dsp
