#include <juce_core/juce_core.h>

#include "dsp/MasterStage.h"
#include "dsp/MixMeters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>
#include <vector>

namespace snds::dsp
{
namespace
{
constexpr double sampleRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

struct Stereo
{
    std::vector<float> left, right;

    explicit Stereo (int frames, float l = 0.0f, float r = 0.0f)
        : left (static_cast<std::size_t> (frames), l),
          right (static_cast<std::size_t> (frames), r)
    {
    }

    int size() const { return static_cast<int> (left.size()); }
};

Stereo makeSine (double rate, double hertz, double seconds, float amplitude)
{
    Stereo s (static_cast<int> (rate * seconds));
    for (int i = 0; i < s.size(); ++i)
        s.left[static_cast<std::size_t> (i)] = s.right[static_cast<std::size_t> (i)] =
            amplitude * static_cast<float> (std::sin (2.0 * std::numbers::pi * hertz * i / rate));
    return s;
}

Stereo makeNoise (int frames, unsigned seed, float amplitude)
{
    juce::Random random (static_cast<juce::int64> (seed));
    Stereo s (frames);
    for (int i = 0; i < frames; ++i)
    {
        s.left[static_cast<std::size_t> (i)] = amplitude * (random.nextFloat() * 2.0f - 1.0f);
        s.right[static_cast<std::size_t> (i)] = amplitude * (random.nextFloat() * 2.0f - 1.0f);
    }
    return s;
}

float peakOf (const std::vector<float>& v)
{
    float peak = 0.0f;
    for (const auto x : v)
        peak = std::max (peak, std::abs (x));
    return peak;
}

// Runs the master stage over a stereo signal in blocks, returning the worst-case block info.
MasterStage::BlockInfo runMaster (MasterStage& stage, const Stereo& in, Stereo& out, int block = 256)
{
    MasterStage::BlockInfo total;
    for (int start = 0; start < in.size(); start += block)
    {
        const auto n = std::min (block, in.size() - start);
        std::array<const float*, 2> src { in.left.data() + start, in.right.data() + start };
        std::array<float*, 2> dst { out.left.data() + start, out.right.data() + start };
        const auto info = stage.process (src.data(), dst.data(), n);
        total.inputPeak = std::max (total.inputPeak, info.inputPeak);
        total.minGain = std::min (total.minGain, info.minGain);
    }
    return total;
}

class DspMasterTests final : public juce::UnitTest
{
public:
    DspMasterTests()
        : juce::UnitTest ("DSP master and meters", "snds")
    {
    }

    void runTest() override
    {
        testLimiter();
        testMasterGain();
        testBallistics();
        testMeterStore();
    }

private:
    void testLimiter()
    {
        beginTest ("The limiter is transparent at or below the ceiling: bit-exact, no latency");

        for (const auto rate : sampleRates)
        {
            MasterStage stage;
            stage.prepare (rate);
            auto in = makeNoise (20000, 1, 0.96f);
            in.left[0] = limiterCeiling; // exactly at the ceiling
            in.right[0] = -limiterCeiling;
            Stereo out (in.size());
            const auto info = runMaster (stage, in, out);
            expect (out.left == in.left && out.right == in.right);
            expectEquals (info.minGain, 1.0f);
        }

        beginTest ("Overs are held at the ceiling, never above it, with the reduction reported");

        for (const auto rate : sampleRates)
        {
            MasterStage stage;
            stage.prepare (rate);
            auto in = makeSine (rate, 220.0, 1.0, 4.0f); // +12 dBFS
            Stereo out (in.size());
            const auto info = runMaster (stage, in, out);

            expect (peakOf (out.left) <= limiterCeiling && peakOf (out.right) <= limiterCeiling);
            expect (peakOf (out.left) > limiterCeiling * 0.99f); // it limits, it does not squash
            expectWithinAbsoluteError (info.inputPeak, 4.0f, 1.0e-3f);
            expectWithinAbsoluteError (info.minGain, limiterCeiling / 4.0f, 1.0e-3f);
            expect (reductionToDecibels (info.minGain) > 11.0f);
        }

        beginTest ("The limiter is stereo-linked: the image does not shift");

        {
            MasterStage stage;
            stage.prepare (48000.0);
            Stereo in (4000, 3.0f, 0.3f);
            Stereo out (4000);
            runMaster (stage, in, out);
            expectWithinAbsoluteError (out.right.back() / out.left.back(), 0.1f, 1.0e-3f);
        }

        beginTest ("After an over, the gain recovers smoothly to exactly 1 and the signal returns bit-exact");

        for (const auto rate : sampleRates)
        {
            MasterStage stage;
            stage.prepare (rate);

            auto loud = makeNoise (static_cast<int> (rate * 0.05), 3, 4.0f);
            Stereo outLoud (loud.size());
            runMaster (stage, loud, outLoud);
            expect (stage.getLimiterGain() < 0.5f);

            // A quiet signal afterwards: the output is the input times a rising gain, never above
            // the ceiling, and never rising faster than the release allows.
            auto quiet = makeNoise (static_cast<int> (rate * 2.0), 4, 0.5f);
            Stereo out (quiet.size());
            float previous = stage.getLimiterGain();
            bool monotone = true;
            int recoveredAt = -1;

            for (int start = 0; start < quiet.size(); start += 64)
            {
                std::array<const float*, 2> src { quiet.left.data() + start, quiet.right.data() + start };
                std::array<float*, 2> dst { out.left.data() + start, out.right.data() + start };
                const auto n = std::min (64, quiet.size() - start);
                stage.process (src.data(), dst.data(), n);
                monotone = monotone && stage.getLimiterGain() >= previous;
                previous = stage.getLimiterGain();

                if (recoveredAt < 0 && stage.getLimiterGain() == 1.0f)
                    recoveredAt = start + n;
            }

            expect (monotone);
            expect (peakOf (out.left) <= limiterCeiling);
            expect (recoveredAt > 0 && recoveredAt < static_cast<int> (rate * 1.6));

            // From the first sample after recovery on, the output is the input, exactly.
            if (recoveredAt > 0)
                expect (
                    std::equal (quiet.left.begin() + recoveredAt, quiet.left.end(), out.left.begin() + recoveredAt));
        }

        beginTest ("Non-finite input becomes silence, not a limiter lock-up");

        {
            MasterStage stage;
            stage.prepare (48000.0);
            Stereo in (1000, 0.5f, 0.5f);
            in.left[10] = std::numeric_limits<float>::quiet_NaN();
            in.right[20] = std::numeric_limits<float>::infinity();
            Stereo out (1000);
            runMaster (stage, in, out);
            expectEquals (out.left[10], 0.0f);
            expectEquals (out.right[20], 0.0f);
            expectEquals (out.left[500], 0.5f);
            expectEquals (stage.getLimiterGain(), 1.0f);
        }

        beginTest ("Limiter output does not depend on the block size");

        {
            auto in = makeNoise (30000, 8, 2.5f);
            Stereo whole (30000), pieces (30000);
            MasterStage a, b;
            a.prepare (44100.0);
            b.prepare (44100.0);
            runMaster (a, in, whole, 30000);
            runMaster (b, in, pieces, 1);
            expect (whole.left == pieces.left && whole.right == pieces.right);
        }
    }

