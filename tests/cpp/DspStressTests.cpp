#include <juce_core/juce_core.h>

#include "DspHeavy.h"

#include "dsp/MixProcessor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

//==============================================================================
// Allocation counting. AudioCallbackAuditTests.cpp replaces the global operator new/delete for
// the whole test executable and counts, per thread, while `tracking` is set. This file reuses
// that hook (it must not define a second one) to prove that MixProcessor::process, reset and the
// meter read never allocate on the audio thread.
namespace snds::test::audit
{
extern thread_local bool tracking;
extern thread_local long long allocationCount;
} // namespace snds::test::audit

namespace
{
struct AllocationScope
{
    AllocationScope() noexcept
        : wasTracking (snds::test::audit::tracking),
          start (snds::test::audit::allocationCount)
    {
        snds::test::audit::tracking = true;
    }

    ~AllocationScope() { snds::test::audit::tracking = wasTracking; }

    long allocations() const noexcept { return static_cast<long> (snds::test::audit::allocationCount - start); }

    bool wasTracking;
    long long start;
};
} // namespace

namespace snds::dsp
{
namespace
{
constexpr int maxBlock = 256;

// Four tonal stems (drums, bass, other, vocals), generated block by block without allocating.
struct Tones
{
    explicit Tones (double sampleRate, float level)
        : rate (sampleRate),
          amplitude (level)
    {
    }

    void fill (std::array<std::array<std::vector<float>, 2>, numStems>& stems, int frames) noexcept
    {
        for (std::size_t s = 0; s < stems.size(); ++s)
            for (int i = 0; i < frames; ++i)
            {
                const auto phase = static_cast<float> (std::sin (2.0 * std::numbers::pi * hertz[s] * position[s]));
                position[s] += 1.0 / rate;
                stems[s][0][static_cast<std::size_t> (i)] = amplitude * phase;
                stems[s][1][static_cast<std::size_t> (i)] = amplitude * 0.9f * phase;
            }
    }

    double rate;
    float amplitude;
    std::array<double, numStems> hertz { 110.0, 55.0, 220.0, 440.0 };
    std::array<double, numStems> position {};
};

// The "message thread": sweeps every parameter as fast as it can, with extremes, resets and
// multi-parameter transactions.
class Sweeper
{
public:
    explicit Sweeper (ParameterStore& target, unsigned seed)
        : store (target),
          random (static_cast<juce::int64> (seed)),
          thread ([this] { run(); })
    {
    }

    ~Sweeper()
    {
        stop = true;
        thread.join();
    }

    long writes() const { return count.load(); }

private:
    float pickValue (int index)
    {
        switch (random.nextInt (4))
        {
            case 0:
                return fromNormalised (index, 0.0f);
            case 1:
                return fromNormalised (index, 1.0f);
            default:
                return fromNormalised (index, random.nextFloat());
        }
    }

    void run()
    {
        while (! stop.load())
        {
            const auto roll = random.nextInt (400);

            if (roll == 0)
            {
                store.resetToDefaults();
            }
            else if (roll < 8)
            {
                std::pair<int, float> changes[4];
                for (std::size_t s = 0; s < 4; ++s)
                {
                    const auto index = parameterIndex (static_cast<Stem> (s), StemControl::gain);
                    changes[s] = { index, pickValue (index) };
                }
                store.setMany (changes);
            }
            else
            {
                const auto index = random.nextInt (numParameters);
                store.set (index, pickValue (index));
            }

            ++count;
            std::this_thread::yield();
        }
    }

    ParameterStore& store;
    juce::Random random;
    std::atomic<bool> stop { false };
    std::atomic<long> count { 0 };
    std::thread thread;
};

class DspStressTests final : public juce::UnitTest
{
public:
    DspStressTests()
        : juce::UnitTest ("DSP parameter stress", "snds")
    {
    }

