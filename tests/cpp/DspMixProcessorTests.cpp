#include <juce_core/juce_core.h>

#include "DspHeavy.h"

#include "dsp/MixProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

namespace snds::dsp
{
namespace
{
constexpr double sampleRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

struct Stereo
{
    std::vector<float> left, right;

    explicit Stereo (int frames = 0, float l = 0.0f, float r = 0.0f)
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

float peakOf (const std::vector<float>& v,
              std::size_t from = 0,
              std::size_t to = std::numeric_limits<std::size_t>::max())
{
    float peak = 0.0f;
    for (auto i = from; i < std::min (to, v.size()); ++i)
        peak = std::max (peak, std::abs (v[i]));
    return peak;
}

bool allFinite (const std::vector<float>& v)
{
    return std::all_of (v.begin(), v.end(), [] (float x) { return std::isfinite (x); });
}

// A processor with four full-length input stems, driven in blocks.
struct Rig
{
    explicit Rig (double sampleRate, int maxBlock = 256)
        : rate (sampleRate),
          mix (store),
          maxBlockFrames (maxBlock)
    {
    }

    void start (int frames)
    {
        for (auto& stem : in)
            if (stem.size() == 0)
                stem = Stereo (frames); // unset stems are silent

        mix.prepare (rate, maxBlockFrames);
        out = Stereo (frames);
    }

    // Processes [from, to) in blocks of `block` frames (<= 0: random sizes 1..256).
    void run (int from, int to, int block = 256, juce::Random* random = nullptr)
    {
        for (int start = from; start < to;)
        {
            const auto want = block > 0 ? block : 1 + (random != nullptr ? random->nextInt (256) : 0);
            const auto n = std::min (want, to - start);
            std::array<std::array<const float*, 2>, numStems> channels;
            std::array<const float* const*, numStems> pointers {};

            for (std::size_t s = 0; s < channels.size(); ++s)
            {
                channels[s] = { in[s].left.data() + start, in[s].right.data() + start };
                pointers[s] = channels[s].data();
            }

            std::array<float*, 2> dst { out.left.data() + start, out.right.data() + start };
            mix.process (pointers, dst.data(), n);
            start += n;
        }
    }

    double rate;
    ParameterStore store;
    MixProcessor mix;
    int maxBlockFrames;
    std::array<Stereo, numStems> in;
    Stereo out;
};

class DspMixProcessorTests final : public juce::UnitTest
{
public:
    DspMixProcessorTests()
        : juce::UnitTest ("DSP mix processor", "snds")
    {
    }

    void runTest() override
    {
        testUnityDefaultNull();
        testBlockIndependence();
        testSoloMutePanMaster();
        testResetAndTap();
        testSendsAndEffects();
        testMeters();
        testMasterHeadroom();
    }

private:
    static void fillNoise (Rig& rig, int frames, float amplitude, unsigned seed = 1)
    {
        for (int s = 0; s < numStems; ++s)
            rig.in[static_cast<std::size_t> (s)] = makeNoise (frames, seed + static_cast<unsigned> (s), amplitude);
    }

    static void setAll (Rig& rig, StemControl control, float value)
    {
        for (int s = 0; s < numStems; ++s)
            rig.store.set (static_cast<Stem> (s), control, value);
    }