    void testMasterGain()
    {
        beginTest ("Master gain: unity default, +6 dB, -inf, and a click-free ramp");

        for (const auto rate : sampleRates)
        {
            MasterStage stage;
            stage.prepare (rate);

            Stereo in (static_cast<int> (rate * 0.2), 0.4f, 0.4f);
            Stereo out (in.size());
            runMaster (stage, in, out);
            expectEquals (out.left.back(), 0.4f); // unity is exact

            stage.setGainDb (6.0f);
            runMaster (stage, in, out);
            expectWithinAbsoluteError (out.left.back(), 0.4f * 1.99526f, 1.0e-5f);

            float worst = 0.0f;
            float last = out.left.back();
            stage.setGainDb (0.0f);
            runMaster (stage, in, out);
            // (that ramp ran from +6 dB down to 0 dB over 20 ms)
            for (const auto v : out.left)
            {
                worst = std::max (worst, std::abs (v - last));
                last = v;
            }
            expect (worst <= 0.4f * 1.3f / (0.020f * static_cast<float> (rate)));

            stage.setGainDb (-60.0f);
            runMaster (stage, in, out);
            expectEquals (out.left.back(), 0.0f);
            expectEquals (out.right.back(), 0.0f);

            stage.setGainDb (100.0f); // clamped to +6 dB
            runMaster (stage, in, out);
            expectWithinAbsoluteError (out.left.back(), 0.4f * 1.99526f, 1.0e-5f);
        }

        beginTest ("The limiter comes after master gain: +6 dB on a hot signal is limited, not clipped");

        MasterStage stage;
        stage.prepare (48000.0);
        stage.setGainDb (6.0f);
        stage.reset();
        auto in = makeSine (48000.0, 440.0, 0.5, 0.9f);
        Stereo out (in.size());
        const auto info = runMaster (stage, in, out);
        expect (peakOf (out.left) <= limiterCeiling);
        expect (info.inputPeak > 1.7f);
        expect (info.minGain < 0.6f);
    }

    //==========================================================================
    static float followBlocks (LevelFollower& follower, const Stereo& signal, int block)
    {
        for (int start = 0; start < signal.size(); start += block)
        {
            const auto n = std::min (block, signal.size() - start);
            std::array<const float*, 2> ptr { signal.left.data() + start, signal.right.data() + start };
            follower.update (ptr.data(), n);
        }
        return follower.peak();
    }

