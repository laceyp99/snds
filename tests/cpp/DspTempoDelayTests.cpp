#include <juce_core/juce_core.h>

#include "dsp/TempoDelay.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

    explicit Stereo (int frames)
        : left (static_cast<std::size_t> (frames), 0.0f),
          right (static_cast<std::size_t> (frames), 0.0f)
    {
    }

    int size() const { return static_cast<int> (left.size()); }
};

// Processes [start, start + count) of `send` into the same range of `wet`.
void runRange (TempoDelay& delay, const Stereo& send, Stereo& wet, int start, int count)
{
    std::array<const float*, 2> in { send.left.data() + start, send.right.data() + start };
    std::array<float*, 2> out { wet.left.data() + start, wet.right.data() + start };
    delay.process (in.data(), out.data(), count);
}

void run (TempoDelay& delay, const Stereo& send, Stereo& wet, int block = 256)
{
    for (int start = 0; start < send.size(); start += block)
        runRange (delay, send, wet, start, std::min (block, send.size() - start));
}

int indexOfFirstNonZero (const std::vector<float>& v)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f)
            return static_cast<int> (i);
    return -1;
}

float peakOf (const std::vector<float>& v)
{
    float peak = 0.0f;
    for (const auto x : v)
        peak = std::max (peak, std::abs (x));
    return peak;
}

float maxStep (const std::vector<float>& v)
{
    float worst = 0.0f;
    for (std::size_t i = 1; i < v.size(); ++i)
        worst = std::max (worst, std::abs (v[i] - v[i - 1]));
    return worst;
}

bool allFinite (const std::vector<float>& v)
{
    return std::all_of (v.begin(), v.end(), [] (float x) { return std::isfinite (x); });
}

