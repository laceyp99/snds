#include "dsp/StemSends.h"

#include <algorithm>

namespace snds::dsp
{
void StemSends::prepare (double sampleRate, int maxBlockFrames)
{
    maxFrames = std::max (1, maxBlockFrames);
    storage.assign (static_cast<std::size_t> (maxFrames) * 4, 0.0f);

    const auto size = static_cast<std::size_t> (maxFrames);
    reverbPointers = { storage.data(), storage.data() + size };
    delayPointers = { storage.data() + 2 * size, storage.data() + 3 * size };

    const auto seconds =
        static_cast<double> (parameterSpec (parameterIndex (Stem::drums, StemControl::reverbSend)).smoothingSeconds);

    for (auto& g : reverbGain)
    {
        g.reset (sampleRate, seconds);
        g.setCurrentAndTargetValue (0.0f);
    }

    for (auto& g : delayGain)
    {
        g.reset (sampleRate, seconds);
        g.setCurrentAndTargetValue (0.0f);
    }
}

void StemSends::setTargets (const ParameterSnapshot& snapshot) noexcept
{
    for (int i = 0; i < numStems; ++i)
    {
        const auto stem = static_cast<Stem> (i);
        reverbGain[static_cast<std::size_t> (i)].setTargetValue (snapshot.get (stem, StemControl::reverbSend)
                                                                 * sendBusPad);
        delayGain[static_cast<std::size_t> (i)].setTargetValue (snapshot.get (stem, StemControl::delaySend)
                                                                * sendBusPad);
    }
}

void StemSends::reset() noexcept
{
    for (auto& g : reverbGain)
        g.setCurrentAndTargetValue (g.getTargetValue());

    for (auto& g : delayGain)
        g.setCurrentAndTargetValue (g.getTargetValue());
}

void StemSends::beginBlock (int numFrames) noexcept
{
    const auto n = static_cast<std::size_t> (std::min (numFrames, maxFrames));

    for (auto* channel : { reverbPointers[0], reverbPointers[1], delayPointers[0], delayPointers[1] })
        std::fill_n (channel, n, 0.0f);
}

void StemSends::mixInto (Smoother& gain, const float* const* signal, float* const* bus, int numFrames) noexcept
{
    if (! gain.isSmoothing())
    {
        const auto level = gain.getTargetValue();

        if (level == 0.0f)
            return;

        for (int i = 0; i < numFrames; ++i)
        {
            bus[0][i] += level * signal[0][i];
            bus[1][i] += level * signal[1][i];
        }
        return;
    }

    for (int i = 0; i < numFrames; ++i)
    {
        const auto level = gain.getNextValue();
        bus[0][i] += level * signal[0][i];
        bus[1][i] += level * signal[1][i];
    }
}

void StemSends::addStem (int stem, const float* const* signal, int numFrames) noexcept
{
    const auto n = std::min (numFrames, maxFrames);
    mixInto (reverbGain[static_cast<std::size_t> (stem)], signal, reverbPointers.data(), n);
    mixInto (delayGain[static_cast<std::size_t> (stem)], signal, delayPointers.data(), n);
}
} // namespace snds::dsp