    void runTest() override
    {
        testAllocationHook();
        testSweepWhilePlaying();
        testLoudSweepWhilePlaying();
        testEveryParameterExtremes();
    }

private:
    struct Result
    {
        long blocks = 0;
        long resets = 0;
        long allocations = 0;
        long nonFinite = 0;
        long overCeiling = 0;
        long badMeters = 0;
        long badPan = 0;
        long limiterBusy = 0; // blocks where the limiter acted
        float peak = 0.0f;
        float worstStep = 0.0f;
    };

    void testAllocationHook()
    {
        beginTest ("The allocation counter sees allocations on the counting thread only");

        long counted = 0;
        {
            const AllocationScope scope;
            auto* p = new int (3);
            counted = scope.allocations();
            delete p;
        }
        expect (counted >= 1);

        // prepare() is allowed to allocate; the counter must notice it does.
        ParameterStore store;
        MixProcessor mix (store);
        const AllocationScope scope;
        mix.prepare (96000.0, 1024); // bigger than the constructor's, so it has to allocate
        expect (scope.allocations() > 0);
    }

    // Plays `seconds` of tones through a processor while the sweeper thread writes parameters. With
    // `minWrites` it keeps playing past `seconds` (for at most 20 s) until the sweeper has written that many
    // parameters, so a busy machine that starves the sweeper thread cannot make the check vacuous.
    Result play (double rate, double seconds, float level, bool sweep, bool noise, unsigned seed, long minWrites = 0)
    {
        Result result;
        ParameterStore store;
        MixProcessor mix (store);
        mix.prepare (rate, maxBlock);

        std::array<std::array<std::vector<float>, 2>, numStems> input;
        for (auto& stem : input)
            for (auto& channel : stem)
                channel.assign (maxBlock, 0.0f);
        std::array<std::vector<float>, 2> output;
        for (auto& channel : output)
            channel.assign (maxBlock, 0.0f);

        Tones tones (rate, level);
        juce::Random random (static_cast<juce::int64> (seed));
        std::unique_ptr<Sweeper> sweeper;
        if (sweep)
            sweeper = std::make_unique<Sweeper> (store, seed + 100);

        const auto totalFrames = static_cast<long> (rate * seconds);
        long done = 0;
        float last = 0.0f;
        long skipSteps = static_cast<long> (rate * 0.01); // a reset restarts filters and tails: not a click
        MeterSnapshot meters;

        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 20000.0;
        const auto sweeperBehind = [&]
        {
            return sweeper != nullptr && sweeper->writes() < minWrites
                && juce::Time::getMillisecondCounterHiRes() < deadline;
        };

        while (done < totalFrames || sweeperBehind())
        {
            const auto frames = static_cast<int> (
                done < totalFrames ? std::min<long> (1 + random.nextInt (maxBlock), totalFrames - done)
                                   : 1 + random.nextInt (maxBlock));
            tones.fill (input, frames);

            if (noise)
                for (auto& stem : input)
                    for (auto& channel : stem)
                        for (int i = 0; i < frames; ++i)
                            channel[static_cast<std::size_t> (i)] = random.nextFloat() * 2.0f - 1.0f;

            std::array<const float* const*, numStems> stems {};
            std::array<std::array<const float*, 2>, numStems> channelPointers {};
            for (std::size_t s = 0; s < input.size(); ++s)
            {
                channelPointers[s] = { input[s][0].data(), input[s][1].data() };
                stems[s] = channelPointers[s].data();
            }
            std::array<float*, 2> out { output[0].data(), output[1].data() };

            {
                const AllocationScope scope;

                // A "seek" now and then: the engine calls reset() between blocks, mid-sweep.
                if (random.nextInt (80) == 0)
                {
                    mix.reset();
                    ++result.resets;
                    skipSteps = static_cast<long> (rate * 0.01);
                }

                mix.process (stems, out.data(), frames);
                const auto readable = mix.meters().read (meters);
                const auto pan = mix.getPanPosition (random.nextInt (numStems));
                result.allocations += scope.allocations();
                result.badPan += ! (std::abs (pan) <= 1.0f);
                result.badMeters += readable && ! meterIsSane (meters);
                result.limiterBusy += readable && meters.limiterActive();
            }

            for (int c = 0; c < 2; ++c)
            {
                for (int i = 0; i < frames; ++i)
                {
                    const auto v = output[static_cast<std::size_t> (c)][static_cast<std::size_t> (i)];
                    result.nonFinite += ! std::isfinite (v);
                    result.overCeiling += ! (std::abs (v) <= limiterCeiling);
                    result.peak = std::max (result.peak, std::abs (v));

                    if (c == 0)
                    {
                        if (skipSteps <= 0)
                            result.worstStep = std::max (result.worstStep, std::abs (v - last));
                        last = v;
                        --skipSteps;
                    }
                }
            }

            done += frames;
            ++result.blocks;
        }

        sweeperWrites = sweeper != nullptr ? sweeper->writes() : 0;
        return result;
    }

