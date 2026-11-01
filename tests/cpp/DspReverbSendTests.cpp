#include <juce_core/juce_core.h>

#include "DspHeavy.h"

#include "dsp/SharedReverb.h"
#include "dsp/StemSends.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

void run (SharedReverb& reverb, const Stereo& send, Stereo& wet, int block = 256)
{
    for (int start = 0; start < send.size(); start += block)
    {
        const auto n = std::min (block, send.size() - start);
        std::array<const float*, 2> in { send.left.data() + start, send.right.data() + start };
        std::array<float*, 2> out { wet.left.data() + start, wet.right.data() + start };
        reverb.process (in.data(), out.data(), n);
    }
}

Stereo makeNoise (int frames, unsigned seed, float amplitude = 1.0f)
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

double rms (const std::vector<float>& v, std::size_t from)
{
    double sum = 0.0;
    for (auto i = from; i < v.size(); ++i)
        sum += static_cast<double> (v[i]) * v[i];
    return std::sqrt (sum / static_cast<double> (v.size() - from));
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

class DspReverbSendTests final : public juce::UnitTest
{
public:
    DspReverbSendTests()
        : juce::UnitTest ("DSP reverb and sends", "snds")
    {
    }

    void runTest() override
    {
        testReverb();
        testSends();
    }

private:
    void testReverb()
    {
        beginTest ("Reverb: an impulse gives a finite stereo tail that decays");

        for (const auto rate : test::ratesFor ({ 48000.0 }, { 44100.0, 48000.0, 96000.0, 192000.0 }))
        {
            SharedReverb reverb;
            reverb.prepare (rate);
            reverb.setReturn (1.0f);
            reverb.reset();

            Stereo send (static_cast<int> (rate * 8.0)), wet (send.size());
            send.left[0] = 1.0f;
            run (reverb, send, wet);

            expect (allFinite (wet.left) && allFinite (wet.right));
            const auto early = peakOf (wet.left, 0, static_cast<std::size_t> (rate * 0.5));
            const auto late = peakOf (wet.left, static_cast<std::size_t> (rate * 7.0));
            expect (early > 0.001f); // there is a tail
            expect (peakOf (wet.right, 0, static_cast<std::size_t> (rate * 0.5)) > 0.001f);
            expect (late < early * 0.01f); // and it dies away (> 40 dB)
            expect (peakOf (wet.left) < 1.0f);
        }

        beginTest ("Reverb: a full-scale noise send comes back at a sensible level");

        for (const auto rate : { 44100.0, 48000.0 })
        {
            SharedReverb reverb;
            reverb.prepare (rate);
            reverb.setReturn (1.0f);
            reverb.reset();

            auto send = makeNoise (static_cast<int> (rate * 5.0), 2);
            Stereo wet (send.size());
            run (reverb, send, wet);

            const auto from = static_cast<std::size_t> (rate * 2.0);
            const auto gain = rms (wet.left, from) / rms (send.left, from);
            logMessage ("rate " + juce::String (rate) + ": wet/send RMS " + juce::String (gain) + ", peak "
                        + juce::String (peakOf (wet.left)));
            expect (gain > 0.4 && gain < 1.6);
            expect (peakOf (wet.left) < 4.0f);
        }

        beginTest ("Reverb: return zero is exact silence; the return ramps without a step");

        {
            const auto rate = 48000.0;
            auto send = makeNoise (static_cast<int> (rate), 5);

            SharedReverb silent;
            silent.prepare (rate);
            silent.setReturn (0.0f);
            silent.reset();
            Stereo none (send.size());
            run (silent, send, none);
            expectEquals (peakOf (none.left), 0.0f);
            expectEquals (peakOf (none.right), 0.0f);

            SharedReverb steady, ramped;
            for (auto* r : { &steady, &ramped })
            {
                r->prepare (rate);
                r->setReturn (1.0f);
                r->reset();
            }

            Stereo a (send.size()), b (send.size());
            run (steady, send, a);

            // Same input, but the return drops to zero at 0.5 s: a linear 20 ms ramp on the same signal
            // (JUCE's ramp has N - 1 steps, hence the tolerance of one step).
            const auto at = static_cast<int> (rate * 0.5);
            Stereo first (at), second (send.size() - at);
            std::copy (send.left.begin(), send.left.begin() + at, first.left.begin());
            std::copy (send.right.begin(), send.right.begin() + at, first.right.begin());
            std::copy (send.left.begin() + at, send.left.end(), second.left.begin());
            std::copy (send.right.begin() + at, send.right.end(), second.right.begin());
            Stereo outFirst (at), outSecond (second.size());
            run (ramped, first, outFirst);
            ramped.setReturn (0.0f);
            run (ramped, second, outSecond);

            const auto rampLength = static_cast<int> (std::lround (0.02 * rate));
            for (int i = 0; i < second.size(); ++i)
            {
                const auto gain = std::max (0.0f, 1.0f - static_cast<float> (i + 1) / static_cast<float> (rampLength));
                const auto expected = a.left[static_cast<std::size_t> (at + i)] * gain;
                expectWithinAbsoluteError (outSecond.left[static_cast<std::size_t> (i)],
                                           expected,
                                           0.0015f * std::abs (a.left[static_cast<std::size_t> (at + i)]) + 2.0e-5f);
            }
            expectEquals (peakOf (outSecond.left, static_cast<std::size_t> (rampLength + 1)), 0.0f);
        }

        beginTest ("Reverb: reset clears the tail; hostile input does not poison it");

        {
            SharedReverb reverb;
            reverb.prepare (48000.0);
            reverb.setReturn (1.0f);
            reverb.reset();

            auto loud = makeNoise (48000, 9);
            loud.left[10] = std::numeric_limits<float>::quiet_NaN();
            loud.right[20] = std::numeric_limits<float>::infinity();
            Stereo wet (loud.size());
            run (reverb, loud, wet);
            expect (allFinite (wet.left) && allFinite (wet.right));

            reverb.reset();
            Stereo silence (96000), out (96000);
            run (reverb, silence, out);
            expectEquals (peakOf (out.left), 0.0f);
            expectEquals (peakOf (out.right), 0.0f);
        }

        beginTest ("Reverb: output does not depend on the block size");

        {
            auto send = makeNoise (20000, 13, 0.5f);
            Stereo whole (20000), pieces (20000);
            SharedReverb a, b;
            for (auto* r : { &a, &b })
            {
                r->prepare (44100.0);
                r->setReturn (0.8f);
                r->reset();
            }
            run (a, send, whole, 20000);
            run (b, send, pieces, 1);
            expect (whole.left == pieces.left && whole.right == pieces.right);
        }
    }

    //==========================================================================
    static ParameterSnapshot snapshotWith (const std::vector<std::pair<int, float>>& values)
    {
        ParameterStore store;
        store.setMany (values);
        ParameterSnapshot snapshot;
        store.read (snapshot);
        return snapshot;
    }

    void testSends()
    {
        constexpr int frames = 64;
        std::array<Stereo, numStems> stems { Stereo (frames, 1.0f, 0.5f),
                                             Stereo (frames, 0.5f, 0.25f),
                                             Stereo (frames, -0.25f, 0.125f),
                                             Stereo (frames, 0.1f, -0.2f) };

        const auto pointers = [&stems] (int stem)
        {
            return std::array<const float*, 2> { stems[static_cast<std::size_t> (stem)].left.data(),
                                                 stems[static_cast<std::size_t> (stem)].right.data() };
        };

        beginTest ("Sends: default (zero) sends put nothing on either bus");

        StemSends sends;
        sends.prepare (48000.0, frames);
        sends.setTargets (ParameterSnapshot());
        sends.reset();
        sends.beginBlock (frames);
        for (int s = 0; s < numStems; ++s)
            sends.addStem (s, pointers (s).data(), frames);
        for (int i = 0; i < frames; ++i)
            expect (sends.reverbBus()[0][i] == 0.0f && sends.delayBus()[1][i] == 0.0f);

        beginTest ("Sends: each stem feeds each bus at its own gain");

        std::vector<std::pair<int, float>> values;
        const float reverbSend[] = { 1.0f, 0.5f, 0.0f, 0.25f };
        const float delaySend[] = { 0.0f, 1.0f, 0.5f, 0.25f };
        for (int s = 0; s < numStems; ++s)
        {
            values.emplace_back (parameterIndex (static_cast<Stem> (s), StemControl::reverbSend), reverbSend[s]);
            values.emplace_back (parameterIndex (static_cast<Stem> (s), StemControl::delaySend), delaySend[s]);
        }

        sends.setTargets (snapshotWith (values));
        sends.reset();
        sends.beginBlock (frames);
        for (int s = 0; s < numStems; ++s)
            sends.addStem (s, pointers (s).data(), frames);

        float expectReverbL = 0.0f, expectReverbR = 0.0f, expectDelayL = 0.0f, expectDelayR = 0.0f;
        for (int s = 0; s < numStems; ++s)
        {
            expectReverbL += reverbSend[s] * sendBusPad * stems[static_cast<std::size_t> (s)].left[0];
            expectReverbR += reverbSend[s] * sendBusPad * stems[static_cast<std::size_t> (s)].right[0];
            expectDelayL += delaySend[s] * sendBusPad * stems[static_cast<std::size_t> (s)].left[0];
            expectDelayR += delaySend[s] * sendBusPad * stems[static_cast<std::size_t> (s)].right[0];
        }

        for (int i = 0; i < frames; ++i)
        {
            expectWithinAbsoluteError (sends.reverbBus()[0][i], expectReverbL, 1.0e-6f);
            expectWithinAbsoluteError (sends.reverbBus()[1][i], expectReverbR, 1.0e-6f);
            expectWithinAbsoluteError (sends.delayBus()[0][i], expectDelayL, 1.0e-6f);
            expectWithinAbsoluteError (sends.delayBus()[1][i], expectDelayR, 1.0e-6f);
        }

        beginTest ("Sends: a silent (muted) stem contributes nothing; beginBlock clears the buses");

        Stereo silent (frames);
        sends.beginBlock (frames);
        std::array<const float*, 2> silentPointers { silent.left.data(), silent.right.data() };
        for (int s = 0; s < numStems; ++s)
            sends.addStem (s, silentPointers.data(), frames);
        for (int i = 0; i < frames; ++i)
            expect (sends.reverbBus()[0][i] == 0.0f && sends.delayBus()[0][i] == 0.0f);

        beginTest ("Sends: send changes ramp over 20 ms without a step");

        for (const auto rate : sampleRates)
        {
            StemSends ramping;
            ramping.prepare (rate, 256);
            ramping.setTargets (ParameterSnapshot());
            ramping.reset();

            Stereo dc (256, 1.0f, 1.0f);
            std::array<const float*, 2> in { dc.left.data(), dc.right.data() };
            const auto bound = 1.25f * sendBusPad / (0.020f * static_cast<float> (rate));
            float last = 0.0f, worst = 0.0f;

            for (int block = 0; block < static_cast<int> (rate * 0.08 / 256) + 1; ++block)
            {
                if (block == 2)
                    ramping.setTargets (
                        snapshotWith ({ { parameterIndex (Stem::drums, StemControl::reverbSend), 1.0f },
                                        { parameterIndex (Stem::drums, StemControl::delaySend), 1.0f } }));

                ramping.beginBlock (256);
                ramping.addStem (0, in.data(), 256);

                for (int i = 0; i < 256; ++i)
                {
                    worst = std::max (worst, std::abs (ramping.reverbBus()[0][i] - last));
                    last = ramping.reverbBus()[0][i];
                }
            }

            expect (worst <= bound);
            expectEquals (last, sendBusPad);
        }
    }
};

DspReverbSendTests dspReverbSendTests;
} // namespace
} // namespace snds::dsp
