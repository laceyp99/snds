#include "audio/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace snds::audio
{
namespace
{
constexpr int zeroCrossings = 40;    // sinc lobes kept on each side, in filter-scaled units
constexpr int tableResolution = 512; // table points per zero crossing
constexpr double kaiserBeta = 9.0;
// Where the sinc cutoff sits relative to the lower Nyquist: the Kaiser transition band (about
// 0.07 of the sampling rate at this length) then ends at Nyquist.
constexpr double cutoffMargin = 0.929;

double besselI0 (double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double quarter = x * x / 4.0;

    for (int k = 1; k < 60; ++k)
    {
        term *= quarter / (static_cast<double> (k) * k);
        sum += term;

        if (term < sum * 1.0e-17)
            break;
    }

    return sum;
}
} // namespace

void Resampler::prepare (double sourceRate, double destinationRate, int maxPushFrames)
{
    jassert (sourceRate > 0.0 && destinationRate > 0.0 && maxPushFrames > 0);
    maxPush = maxPushFrames;
    ratio = sourceRate / destinationRate;
    bypass = sourceRate == destinationRate;

    if (bypass)
    {
        halfWidth = 0;
        cutoff = 1.0;
        table.clear();
        weights.clear();
    }
    else
    {
        cutoff = cutoffMargin * std::min (1.0, 1.0 / ratio);
        halfWidth = static_cast<int> (std::ceil (zeroCrossings / cutoff));

        const auto points = zeroCrossings * tableResolution;
        table.assign (static_cast<size_t> (points + 2), 0.0f);
        const auto normaliser = besselI0 (kaiserBeta);

        for (int i = 0; i < points; ++i)
        {
            const auto x = static_cast<double> (i) / tableResolution;
            const auto sinc =
                i == 0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
            const auto r = x / zeroCrossings;
            table[static_cast<size_t> (i)] =
                static_cast<float> (sinc * besselI0 (kaiserBeta * std::sqrt (1.0 - r * r)) / normaliser);
        }

        weights.assign (static_cast<size_t> (2 * halfWidth), 0.0f);
    }

    const auto capacity = static_cast<size_t> (2 * maxPush + 2 * halfWidth + static_cast<int> (std::ceil (ratio)) + 16);

    for (auto& channel : buffer)
        channel.assign (capacity, 0.0f);

    reset();
}

void Resampler::reset()
{
    for (auto& channel : buffer)
        std::fill (channel.begin(), channel.end(), 0.0f);

    filled = halfWidth; // the filter sees silence before the start
    position = halfWidth;
    bypassTime = 0.0;
    discarded = 0.0;
}

void Resampler::compact() noexcept
{
    if (bypass)
        return;

    const auto start = static_cast<int> (std::floor (position)) - halfWidth + 1;

    if (start <= 0)
        return;

    const auto keep = std::max (0, filled - start);

    for (auto& channel : buffer)
        std::memmove (channel.data(), channel.data() + start, sizeof (float) * static_cast<size_t> (keep));

    filled = keep;
    position -= start;
    discarded += start;
}

void Resampler::push (const float* const* source, int numFrames) noexcept
{
    jassert (numFrames <= maxPush);

    if (filled + numFrames > static_cast<int> (buffer[0].size()))
        compact();

    numFrames = std::min (numFrames, static_cast<int> (buffer[0].size()) - filled);

    for (int channel = 0; channel < channelCount; ++channel)
        std::memcpy (
            buffer[channel].data() + filled, source[channel], sizeof (float) * static_cast<size_t> (numFrames));

    filled += numFrames;
}

int Resampler::pull (float* const* destination, int maxFrames) noexcept
{
    if (bypass)
    {
        const auto count = std::min (filled, maxFrames);

        for (int channel = 0; channel < channelCount; ++channel)
        {
            std::memcpy (destination[channel], buffer[channel].data(), sizeof (float) * static_cast<size_t> (count));
            std::memmove (buffer[channel].data(),
                          buffer[channel].data() + count,
                          sizeof (float) * static_cast<size_t> (filled - count));
        }

        filled -= count;
        bypassTime += count;
        return count;
    }

    int produced = 0;
    const auto width = 2 * halfWidth;

    while (produced < maxFrames)
    {
        const auto centre = static_cast<int> (std::floor (position));

        if (centre + halfWidth >= filled)
            break;

        const auto fraction = static_cast<float> (position - centre);
        const auto first = centre - halfWidth + 1;
        float sum = 0.0f;

        for (int j = 0; j < width; ++j)
        {
            const auto x =
                std::fabs ((fraction - static_cast<float> (j - halfWidth + 1)) * static_cast<float> (cutoff));
            float weight = 0.0f;

            if (x < static_cast<float> (zeroCrossings))
            {
                const auto scaled = x * static_cast<float> (tableResolution);
                const auto index = static_cast<int> (scaled);
                const auto low = table[static_cast<size_t> (index)];
                weight = low + (scaled - static_cast<float> (index)) * (table[static_cast<size_t> (index) + 1] - low);
            }

            weights[static_cast<size_t> (j)] = weight;
            sum += weight;
        }

        const auto gain = sum > 0.0f ? 1.0f / sum : 0.0f; // unity gain at DC for every phase

        for (int channel = 0; channel < channelCount; ++channel)
        {
            const float* samples = buffer[channel].data() + first;
            float accumulator = 0.0f;

            for (int j = 0; j < width; ++j)
                accumulator += samples[j] * weights[static_cast<size_t> (j)];

            destination[channel][produced] = accumulator * gain;
        }

        ++produced;
        position += ratio;
    }

    return produced;
}
} // namespace snds::audio