    void testUnityDefaultNull()
    {
        beginTest ("Unity default: the mix is the plain stem sum, bit for bit, with the limiter idle");

        for (const auto rate : test::ratesFor ({ 44100.0, 96000.0 }, { 44100.0, 48000.0, 96000.0, 192000.0 }))
        {
            for (const auto block : { 256, 1, 0 })
            {
                Rig rig (rate);
                const auto frames = static_cast<int> (rate * test::workload (0.25, 0.5));
                fillNoise (rig, frames, 0.2f); // four of them stay below the ceiling
                rig.start (frames);

                juce::Random random (3);
                rig.run (0, frames, block, &random);

                Stereo expected (frames);
                for (int i = 0; i < frames; ++i)
                {
                    const auto at = static_cast<std::size_t> (i);
                    expected.left[at] =
                        ((rig.in[0].left[at] + rig.in[1].left[at]) + rig.in[2].left[at]) + rig.in[3].left[at];
                    expected.right[at] =
                        ((rig.in[0].right[at] + rig.in[1].right[at]) + rig.in[2].right[at]) + rig.in[3].right[at];
                }

                expect (rig.out.left == expected.left && rig.out.right == expected.right);

                MeterSnapshot meters;
                expect (rig.mix.meters().read (meters));
                expectEquals (meters.limiterReductionDb, 0.0f);
                expect (! meters.limiterActive());
                for (const auto& stem : meters.stems)
                    expect (stem.peak > 0.15f && stem.peak <= 0.2f);
            }
        }

        beginTest ("Defaults really are the documented defaults");

        {
            Rig rig (48000.0);
            ParameterSnapshot defaults;
            for (int s = 0; s < numStems; ++s)
            {
                const auto stem = static_cast<Stem> (s);
                expectEquals (defaults.get (stem, StemControl::gain), 0.0f);
                expectEquals (defaults.get (stem, StemControl::pan), 0.0f);
                expectEquals (defaults.get (stem, StemControl::reverbSend), 0.0f);
                expectEquals (defaults.get (stem, StemControl::delaySend), 0.0f);
                expectEquals (defaults.get (stem, StemControl::mute), 0.0f);
                expectEquals (defaults.get (stem, StemControl::solo), 0.0f);
            }
            expectEquals (defaults.get (GlobalControl::masterGain), 0.0f);
        }
    }

    void testBlockIndependence()
    {
        beginTest ("A fully loaded mix is identical for every block size, and for one long call");

        for (const auto rate : test::ratesFor ({ 44100.0 }, { 44100.0, 96000.0 }))
        {
            Stereo reference;

            for (const auto block : { 256, 1, 77, 0 })
            {
                Rig rig (rate);
                const auto frames = static_cast<int> (rate * test::workload (0.4, 1.0));
                fillNoise (rig, frames, 0.3f, 10);
                for (int s = 0; s < numStems; ++s)
                {
                    const auto stem = static_cast<Stem> (s);
                    rig.store.set (stem, StemControl::gain, -3.0f * static_cast<float> (s));
                    rig.store.set (stem, StemControl::pan, -0.6f + 0.4f * static_cast<float> (s));
                    rig.store.set (stem, StemControl::lowCut, 80.0f + 50.0f * static_cast<float> (s));
                    rig.store.set (stem, StemControl::highCut, 9000.0f - 1000.0f * static_cast<float> (s));
                    rig.store.set (stem, StemControl::reverbSend, 0.3f);
                    rig.store.set (stem, StemControl::delaySend, 0.25f);
                }
                rig.store.set (GlobalControl::delayFeedback, 0.6f);
                rig.store.set (GlobalControl::tempoBpm, 133.0f);
                rig.store.set (GlobalControl::delayDivision, 2.0f);
                rig.store.set (GlobalControl::masterGain, -2.0f);
                rig.start (frames);

                juce::Random random (9);
                rig.run (0, frames, block, &random);

                if (reference.size() == 0)
                    reference = rig.out;
                else
                    expect (rig.out.left == reference.left && rig.out.right == reference.right);
            }
        }

        beginTest ("A call longer than the prepared block is processed in chunks, identically");

        {
            Rig whole (48000.0), chunks (48000.0);
            for (auto* rig : { &whole, &chunks })
            {
                fillNoise (*rig, 1000, 0.2f, 4);
                rig->store.set (Stem::bass, StemControl::reverbSend, 0.5f);
                rig->start (1000);
            }
            whole.run (0, 1000, 1000); // a single 1000-frame call, with maxBlock 256
            chunks.run (0, 1000, 256);
            expect (whole.out.left == chunks.out.left && whole.out.right == chunks.out.right);
        }
    }

