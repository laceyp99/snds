#include <juce_core/juce_core.h>

#include "DspHeavy.h"

#include "dsp/StemFilter.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace snds::dsp
{
namespace
{
std::vector<double> sampleRates()
{
    return test::ratesFor ({ 44100.0, 96000.0 }, { 44100.0, 48000.0, 96000.0, 192000.0 });
}
constexpr int blockSize = 256; // the engine's largest block

struct Stereo
{
    std::vector<float> left, right;

    explicit Stereo (int frames)
        : left (static_cast<std::size_t> (frames), 0.0f),
          right (static_cast<std::size_t> (frames), 0.0f)
    {
    }

    int size() const { return static_cast<int> (left.size()); }
};

// Runs a whole buffer through the filter in engine-sized blocks, in place.
void run (StemFilter& filter, Stereo& signal, int block = blockSize)
{
    for (int start = 0; start < signal.size(); start += block)
    {
        const auto n = std::min (block, signal.size() - start);
        std::array<const float*, 2> in { signal.left.data() + start, signal.right.data() + start };
        std::array<float*, 2> out { signal.left.data() + start, signal.right.data() + start };
        filter.process (in.data(), out.data(), n);
    }
}

Stereo makeSine (double rate, double hertz, double seconds, float amplitude = 0.5f)
{
    Stereo signal (static_cast<int> (rate * seconds));
    for (int i = 0; i < signal.size(); ++i)
        signal.left[static_cast<std::size_t> (i)] = signal.right[static_cast<std::size_t> (i)] =
            amplitude * static_cast<float> (std::sin (2.0 * std::numbers::pi * hertz * i / rate));
    return signal;
}

Stereo makeNoise (int frames, unsigned seed)
{
    juce::Random random (static_cast<juce::int64> (seed));
    Stereo signal (frames);
    for (int i = 0; i < frames; ++i)
        signal.left[static_cast<std::size_t> (i)] = signal.right[static_cast<std::size_t> (i)] =
            random.nextFloat() * 2.0f - 1.0f;
    return signal;
}

// Amplitude of the sine at `hertz` in the last `window` seconds of a channel (lock-in).
double amplitudeAt (const std::vector<float>& channel, double rate, double hertz, double window)
{
    const auto count = static_cast<int> (rate * window);
    const auto first = static_cast<int> (channel.size()) - count;
    double s = 0.0, c = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const auto phase = 2.0 * std::numbers::pi * hertz * (first + i) / rate;
        s += static_cast<double> (channel[static_cast<std::size_t> (first + i)]) * std::sin (phase);
        c += static_cast<double> (channel[static_cast<std::size_t> (first + i)]) * std::cos (phase);
    }

    return 2.0 * std::hypot (s, c) / count;
}

// The analytic response of a prewarped 2nd-order Butterworth.
double expectedGainDb (bool highPass, double rate, double cutoff, double hertz)
{
    const auto gc = std::tan (std::numbers::pi * cutoff / rate);
    const auto gf = std::tan (std::numbers::pi * hertz / rate);
    const auto ratio = highPass ? gc / gf : gf / gc;
    return -10.0 * std::log10 (1.0 + std::pow (ratio, 4.0));
}

class DspStemFilterTests final : public juce::UnitTest
{
public:
    DspStemFilterTests()
        : juce::UnitTest ("DSP stem filters", "snds")
    {
    }

    void runTest() override
    {
        testBypass();
        testResponse();
        testStability();
        testClickFree();
        testChannelsAndBlocks();
    }

private:
    static double measuredGainDb (double rate, bool highPass, double cutoff, double hertz)
    {
        StemFilter filter;
        filter.prepare (rate);
        filter.setTargets (highPass ? static_cast<float> (cutoff) : 20.0f,
                           highPass ? 20000.0f : static_cast<float> (cutoff));

        auto signal = makeSine (rate, hertz, 1.8);
        run (filter, signal);
        return 20.0 * std::log10 (amplitudeAt (signal.left, rate, hertz, 1.0) / 0.5);
    }

    void testBypass()
    {
        beginTest ("Defaults are bypassed: bit-exact output at every rate");

        for (const auto rate : sampleRates())
        {
            StemFilter filter;
            filter.prepare (rate);
            filter.setTargets (20.0f, 20000.0f);

            auto signal = makeNoise (5000, 3);
            const auto original = signal;
            run (filter, signal);
            expect (signal.left == original.left && signal.right == original.right);
        }

        beginTest ("Returning to the endpoints restores a bit-exact bypass");

        for (const auto rate : sampleRates())
        {
            StemFilter filter;
            filter.prepare (rate);
            filter.setTargets (300.0f, 3000.0f);
            auto settle = makeNoise (static_cast<int> (rate * 0.2), 5);
            run (filter, settle);

            filter.setTargets (20.0f, 20000.0f);
            auto ramp = makeNoise (static_cast<int> (rate * 0.2), 6);
            run (filter, ramp);

            auto signal = makeNoise (2000, 7);
            const auto original = signal;
            run (filter, signal);
            expect (signal.left == original.left && signal.right == original.right);
        }

        beginTest ("The endpoint values are the bypass; the next value inside is not");

        expect (isLowCutBypassed (parameterSpec (parameterIndex (Stem::drums, StemControl::lowCut)).defaultValue));
        expect (isHighCutBypassed (parameterSpec (parameterIndex (Stem::drums, StemControl::highCut)).defaultValue));

        CutFilter low (CutFilter::Mode::highPass);
        low.prepare (48000.0, 0.03f);
        expect (low.isBypassed());
        low.setCutoff (21.0f);
        expect (! low.isBypassed());
        low.setCutoff (20.0f);
        expect (! low.isBypassed()); // still ramping out
    }

    void testResponse()
    {
        beginTest ("Low cut: -3 dB at the cutoff and Butterworth slope, at every rate");

        for (const auto rate : sampleRates())
        {
            for (const auto cutoff : { 100.0, 250.0, 1000.0 })
            {
                const auto atCutoff = measuredGainDb (rate, true, cutoff, cutoff);
                expectWithinAbsoluteError (atCutoff, -3.01, 0.1);

                for (const auto f : { cutoff * 0.5, cutoff * 2.0, cutoff * 4.0 })
                    expectWithinAbsoluteError (
                        measuredGainDb (rate, true, cutoff, f), expectedGainDb (true, rate, cutoff, f), 0.15);
            }
        }

        beginTest ("High cut: -3 dB at the cutoff and Butterworth slope, at every rate");

        for (const auto rate : sampleRates())
        {
            for (const auto cutoff : { 500.0, 2000.0, 8000.0, 16000.0 })
            {
                const auto atCutoff = measuredGainDb (rate, false, cutoff, cutoff);
                expectWithinAbsoluteError (atCutoff, -3.01, 0.1);

                for (const auto f : { cutoff * 0.5, cutoff * 1.5 })
                    if (f < 0.45 * rate)
                        expectWithinAbsoluteError (
                            measuredGainDb (rate, false, cutoff, f), expectedGainDb (false, rate, cutoff, f), 0.15);
            }
        }

        beginTest ("Low and high cut together form a band; the passband is flat");

        for (const auto rate : sampleRates())
        {
            StemFilter filter;
            filter.prepare (rate);
            filter.setTargets (100.0f, 8000.0f);
            auto signal = makeSine (rate, 1000.0, 1.8);
            run (filter, signal);
            const auto gain = 20.0 * std::log10 (amplitudeAt (signal.left, rate, 1000.0, 1.0) / 0.5);
            const auto expected =
                expectedGainDb (true, rate, 100.0, 1000.0) + expectedGainDb (false, rate, 8000.0, 1000.0);
            expectWithinAbsoluteError (gain, expected, 0.1);
            expect (gain > -0.2);
        }
    }

    void testStability()
    {
        beginTest ("Stable under rapid, random and extreme cutoff modulation");

        for (const auto rate : sampleRates())
        {
            StemFilter filter;
            filter.prepare (rate);
            juce::Random random (42);
            auto noise = makeNoise (static_cast<int> (rate * 3.0), 9);
            float peak = 0.0f;
            bool finite = true;

            for (int start = 0; start < noise.size();)
            {
                // New random targets every 1..400 samples, including the extremes and the endpoints.
                const auto n = std::min (1 + random.nextInt (400), noise.size() - start);
                const auto pick = [&random] (float lo, float hi)
                {
                    switch (random.nextInt (4))
                    {
                        case 0:
                            return lo;
                        case 1:
                            return hi;
                        default:
                            return lo * std::pow (hi / lo, random.nextFloat());
                    }
                };
                filter.setTargets (pick (20.0f, 1000.0f), pick (500.0f, 20000.0f));

                std::array<const float*, 2> in { noise.left.data() + start, noise.right.data() + start };
                std::array<float*, 2> out { noise.left.data() + start, noise.right.data() + start };
                filter.process (in.data(), out.data(), n);

                for (int i = start; i < start + n; ++i)
                {
                    const auto v = noise.left[static_cast<std::size_t> (i)];
                    finite = finite && std::isfinite (v);
                    peak = std::max (peak, std::abs (v));
                }
                start += n;
            }

            logMessage ("rate " + juce::String (rate) + ": peak under modulation " + juce::String (peak));
            expect (finite);
            expect (peak < 4.0f);
        }

        beginTest ("A continuous sweep of both cuts, there and back, stays bounded");

        StemFilter filter;
        filter.prepare (48000.0);
        float peak = 0.0f;

        for (int pass = 0; pass < 6; ++pass)
        {
            for (int step = 0; step <= 100; ++step)
            {
                const auto t = static_cast<float> (pass % 2 == 0 ? step : 100 - step) / 100.0f;
                filter.setTargets (20.0f * std::pow (50.0f, t), 20000.0f * std::pow (0.025f, t));
                auto block = makeNoise (480, static_cast<unsigned> (step + pass * 101));
                run (filter, block);
                for (const auto v : block.left)
                {
                    expect (std::isfinite (v));
                    peak = std::max (peak, std::abs (v));
                }
            }
        }
        expect (peak < 4.0f);

        beginTest ("A non-finite input burst does not poison the filter");

        filter.setTargets (200.0f, 4000.0f);
        auto bad = makeNoise (256, 1);
        bad.left[10] = std::numeric_limits<float>::quiet_NaN();
        run (filter, bad);
        auto good = makeNoise (4096, 2);
        run (filter, good);
        bool allFinite = true;
        for (const auto v : good.left)
            allFinite = allFinite && std::isfinite (v);
        expect (allFinite);
    }

    static float maxStep (const std::vector<float>& channel)
    {
        float worst = 0.0f;
        for (std::size_t i = 1; i < channel.size(); ++i)
            worst = std::max (worst, std::abs (channel[i] - channel[i - 1]));
        return worst;
    }

    void testClickFree()
    {
        beginTest ("Engaging and releasing a cut does not click");

        constexpr double clickRate = 48000.0;

        for (const auto hertz : { 200.0, 5000.0 })
        {
            const auto original = makeSine (clickRate, hertz, 0.6, 0.8f);
            const auto inputStep = maxStep (original.left);

            for (const auto engage :
                 { std::pair { 1000.0f, 20000.0f }, std::pair { 20.0f, 500.0f }, std::pair { 150.0f, 2000.0f } })
            {
                StemFilter filter;
                filter.prepare (clickRate);
                auto signal = original;

                // 0.1 s untouched, engage, then release at 0.35 s.
                const int engageAt = 4800;
                const int releaseAt = 16800;
                Stereo first (engageAt), second (releaseAt - engageAt), third (signal.size() - releaseAt);
                std::copy (signal.left.begin(), signal.left.begin() + engageAt, first.left.begin());
                std::copy (signal.right.begin(), signal.right.begin() + engageAt, first.right.begin());
                std::copy (signal.left.begin() + engageAt, signal.left.begin() + releaseAt, second.left.begin());
                std::copy (signal.right.begin() + engageAt, signal.right.begin() + releaseAt, second.right.begin());
                std::copy (signal.left.begin() + releaseAt, signal.left.end(), third.left.begin());
                std::copy (signal.right.begin() + releaseAt, signal.right.end(), third.right.begin());

                run (filter, first);
                filter.setTargets (engage.first, engage.second);
                run (filter, second);
                filter.setTargets (20.0f, 20000.0f);
                run (filter, third);

                std::vector<float> joined;
                for (const auto* part : { &first, &second, &third })
                    joined.insert (joined.end(), part->left.begin(), part->left.end());

                const auto step = maxStep (joined);
                logMessage ("sine " + juce::String (hertz) + " Hz, engage " + juce::String (engage.first) + "/"
                            + juce::String (engage.second) + ": step " + juce::String (step) + " vs input "
                            + juce::String (inputStep));
                expect (step <= inputStep * 1.25f + 0.02f);
            }
        }

        beginTest ("Engaging on a DC level fades smoothly");

        for (const auto rate : sampleRates())
        {
            for (const auto engage : { std::pair { 1000.0f, 20000.0f }, std::pair { 20.0f, 500.0f } })
            {
                StemFilter filter;
                filter.prepare (rate);
                filter.setTargets (engage.first, engage.second);
                Stereo dc (static_cast<int> (rate * 0.3));
                std::fill (dc.left.begin(), dc.left.end(), 1.0f);
                std::fill (dc.right.begin(), dc.right.end(), 1.0f);
                run (filter, dc);
                expect (maxStep (dc.left) <= 0.03f);
            }
        }
    }

    void testChannelsAndBlocks()
    {
        beginTest ("Left and right are processed identically");

        auto sweep = [] (StemFilter& filter, Stereo& signal)
        {
            for (int start = 0; start < signal.size(); start += 100)
            {
                const auto t = static_cast<float> (start % 3000) / 3000.0f;
                filter.setTargets (20.0f + 900.0f * t, 20000.0f - 15000.0f * t);
                std::array<const float*, 2> in { signal.left.data() + start, signal.right.data() + start };
                std::array<float*, 2> out { signal.left.data() + start, signal.right.data() + start };
                filter.process (in.data(), out.data(), std::min (100, signal.size() - start));
            }
        };

        StemFilter both, leftOnly, rightOnly;
        for (auto* f : { &both, &leftOnly, &rightOnly })
            f->prepare (48000.0);

        auto stereo = makeNoise (30000, 77);
        Stereo left (30000), right (30000);
        left.left = stereo.left;
        right.right = stereo.right;

        sweep (both, stereo);
        sweep (leftOnly, left);
        sweep (rightOnly, right);

        expect (stereo.left == stereo.right); // identical input, identical output
        expect (left.left == stereo.left);    // the left channel alone behaves the same
        expect (right.right == stereo.right); // and so does the right
        expect (std::all_of (left.right.begin(), left.right.end(), [] (float v) { return v == 0.0f; }));
        expect (std::all_of (right.left.begin(), right.left.end(), [] (float v) { return v == 0.0f; }));

        beginTest ("Output does not depend on the block size");

        StemFilter a, b;
        a.prepare (44100.0);
        b.prepare (44100.0);
        a.setTargets (180.0f, 3500.0f);
        b.setTargets (180.0f, 3500.0f);
        auto whole = makeNoise (20000, 5);
        auto pieces = whole;
        run (a, whole, 20000);
        run (b, pieces, 1);
        expect (whole.left == pieces.left && whole.right == pieces.right);

        beginTest ("Reset clears the state and snaps to the targets");

        StemFilter filter;
        filter.prepare (48000.0);
        filter.setTargets (400.0f, 2000.0f);
        auto loud = makeNoise (4800, 8);
        run (filter, loud);
        filter.setTargets (20.0f, 20000.0f);
        filter.reset(); // no ramp: bypassed immediately
        auto signal = makeNoise (512, 12);
        const auto original = signal;
        run (filter, signal);
        expect (signal.left == original.left && signal.right == original.right);

        filter.setTargets (500.0f, 20000.0f);
        filter.reset();
        Stereo silence (4096);
        run (filter, silence);
        expect (std::all_of (silence.left.begin(), silence.left.end(), [] (float v) { return v == 0.0f; }));
    }
};

DspStemFilterTests dspStemFilterTests;
} // namespace
} // namespace snds::dsp