Stereo makeSine (double rate, double hertz, double seconds, float amplitude)
{
    Stereo s (static_cast<int> (rate * seconds));
    for (int i = 0; i < s.size(); ++i)
        s.left[static_cast<std::size_t> (i)] = s.right[static_cast<std::size_t> (i)] =
            amplitude * static_cast<float> (std::sin (2.0 * std::numbers::pi * hertz * i / rate));
    return s;
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

class DspTempoDelayTests final : public juce::UnitTest
{
public:
    DspTempoDelayTests()
        : juce::UnitTest ("DSP tempo delay", "snds")
    {
    }

    void runTest() override
    {
        testEchoTimes();
        testSilenceAndDecay();
        testMaxFeedback();
        testTempoChanges();
        testRobustness();
        testResetAndBlocks();
    }

private:
    static TempoDelay& configured (TempoDelay& delay, double rate, double bpm, DelayDivision division, float feedback)
    {
        delay.prepare (rate);
        delay.setTempo (bpm, division);
        delay.setFeedback (feedback);
        delay.setReturn (1.0f);
        delay.reset(); // snap: no crossfade from the default time
        return delay;
    }

    void testEchoTimes()
    {
        beginTest ("Every division lands the first echo at the musical time, at every rate");

        struct Case
        {
            double bpm;
            DelayDivision division;
        };

        std::vector<Case> cases;
        for (int d = 0; d < numDelayDivisions; ++d)
            for (const auto bpm : { 20.0, 90.0, 120.0, 133.0, 400.0 })
                cases.push_back ({ bpm, static_cast<DelayDivision> (d) });

        for (const auto rate : sampleRates)
        {
            for (const auto& c : cases)
            {
                TempoDelay delay;
                configured (delay, rate, c.bpm, c.division, 0.0f);

                const auto expected = static_cast<int> (std::lround (delayTimeSeconds (c.bpm, c.division) * rate));
                Stereo send (expected + 1000);
                Stereo wet (send.size());
                send.left[0] = 1.0f;
                send.right[0] = 0.5f;
                run (delay, send, wet);

                expectEquals (indexOfFirstNonZero (wet.left), expected);
                expectEquals (wet.left[static_cast<std::size_t> (expected)], 1.0f);
                expectEquals (wet.right[static_cast<std::size_t> (expected)], 0.5f);
                expectEquals (delay.getTargetDelaySamples(), expected);
            }
        }

        beginTest ("The buffer covers the slowest BPM with the longest division");

        for (const auto rate : sampleRates)
        {
            TempoDelay delay;
            delay.prepare (rate);
            expect (delay.getMaxDelaySamples() >= static_cast<int> (std::lround (3.0 * rate)));

            for (int d = 0; d < numDelayDivisions; ++d)
            {
                delay.setTempo (static_cast<double> (tempoMinBpm), static_cast<DelayDivision> (d));
                expect (delay.getTargetDelaySamples() <= delay.getMaxDelaySamples());
            }

            // Out-of-range tempi are clamped, never overrun the buffer.
            delay.setTempo (1.0, DelayDivision::quarter);
            expect (delay.getTargetDelaySamples() <= delay.getMaxDelaySamples());
            delay.setTempo (1.0e9, DelayDivision::sixteenth);
            expect (delay.getTargetDelaySamples() >= 1);
        }

        beginTest ("Return and feedback scale the repeats");

        {
            TempoDelay delay;
            delay.prepare (48000.0);
            delay.setTempo (120.0, DelayDivision::quarter); // 0.5 s = 24000
            delay.setFeedback (0.5f);
            delay.setReturn (0.5f);
            delay.reset();

            Stereo send (60000), wet (60000);
            send.left[0] = 1.0f;
            run (delay, send, wet);
            expectWithinAbsoluteError (wet.left[24000], 0.5f, 1.0e-6f);
            expect (wet.left[48000] > 0.0f && wet.left[48000] < 0.5f * 0.5f); // 0.5 * 0.5 less the loop filter
        }
    }

    void testSilenceAndDecay()
    {
        beginTest ("Silent input stays silent; the output is exactly zero");

        for (const auto rate : sampleRates)
        {
            TempoDelay delay;
            configured (delay, rate, 140.0, DelayDivision::dottedEighth, maxFeedback);
            Stereo send (static_cast<int> (rate)), wet (send.size());
            std::fill (wet.left.begin(), wet.left.end(), 1.0f);
            run (delay, send, wet);
            expectEquals (peakOf (wet.left), 0.0f);
            expectEquals (peakOf (wet.right), 0.0f);
        }

        beginTest ("At maximum feedback a burst decays away, window by window");

        for (const auto rate : { 44100.0, 192000.0 })
        {
            TempoDelay delay;
            configured (delay, rate, 400.0, DelayDivision::sixteenth, maxFeedback); // 37.5 ms repeats

            const auto burst = static_cast<int> (rate * 0.1);
            auto send = makeNoise (static_cast<int> (rate * 8.0), 4, 0.8f);
            std::fill (send.left.begin() + burst, send.left.end(), 0.0f);
            std::fill (send.right.begin() + burst, send.right.end(), 0.0f);
            Stereo wet (send.size());
            run (delay, send, wet);

            expect (allFinite (wet.left) && allFinite (wet.right));

            const auto window = static_cast<int> (rate * 0.5);
            float previous = std::numeric_limits<float>::max();
            float last = 0.0f;
            for (int start = burst + window; start + window <= send.size(); start += window)
            {
                const auto peak = *std::max_element (wet.left.begin() + start, wet.left.begin() + start + window);
                expect (peak <= previous * 1.02f + 1.0e-9f);
                previous = peak;
                last = peak;
            }

            logMessage ("rate " + juce::String (rate) + ": last 0.5 s window peak " + juce::String (last));
            expect (last < 1.0e-3f); // > 58 dB down from the burst within 8 s
        }
    }

    void testMaxFeedback()
    {
        beginTest ("Maximum feedback on a sustained tone at the delay's resonance stays under 1 / (1 - maxFeedback)");

        for (const auto rate : { 48000.0, 96000.0 })
        {
            TempoDelay delay;
            configured (delay, rate, 400.0, DelayDivision::sixteenth, maxFeedback); // 37.5 ms; 800 Hz = 30 cycles

            auto send = makeSine (rate, 800.0, 6.0, 0.5f);
            Stereo wet (send.size());
            run (delay, send, wet);

            const auto peak = peakOf (wet.left);
            logMessage ("rate " + juce::String (rate) + ": resonant peak " + juce::String (peak)
                        + " from an input peak of 0.5");
            expect (allFinite (wet.left));
            expect (peak <= 0.5f / (1.0f - maxFeedback) * 1.001f);
            expect (peak > 0.5f * 2.0f); // it really does build up (the worst case being exercised)
        }

        beginTest ("Feedback is clamped to the documented maximum");

        TempoDelay delay;
        configured (delay, 48000.0, 400.0, DelayDivision::sixteenth, 50.0f); // clamped to maxFeedback
        auto send = makeSine (48000.0, 800.0, 4.0, 0.5f);
        Stereo wet (send.size());
        run (delay, send, wet);
        expect (allFinite (wet.left));
        expect (peakOf (wet.left) <= 0.5f / (1.0f - maxFeedback) * 1.001f);
    }

    void testTempoChanges()
    {
        beginTest ("Tempo and division changes crossfade without clicks");

        for (const auto rate : sampleRates)
        {
            TempoDelay delay;
            configured (delay, rate, 120.0, DelayDivision::eighth, 0.5f);

            const auto frequency = 220.0;
            const auto amplitude = 0.5f;
            auto send = makeSine (rate, frequency, 12.0, amplitude);
            Stereo wet (send.size());
            juce::Random random (17);
            int position = 0;

            while (position < send.size())
            {
                const auto count =
                    std::min (1 + random.nextInt (static_cast<int> (rate * 0.4)), send.size() - position);
                runRange (delay, send, wet, position, count);
                position += count;

                // Hop between wildly different times, including the slowest and fastest.
                const double bpms[] = { 20.0, 60.0, 95.0, 120.0, 240.0, 400.0, 20.0 + 380.0 * random.nextDouble() };
                delay.setTempo (bpms[random.nextInt (7)],
                                static_cast<DelayDivision> (random.nextInt (numDelayDivisions)));
            }

            expect (allFinite (wet.left) && allFinite (wet.right));

            // Per-sample change is bounded by the tone's own slope plus the equal-power crossfade.
            const auto peak = peakOf (wet.left);
            const auto crossfade = static_cast<float> (delay.getCrossfadeSamples());
            const auto toneSlope = static_cast<float> (2.0 * std::numbers::pi * frequency / rate);
            const auto bound = 1.3f * peak * (toneSlope + std::numbers::pi_v<float> / crossfade);
            logMessage ("rate " + juce::String (rate) + ": worst step " + juce::String (maxStep (wet.left)) + ", bound "
                        + juce::String (bound) + " (peak " + juce::String (peak) + ")");
            expect (maxStep (wet.left) <= bound);
        }

        beginTest ("A time change is a smooth hop, not a jump");

        {
            TempoDelay delay;
            configured (delay, 48000.0, 120.0, DelayDivision::eighth, 0.0f);
            auto send = makeSine (48000.0, 1000.0, 1.0, 0.5f);
            Stereo wet (send.size());
            runRange (delay, send, wet, 0, 20000);
            delay.setTempo (97.0, DelayDivision::eighthTriplet);
            expect (delay.isCrossfading());
            runRange (delay, send, wet, 20000, send.size() - 20000);

            // A hard switch between these two delays would step by up to ~1.0 here.
            expect (maxStep (wet.left) < 0.5f * 2.0f * static_cast<float> (std::numbers::pi * 1000.0 / 48000.0) * 1.2f);
            expect (! delay.isCrossfading());
        }

        beginTest ("After the changes settle, the echo is exactly at the final time");

        for (const auto rate : { 44100.0, 96000.0 })
        {
            for (int d = 0; d < numDelayDivisions; ++d)
            {
                TempoDelay delay;
                configured (delay, rate, 100.0, DelayDivision::quarter, 0.0f);
                delay.setTempo (250.0, DelayDivision::sixteenth);
                delay.setTempo (60.0, DelayDivision::eighthTriplet);    // queued behind the running fade
                delay.setTempo (133.0, static_cast<DelayDivision> (d)); // the final target
                const auto expected =
                    static_cast<int> (std::lround (delayTimeSeconds (133.0, static_cast<DelayDivision> (d)) * rate));
                expectEquals (delay.getTargetDelaySamples(), expected);

                Stereo silence (static_cast<int> (rate * 0.3)), wet (silence.size());
                run (delay, silence, wet);
                expect (! delay.isCrossfading());
                expectEquals (delay.getTargetDelaySamples(), expected);

                Stereo send (expected + 100), out (expected + 100);
                send.left[0] = 1.0f;
                run (delay, send, out);
                expectEquals (indexOfFirstNonZero (out.left), expected);
                expectEquals (out.left[static_cast<std::size_t> (expected)], 1.0f);
            }
        }

        beginTest ("Setting the time it already has changes nothing");

        TempoDelay delay;
        configured (delay, 48000.0, 120.0, DelayDivision::eighth, 0.3f);
        delay.setTempo (120.0, DelayDivision::eighth);
        expect (! delay.isCrossfading());
    }

    void testRobustness()
    {
        beginTest ("A NaN, infinity or absurd sample cannot lodge in the feedback loop");

        TempoDelay delay;
        configured (delay, 48000.0, 400.0, DelayDivision::sixteenth, maxFeedback);
        auto send = makeNoise (48000, 3, 0.5f);
        send.left[100] = std::numeric_limits<float>::quiet_NaN();
        send.right[200] = std::numeric_limits<float>::infinity();
        send.left[300] = 1.0e12f;
        Stereo wet (send.size());
        run (delay, send, wet);
        expect (allFinite (wet.left) && allFinite (wet.right));
        expect (peakOf (wet.left) < 6.0f);

        Stereo silence (96000), tail (96000);
        run (delay, silence, tail);
        expect (allFinite (tail.left) && allFinite (tail.right));
    }

    void testResetAndBlocks()
    {
        beginTest ("Reset empties the delay and snaps to the targets");

        TempoDelay delay;
        configured (delay, 48000.0, 400.0, DelayDivision::sixteenth, maxFeedback);
        auto loud = makeNoise (48000, 11, 1.0f);
        Stereo wet (loud.size());
        run (delay, loud, wet);
        expect (peakOf (wet.left) > 0.5f);

        delay.setTempo (60.0, DelayDivision::quarter);
        delay.reset();
        expect (! delay.isCrossfading());
        expectEquals (delay.getTargetDelaySamples(), 48000);

        Stereo silence (200000), out (200000);
        run (delay, silence, out);
        expectEquals (peakOf (out.left), 0.0f);
        expectEquals (peakOf (out.right), 0.0f);

        beginTest ("Output does not depend on the block size, even across a tempo change");

        auto send = makeNoise (30000, 21, 0.6f);
        Stereo whole (30000), pieces (30000);

        for (int pass = 0; pass < 2; ++pass)
        {
            TempoDelay d;
            configured (d, 44100.0, 140.0, DelayDivision::eighth, 0.6f);
            auto& target = pass == 0 ? whole : pieces;
            const auto block = pass == 0 ? 30000 : 1;

            for (int start = 0; start < 30000; start += 5000) // tempo changes land on the same sample
            {
                if (start > 0)
                    d.setTempo (start == 10000 ? 75.0 : 210.0,
                                start == 10000 ? DelayDivision::dottedEighth : DelayDivision::quarter);

                const auto length = 5000;
                for (int at = start; at < start + length; at += block)
                    runRange (d, send, target, at, std::min (block, start + length - at));
            }
        }

        expect (whole.left == pieces.left && whole.right == pieces.right);
    }
};

DspTempoDelayTests dspTempoDelayTests;
} // namespace
} // namespace snds::dsp