    void testSoloMutePanMaster()
    {
        beginTest ("Solo, mute, pan and master gain shape the mix");

        const auto rate = 48000.0;
        const auto frames = static_cast<int> (rate * 0.4);
        const auto tail = static_cast<std::size_t> (rate * 0.3);

        {
            Rig rig (rate);
            fillNoise (rig, frames, 0.2f);
            rig.store.set (Stem::bass, StemControl::solo, 1.0f);
            rig.start (frames);
            rig.run (0, frames);
            expect (std::equal (rig.in[1].left.begin() + static_cast<std::ptrdiff_t> (tail),
                                rig.in[1].left.end(),
                                rig.out.left.begin() + static_cast<std::ptrdiff_t> (tail)));
        }

        {
            // Two solos and a mute on one of them: mute wins, the other soloed stem alone remains.
            Rig rig (rate);
            fillNoise (rig, frames, 0.2f);
            rig.store.set (Stem::drums, StemControl::solo, 1.0f);
            rig.store.set (Stem::vocals, StemControl::solo, 1.0f);
            rig.store.set (Stem::vocals, StemControl::mute, 1.0f);
            rig.start (frames);
            rig.run (0, frames);
            expect (std::equal (rig.in[0].left.begin() + static_cast<std::ptrdiff_t> (tail),
                                rig.in[0].left.end(),
                                rig.out.left.begin() + static_cast<std::ptrdiff_t> (tail)));
        }

        {
            // Changing solo mid-stream ramps: no step bigger than the gate slope allows.
            Rig rig (rate);
            for (auto& stem : rig.in)
                stem = Stereo (frames, 0.2f, 0.2f);
            rig.start (frames);
            rig.run (0, 2000);
            rig.store.set (Stem::other, StemControl::solo, 1.0f);
            rig.run (2000, frames);

            float worst = 0.0f;
            for (int i = 1; i < frames; ++i)
                worst = std::max (worst,
                                  std::abs (rig.out.left[static_cast<std::size_t> (i)]
                                            - rig.out.left[static_cast<std::size_t> (i - 1)]));
            expect (worst <= 3.3f * 0.2f / (0.010f * static_cast<float> (rate)));
            expectWithinAbsoluteError (rig.out.left.back(), 0.2f, 1.0e-6f);
        }

        {
            // Hard-left vocals alone: right channel silent, left at +3 dB (constant-power law).
            Rig rig (rate);
            fillNoise (rig, frames, 0.2f);
            rig.store.set (Stem::vocals, StemControl::solo, 1.0f);
            rig.store.set (Stem::vocals, StemControl::pan, -1.0f);
            rig.start (frames);
            rig.run (0, frames);
            expectEquals (peakOf (rig.out.right, tail), 0.0f);
            expectWithinAbsoluteError (rig.out.left.back(), rig.in[3].left.back() * std::sqrt (2.0f), 1.0e-5f);
            expectEquals (rig.mix.getPanPosition (3), -1.0f);
            expectEquals (rig.mix.getPanPosition (0), 0.0f);
        }

        {
            Rig rig (rate);
            fillNoise (rig, frames, 0.1f);
            rig.store.set (GlobalControl::masterGain, 6.0f);
            rig.start (frames);
            rig.run (0, frames);
            const auto sum =
                ((rig.in[0].left.back() + rig.in[1].left.back()) + rig.in[2].left.back()) + rig.in[3].left.back();
            expectWithinAbsoluteError (rig.out.left.back(), sum * 1.99526f, 2.0e-5f);

            rig.store.set (GlobalControl::masterGain, -60.0f); // -inf
            rig.run (0, frames);
            expectEquals (peakOf (rig.out.left, tail), 0.0f);
        }

        {
            // Everything muted: exact silence.
            Rig rig (rate);
            fillNoise (rig, frames, 0.2f);
            setAll (rig, StemControl::mute, 1.0f);
            rig.start (frames);
            rig.run (0, frames);
            expectEquals (peakOf (rig.out.left), 0.0f);
        }
    }