    void testBallistics()
    {
        beginTest ("Peak meter: instant attack, 300 ms exponential release");

        for (const auto block : { 32, 256 })
        {
            const auto rate = 48000.0;
            LevelFollower follower;
            follower.prepare (rate);

            followBlocks (follower, makeSine (rate, 1000.0, 0.05, 0.8f), block);
            expectWithinAbsoluteError (follower.peak(), 0.8f, 0.01f); // attack within the first blocks

            const Stereo silence (static_cast<int> (rate * 0.3));
            for (int pass = 1; pass <= 4; ++pass) // 0.3, 0.6, 0.9, 1.2 s of silence
            {
                const auto before = follower.peak();
                followBlocks (follower, silence, block);
                expectWithinAbsoluteError (
                    follower.peak(), before * static_cast<float> (std::exp (-1.0)), before * 0.01f);
            }
        }

        beginTest ("Peak meter: a quiet block never pulls the reading down faster than the release");

        {
            LevelFollower follower;
            follower.prepare (48000.0);
            followBlocks (follower, makeSine (48000.0, 440.0, 0.05, 1.0f), 256);
            const auto loud = follower.peak();
            followBlocks (follower, Stereo (256, 0.01f, 0.01f), 256);
            expect (follower.peak() > loud * 0.97f);
        }

        beginTest ("RMS meter: 300 ms smoothing, converges to the true RMS, block-size independent");

        for (const auto rate : sampleRates)
        {
            const auto sine = makeSine (rate, 1000.0, 2.5, 0.5f);
            const auto trueRms = 0.5f / std::sqrt (2.0f);

            LevelFollower a, b;
            a.prepare (rate);
            b.prepare (rate);

            // After one time constant a first-order average is 63.2 % of the way (in power).
            const auto oneTau = Stereo (static_cast<int> (rate * 0.3));
            LevelFollower early;
            early.prepare (rate);
            Stereo tauSine (oneTau.size());
            std::copy (sine.left.begin(), sine.left.begin() + oneTau.size(), tauSine.left.begin());
            std::copy (sine.right.begin(), sine.right.begin() + oneTau.size(), tauSine.right.begin());
            followBlocks (early, tauSine, 256);
            expectWithinAbsoluteError (early.rms(), trueRms * std::sqrt (1.0f - std::exp (-1.0f)), trueRms * 0.03f);

            followBlocks (a, sine, 64);
            followBlocks (b, sine, 256);
            expectWithinAbsoluteError (a.rms(), trueRms, trueRms * 0.01f);
            expectWithinAbsoluteError (b.rms(), trueRms, trueRms * 0.01f);
            expectWithinAbsoluteError (a.rms(), b.rms(), trueRms * 0.005f);
        }

        beginTest ("Meters settle to exactly zero in silence");

        {
            LevelFollower follower;
            follower.prepare (48000.0);
            followBlocks (follower, makeNoise (4800, 5, 1.0f), 256);
            followBlocks (follower, Stereo (48000 * 20), 256);
            expectEquals (follower.peak(), 0.0f);
            expectEquals (follower.rms(), 0.0f);
        }

        beginTest ("Gain reduction is reported in positive decibels");

        expectEquals (reductionToDecibels (1.0f), 0.0f);
        expectEquals (reductionToDecibels (1.5f), 0.0f);
        expectWithinAbsoluteError (reductionToDecibels (0.5f), 6.0206f, 1.0e-3f);
        expect (std::isfinite (reductionToDecibels (0.0f)));
    }

    void testMeterStore()
    {
        beginTest ("Meter snapshots are consistent while the audio thread publishes");

        MeterStore store;
        std::atomic<bool> stop { false };
        std::atomic<long> published { 0 };

        std::thread writer (
            [&]
            {
                float n = 0.0f;
                while (! stop.load())
                {
                    MeterSnapshot s;
                    n += 1.0f;
                    for (auto& stem : s.stems)
                        stem = { n, n };
                    s.masterPeak = s.masterRms = s.limiterInputPeak = s.limiterReductionDb = n;
                    store.publish (s);
                    ++published;
                }
            });

        MeterSnapshot snapshot;
        long reads = 0, torn = 0, failed = 0, backwards = 0;
        std::uint32_t lastVersion = 0;
        const auto end = juce::Time::getMillisecondCounter() + 300;

        while (juce::Time::getMillisecondCounter() < end)
        {
            if (! store.read (snapshot))
            {
                ++failed;
                continue;
            }

            ++reads;
            const auto n = snapshot.masterPeak;
            bool same = snapshot.masterRms == n && snapshot.limiterInputPeak == n && snapshot.limiterReductionDb == n;
            for (const auto& stem : snapshot.stems)
                same = same && stem.peak == n && stem.rms == n;
            torn += ! same;
            backwards += snapshot.version < lastVersion;
            lastVersion = snapshot.version;
        }

        stop = true;
        writer.join();
        logMessage ("reads " + juce::String (reads) + ", failed " + juce::String (failed) + ", published "
                    + juce::String (published.load()));
        expect (reads > 0 && published.load() > 0);
        expectEquals (torn, 0L);
        expectEquals (backwards, 0L);

        beginTest ("Limiter activity flag follows the reduction threshold");

        MeterSnapshot s;
        expect (! s.limiterActive());
        s.limiterReductionDb = 0.05f;
        expect (! s.limiterActive());
        s.limiterReductionDb = 0.1f;
        expect (s.limiterActive());
    }
};

DspMasterTests dspMasterTests;
} // namespace
} // namespace snds::dsp