    static bool meterIsSane (const MeterSnapshot& m)
    {
        auto ok = [] (float v) { return std::isfinite (v) && v >= 0.0f; };
        bool sane = ok (m.masterPeak) && ok (m.masterRms) && ok (m.limiterInputPeak) && ok (m.limiterReductionDb);
        for (const auto& stem : m.stems)
            sane = sane && ok (stem.peak) && ok (stem.rms);
        return sane;
    }

    void testSweepWhilePlaying()
    {
        beginTest ("Sweeping every parameter at full speed while playing and seeking: finite, bounded, no allocation");

        for (const auto rate : test::ratesFor ({ 48000.0, 96000.0 }, { 44100.0, 48000.0, 96000.0, 192000.0 }))
        {
            // Quiet tones keep the limiter out of it, so every step in the output is the mix's own.
            const auto level = 0.02f;
            const auto r = play (rate, test::workload (2.0, 4.0), level, true, false, 11, 1500);

            // A tone of up to 440 Hz moves by at most 2 pi f / rate of its peak per sample; the
            // smoothing ramps (>= 10 ms) add at most ~30 / (0.01 rate) of the peak (fader, gate, pan and master ramps
            // can all be running at once).
            const auto slope = static_cast<float> (2.0 * std::numbers::pi * 440.0 / rate);
            const auto ramps = 30.0f / (0.010f * static_cast<float> (rate));
            // The sweeper thread races the audio thread, so how many ramps overlap varies from run to run and
            // a loaded machine stacks more of them: 50 % headroom. An unramped jump (a mute, a pan, a filter
            // switch with no smoothing) steps by a large part of the peak, about ten times this bound.
            const auto bound = 1.5f * r.peak * (slope + ramps);

            logMessage ("rate " + juce::String (rate) + ": " + juce::String (r.blocks) + " blocks, "
                        + juce::String (r.resets) + " resets, " + juce::String (sweeperWrites) + " writes, peak "
                        + juce::String (r.peak) + ", worst step " + juce::String (r.worstStep) + " vs bound "
                        + juce::String (bound) + ", limiter busy " + juce::String (r.limiterBusy));

            expect (sweeperWrites > 1000);
            expectEquals (r.nonFinite, 0L);
            expectEquals (r.overCeiling, 0L);
            expectEquals (r.allocations, 0L);
            expectEquals (r.badMeters, 0L);
            expectEquals (r.badPan, 0L);
            expectEquals (r.limiterBusy, 0L); // the premise of the step bound
            expect (r.peak > 0.01f);
            expect (r.worstStep <= bound);
        }
    }

    void testLoudSweepWhilePlaying()
    {
        beginTest ("Full-scale noise with every parameter sweeping: never above the ceiling, never non-finite");

        for (const auto rate : test::ratesFor ({ 44100.0 }, { 44100.0, 96000.0 }))
        {
            const auto r = play (rate, test::workload (2.0, 4.0), 1.0f, true, true, 23);
            logMessage ("rate " + juce::String (rate) + ": peak " + juce::String (r.peak) + ", limiter busy in "
                        + juce::String (r.limiterBusy) + " of " + juce::String (r.blocks) + " blocks");
            expectEquals (r.nonFinite, 0L);
            expectEquals (r.overCeiling, 0L);
            expectEquals (r.allocations, 0L);
            expectEquals (r.badMeters, 0L);
            expect (r.limiterBusy > 0);
        }
    }