    void testResetAndTap()
    {
        beginTest ("Reset clears the delay and reverb tails and snaps to the controls without a ramp");

        {
            const auto rate = 48000.0;
            const auto frames = static_cast<int> (rate * 2.0);
            Rig rig (rate);
            fillNoise (rig, frames, 0.6f);
            setAll (rig, StemControl::reverbSend, 1.0f);
            setAll (rig, StemControl::delaySend, 1.0f);
            rig.store.set (GlobalControl::delayFeedback, maxFeedback);
            rig.start (frames);
            rig.run (0, frames);
            expect (peakOf (rig.out.left) > 0.5f);

            // Now silence in, reset first, and the master set to -inf: the very first block is silent.
            for (auto& stem : rig.in)
                stem = Stereo (frames);
            rig.store.set (GlobalControl::masterGain, -60.0f);
            rig.mix.reset();
            rig.run (0, 4096);
            expectEquals (peakOf (rig.out.left, 0, 4096), 0.0f); // (the rest of `out` is the first run)
            expectEquals (peakOf (rig.out.right, 0, 4096), 0.0f);

            // And with the master back up, the old tails are really gone.
            rig.store.set (GlobalControl::masterGain, 0.0f);
            rig.mix.reset();
            rig.run (0, frames);
            expectEquals (peakOf (rig.out.left), 0.0f);

            MeterSnapshot meters;
            expect (rig.mix.meters().read (meters));
            expectEquals (meters.masterPeak, 0.0f);
        }

        beginTest ("The per-stem tap is the post-filter, post-fader, post-pan signal");

        {
            const auto rate = 48000.0;
            const auto frames = static_cast<int> (rate * 0.4);
            Rig rig (rate);
            fillNoise (rig, frames, 0.15f);
            rig.store.set (Stem::bass, StemControl::gain, -6.0f);
            rig.store.set (Stem::bass, StemControl::pan, -1.0f);
            rig.store.set (Stem::other, StemControl::mute, 1.0f);
            rig.start (frames);

            // Nothing tapped before anything has run.
            expectEquals (rig.mix.lastFrameCount(), 0);

            rig.run (0, frames, 100);
            expectEquals (rig.mix.lastFrameCount(), 100);

            const auto tap = rig.mix.lastStemSignals();
            const auto gain = std::pow (10.0f, -6.0f / 20.0f) * std::sqrt (2.0f);
            const auto last = static_cast<std::size_t> (frames - 1);

            expectWithinAbsoluteError (tap[1][0][99], rig.in[1].left[last] * gain, 1.0e-5f);
            expectEquals (tap[1][1][99], 0.0f);
            expectEquals (tap[2][0][99], 0.0f);                 // muted
            expectEquals (tap[0][0][99], rig.in[0].left[last]); // untouched drums
            expectEquals (tap[3][1][99], rig.in[3].right[last]);

            // With no effects the output is the sum of the taps (they are what is mixed).
            expectEquals (rig.out.left[last], ((tap[0][0][99] + tap[1][0][99]) + tap[2][0][99]) + tap[3][0][99]);
        }
    }

    void testSendsAndEffects()
    {
        beginTest ("Sends feed the shared effects; zero sends leave the mix dry");

        const auto rate = 48000.0;
        const auto frames = static_cast<int> (rate * 1.0);

        auto impulse = [frames]
        {
            Stereo s (frames);
            s.left[0] = s.right[0] = 0.5f;
            return s;
        };

        {
            Rig dry (rate);
            dry.in[0] = impulse();
            dry.start (frames);
            dry.run (0, frames);
            expectEquals (dry.out.left[0], 0.5f);
            expectEquals (peakOf (dry.out.left, 1), 0.0f); // no reverb, no echo

            Rig wet (rate);
            wet.in[0] = impulse();
            wet.store.set (Stem::drums, StemControl::reverbSend, 1.0f);
            wet.start (frames);
            wet.run (0, frames);
            expect (peakOf (wet.out.left, 100) > 0.001f);
            expect (allFinite (wet.out.left));
        }

        {
            // 120 BPM, 1/8: the echo lands at 0.25 s with the impulse's level; no feedback.
            Rig echo (rate);
            echo.in[0] = impulse();
            echo.store.set (Stem::drums, StemControl::delaySend, 1.0f);
            echo.store.set (GlobalControl::delayFeedback, 0.0f);
            echo.start (frames);
            echo.run (0, frames);
            expectEquals (echo.out.left[0], 0.5f);
            expectEquals (echo.out.left[12000], 0.5f * sendBusPad); // the send is padded by -14 dB
            expectEquals (peakOf (echo.out.left, 12001), 0.0f);

            // The send follows the fader and the mute (post-fader): a muted stem sends nothing.
            Rig muted (rate);
            muted.in[0] = impulse();
            muted.store.set (Stem::drums, StemControl::delaySend, 1.0f);
            muted.store.set (Stem::drums, StemControl::mute, 1.0f);
            muted.store.set (GlobalControl::delayFeedback, 0.0f);
            muted.start (frames);
            muted.run (0, frames);
            expectEquals (peakOf (muted.out.left), 0.0f);

            // Returns scale the effect.
            Rig quiet (rate);
            quiet.in[0] = impulse();
            quiet.store.set (Stem::drums, StemControl::delaySend, 1.0f);
            quiet.store.set (GlobalControl::delayFeedback, 0.0f);
            quiet.store.set (GlobalControl::delayReturn, 0.5f);
            quiet.start (frames);
            quiet.run (0, frames);
            expectEquals (quiet.out.left[12000], 0.25f * sendBusPad);
        }
    }

    void testMeters()
    {
        beginTest ("Stem meters read post-fader levels; master and limiter meters follow the output");

        const auto rate = 48000.0;
        const auto frames = static_cast<int> (rate * 2.5);
        Rig rig (rate);
        rig.in[0] = makeSine (rate, 1000.0, 2.5, 0.5f);
        rig.in[1] = makeSine (rate, 220.0, 2.5, 0.5f);
        rig.store.set (Stem::drums, StemControl::gain, -6.0f);
        rig.store.set (Stem::bass, StemControl::gain, -60.0f); // -inf: silent post-fader
        rig.start (frames);
        rig.run (0, frames);

        MeterSnapshot meters;
        expect (rig.mix.meters().read (meters));
        const auto drumGain = std::pow (10.0f, -6.0f / 20.0f);
        expectWithinAbsoluteError (meters.stems[0].peak, 0.5f * drumGain, 0.005f);
        expectWithinAbsoluteError (meters.stems[0].rms, 0.5f * drumGain / std::sqrt (2.0f), 0.003f);
        expectEquals (meters.stems[1].peak, 0.0f); // the fader is before the meter
        expectEquals (meters.stems[1].rms, 0.0f);
        expectEquals (meters.stems[2].peak, 0.0f);
        expectWithinAbsoluteError (meters.masterPeak, 0.5f * drumGain, 0.005f);
        expect (meters.version > 0);
        expect (! meters.limiterActive());

        // Muting the stem drops its meter: it releases from where it was.
        rig.store.set (Stem::drums, StemControl::mute, 1.0f);
        rig.in[0] = Stereo (frames);
        rig.run (0, frames);
        expect (rig.mix.meters().read (meters));
        expect (meters.stems[0].peak < 1.0e-3f); // 2.5 s of release from 0.25
    }

    //==========================================================================
    struct Headroom
    {
        float outputPeak = 0.0f;
        float inputPeak = 0.0f;
        float maxReductionDb = 0.0f;
        bool finite = true;
        bool everActive = false;
    };