    void testEveryParameterExtremes()
    {
        beginTest ("Each parameter slammed between its extremes, one at a time, stays finite and click-free");

        const auto rate = 48000.0;
        float worstRatio = 0.0f;
        juce::String worstId;

        for (int index = 0; index < numParameters; ++index)
        {
            ParameterStore store;
            MixProcessor mix (store);
            mix.prepare (rate, maxBlock);

            std::array<std::array<std::vector<float>, 2>, numStems> input;
            for (auto& stem : input)
                for (auto& channel : stem)
                    channel.assign (maxBlock, 0.0f);
            std::array<std::vector<float>, 2> output;
            for (auto& channel : output)
                channel.assign (maxBlock, 0.0f);

            Tones tones (rate, 0.02f);
            const auto& spec = parameterSpec (index);

            // Sends and returns need something to send: give the effects a signal to work on.
            for (int s = 0; s < numStems; ++s)
            {
                store.set (static_cast<Stem> (s), StemControl::reverbSend, 0.5f);
                store.set (static_cast<Stem> (s), StemControl::delaySend, 0.5f);
            }
            mix.reset();

            float last = 0.0f;
            float worst = 0.0f;
            float peak = 0.0f;
            bool first = true;
            bool finite = true;
            long allocations = 0;

            for (int block = 0; block < test::workload (80, 400); ++block)
            {
                // Every 40 blocks (~0.2 s) slam to the other end of the range.
                if (block % 40 == 0)
                    store.set (index, ((block / 40) % 2 == 0) ? spec.maximum : spec.minimum);

                tones.fill (input, maxBlock);
                std::array<std::array<const float*, 2>, numStems> channelPointers {};
                std::array<const float* const*, numStems> stems {};
                for (std::size_t s = 0; s < input.size(); ++s)
                {
                    channelPointers[s] = { input[s][0].data(), input[s][1].data() };
                    stems[s] = channelPointers[s].data();
                }
                std::array<float*, 2> out { output[0].data(), output[1].data() };

                {
                    const AllocationScope scope;
                    mix.process (stems, out.data(), maxBlock);
                    allocations += scope.allocations();
                }

                for (int i = 0; i < maxBlock; ++i)
                {
                    const auto v = output[0][static_cast<std::size_t> (i)];
                    finite = finite && std::isfinite (v) && std::isfinite (output[1][static_cast<std::size_t> (i)]);
                    peak = std::max (peak, std::abs (v));
                    if (! first)
                        worst = std::max (worst, std::abs (v - last));
                    last = v;
                    first = false;
                }
            }

            const auto id = juce::String (std::string (spec.id.view()));
            expect (finite, id + " produced a non-finite sample");
            expectEquals (allocations, 0L);

            // The loudest the mix gets here is far below the ceiling, so any limiter action would be a bug.
            expect (peak < limiterCeiling, id + " reached the limiter");

            const auto slope = static_cast<float> (2.0 * std::numbers::pi * 440.0 / rate);
            const auto ramps = 30.0f / (0.010f * static_cast<float> (rate));
            const auto ratio = worst / (peak * (slope + ramps));
            if (ratio > worstRatio)
            {
                worstRatio = ratio;
                worstId = id;
            }
            expect (ratio <= 1.0f, id + ": step " + juce::String (worst) + " against peak " + juce::String (peak));
        }

        logMessage ("worst step ratio over all parameters: " + juce::String (worstRatio) + " (" + worstId + ")");
    }

    long sweeperWrites = 0;
};

DspStressTests dspStressTests;
} // namespace
} // namespace snds::dsp