    static Headroom measure (Rig& rig, int frames)
    {
        Headroom result;
        constexpr int block = 256;

        for (int start = 0; start < frames; start += block)
        {
            const auto end = std::min (frames, start + block);
            rig.run (start, end, block);

            MeterSnapshot meters;
            if (rig.mix.meters().read (meters))
            {
                result.inputPeak = std::max (result.inputPeak, meters.limiterInputPeak);
                result.maxReductionDb = std::max (result.maxReductionDb, meters.limiterReductionDb);
                result.everActive = result.everActive || meters.limiterActive();
            }
        }

        result.outputPeak = std::max (peakOf (rig.out.left), peakOf (rig.out.right));
        result.finite = allFinite (rig.out.left) && allFinite (rig.out.right);
        return result;
    }

    void testMasterHeadroom()
    {
        beginTest ("Headroom: four full-scale stems at +6 dB with the master up stay under the ceiling");

        for (const auto rate : test::ratesFor ({ 44100.0 }, { 44100.0, 96000.0 }))
        {
            Rig rig (rate);
            const auto frames = static_cast<int> (rate * 2.0);
            fillNoise (rig, frames, 1.0f, 20);
            setAll (rig, StemControl::gain, 6.0f);
            rig.store.set (GlobalControl::masterGain, 6.0f);
            rig.start (frames);

            const auto h = measure (rig, frames);
            logMessage ("rate " + juce::String (rate) + ": limiter input peak " + juce::String (h.inputPeak)
                        + ", output peak " + juce::String (h.outputPeak) + ", max reduction "
                        + juce::String (h.maxReductionDb) + " dB");
            expect (h.finite);
            expect (h.outputPeak <= limiterCeiling);
            expect (h.inputPeak > 8.0f); // the sum really did go far over full scale
            expect (h.everActive);       // and the limiter says so
            expect (h.maxReductionDb > 10.0f);
        }

        beginTest ("Headroom budget: all sends, returns and feedback at maximum add only ~2 dB to a full-scale mix");

        for (const auto rate : test::ratesFor ({ 44100.0 }, { 44100.0, 96000.0 }))
        {
            for (const auto fast : { true, false })
            {
                const auto frames = static_cast<int> (rate * test::workload (4.0, 10.0));

                auto configure = [&] (Rig& rig, bool effects)
                {
                    fillNoise (rig, frames, 1.0f, 40);
                    if (effects)
                    {
                        setAll (rig, StemControl::delaySend, 1.0f);
                        setAll (rig, StemControl::reverbSend, 1.0f);
                    }
                    rig.store.set (GlobalControl::tempoBpm, fast ? 400.0f : 120.0f);
                    rig.store.set (GlobalControl::delayDivision,
                                   static_cast<float> (fast ? DelayDivision::sixteenth : DelayDivision::eighth));
                    rig.store.set (GlobalControl::delayFeedback, maxFeedback);
                    rig.store.set (GlobalControl::delayReturn, 1.0f);
                    rig.store.set (GlobalControl::reverbReturn, 1.0f);
                    rig.start (frames);
                };

                Rig dry (rate), wet (rate);
                configure (dry, false);
                configure (wet, true);
                const auto dryResult = measure (dry, frames);
                const auto wetResult = measure (wet, frames);

                logMessage (
                    "rate " + juce::String (rate) + (fast ? " 400 BPM 1/16" : " 120 BPM 1/8") + ": dry peak "
                    + juce::String (dryResult.inputPeak) + ", with effects " + juce::String (wetResult.inputPeak) + " ("
                    + juce::String (20.0 * std::log10 (wetResult.inputPeak / dryResult.inputPeak), 1) + " dB more)");
                expect (wetResult.finite && wetResult.outputPeak <= limiterCeiling);
                expect (wetResult.inputPeak > dryResult.inputPeak);         // the effects really are contributing
                expect (wetResult.inputPeak <= dryResult.inputPeak * 1.3f); // at most ~2.3 dB over the dry sum
                expect (wetResult.inputPeak <= 5.2f); // about +14 dBFS in total (the dry sum alone is +12)
            }
        }

        beginTest ("Headroom budget: the delay's resonant worst case stays within +20 dB and is limited");

        for (const auto rate : test::ratesFor ({ 44100.0, 96000.0 }, { 44100.0, 48000.0, 96000.0 }))
        {
            Rig rig (rate);
            const auto frames = static_cast<int> (rate * test::workload (4.0, 8.0));
            for (auto& stem : rig.in)
                stem = makeSine (rate, 800.0, 8.0, 1.0f); // 800 Hz is a whole number of cycles in 37.5 ms

            setAll (rig, StemControl::delaySend, 1.0f);
            setAll (rig, StemControl::reverbSend, 1.0f);
            rig.store.set (GlobalControl::tempoBpm, 400.0f);
            rig.store.set (GlobalControl::delayDivision, static_cast<float> (DelayDivision::sixteenth));
            rig.store.set (GlobalControl::delayFeedback, maxFeedback);
            rig.store.set (GlobalControl::delayReturn, 1.0f);
            rig.store.set (GlobalControl::reverbReturn, 1.0f);
            rig.start (frames);

            const auto h = measure (rig, frames);
            logMessage ("rate " + juce::String (rate) + ": limiter input peak " + juce::String (h.inputPeak)
                        + ", output peak " + juce::String (h.outputPeak) + ", max reduction "
                        + juce::String (h.maxReductionDb) + " dB");
            expect (h.finite);
            expect (h.outputPeak <= limiterCeiling);
            expect (h.inputPeak > 6.0f);  // the dry sum is 4; the resonance really adds to it
            expect (h.inputPeak <= 9.0f); // and stays within +19 dBFS (it was +40 dBFS at feedback 0.9, no pad)
            expect (h.everActive);
        }

        beginTest ("Headroom: after the loud input stops, the tails die away and the limiter lets go");

        {
            const auto rate = 48000.0;
            Rig rig (rate);
            const auto loud = static_cast<int> (rate * 1.0);
            const auto frames = static_cast<int> (rate * 14.0);

            for (auto& stem : rig.in)
            {
                stem = Stereo (frames);
                const auto burst = makeNoise (loud, 31, 1.0f);
                std::copy (burst.left.begin(), burst.left.end(), stem.left.begin());
                std::copy (burst.right.begin(), burst.right.end(), stem.right.begin());
            }

            setAll (rig, StemControl::delaySend, 1.0f);
            rig.store.set (GlobalControl::tempoBpm, 400.0f);
            rig.store.set (GlobalControl::delayDivision, static_cast<float> (DelayDivision::sixteenth));
            rig.store.set (GlobalControl::delayFeedback, maxFeedback);
            rig.start (frames);

            const auto h = measure (rig, frames);
            expect (h.finite && h.outputPeak <= limiterCeiling && h.everActive);

            MeterSnapshot meters;
            expect (rig.mix.meters().read (meters));
            expect (! meters.limiterActive());
            expect (peakOf (rig.out.left, static_cast<std::size_t> (rate * 13.0)) < 1.0e-3f);
            expect (meters.masterPeak < 1.0e-3f);
        }

        beginTest ("Headroom: the limiter cannot hide a quiet mix; below the ceiling nothing is reduced");

        {
            const auto rate = 48000.0;
            Rig rig (rate);
            const auto frames = static_cast<int> (rate * 1.0);
            fillNoise (rig, frames, 0.1f);
            rig.store.set (GlobalControl::masterGain, 3.0f);
            rig.start (frames);
            const auto h = measure (rig, frames);
            expect (! h.everActive);
            expectEquals (h.maxReductionDb, 0.0f);
            expect (h.outputPeak < limiterCeiling);
        }
    }
};

DspMixProcessorTests dspMixProcessorTests;
} // namespace
} // namespace snds::dsp
