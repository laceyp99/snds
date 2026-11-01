#include <juce_core/juce_core.h>

#include "DspHeavy.h"
#include "dsp/MixProcessor.h"
#include "dsp/VisualAnalysis.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>
#include <vector>

// The allocation counter lives in AudioCallbackAuditTests.cpp (it replaces the global operator
// new for the whole test executable); this file reuses it and must not define its own.
namespace snds::test::audit
{
extern thread_local bool tracking;
extern thread_local long long allocationCount;
} // namespace snds::test::audit

namespace snds::dsp
{
namespace
{
constexpr double sampleRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

struct Mono
{
    explicit Mono (int frames = 0)
        : samples (static_cast<std::size_t> (frames), 0.0f)
    {
    }

    std::vector<float> samples;
};

Mono sine (double rate, double hertz, double seconds, float amplitude)
{
    Mono m (static_cast<int> (rate * seconds));
    for (std::size_t i = 0; i < m.samples.size(); ++i)
        m.samples[i] =
            amplitude * static_cast<float> (std::sin (2.0 * std::numbers::pi * hertz * static_cast<double> (i) / rate));
    return m;
}

Mono noise (int frames, unsigned seed, float amplitude)
{
    juce::Random random (static_cast<juce::int64> (seed));
    Mono m (frames);
    for (auto& v : m.samples)
        v = amplitude * (random.nextFloat() * 2.0f - 1.0f);
    return m;
}

// A drum-like hit: a burst of decaying noise.
void addHit (Mono& m, int at, float amplitude, double rate, unsigned seed = 1)
{
    juce::Random random (static_cast<juce::int64> (seed + static_cast<unsigned> (at)));
    const auto length = static_cast<int> (rate * 0.08);
    for (int i = 0; i < length && at + i < static_cast<int> (m.samples.size()); ++i)
        m.samples[static_cast<std::size_t> (at + i)] +=
            amplitude * (random.nextFloat() * 2.0f - 1.0f)
            * std::exp (-static_cast<float> (i) / (0.02f * static_cast<float> (rate)));
}

// Four mono stems (duplicated to both channels), run through a VisualAnalyzer in blocks.
struct Rig
{
    explicit Rig (double sampleRate)
        : rate (sampleRate)
    {
        analyzer.prepare (rate);
    }

    // Processes [from, to) in blocks of `block` frames (0: random sizes 1..256) and returns the
    // snapshots published after each block.
    std::vector<VisualSnapshot> run (int from, int to, int block = 256, unsigned seed = 1)
    {
        std::vector<VisualSnapshot> snapshots;
        juce::Random random (static_cast<juce::int64> (seed));

        for (int start = from; start < to;)
        {
            const auto want = block > 0 ? block : 1 + random.nextInt (256);
            const auto n = std::min (want, to - start);

            std::array<std::array<const float*, 2>, numStems> channels;
            std::array<const float* const*, numStems> pointers {};
            for (std::size_t s = 0; s < channels.size(); ++s)
            {
                if (in[s].samples.size() < static_cast<std::size_t> (to))
                    in[s].samples.resize (static_cast<std::size_t> (to), 0.0f);
                channels[s] = { in[s].samples.data() + start, in[s].samples.data() + start };
                pointers[s] = channels[s].data();
            }

            analyzer.process (pointers, pan, bpm, n);
            snapshots.push_back (analyzer.current());
            start += n;
        }

        return snapshots;
    }

    double rate;
    VisualAnalyzer analyzer;
    std::array<Mono, numStems> in;
    std::array<float, numStems> pan {};
    float bpm = 120.0f;
};

bool sameValues (const VisualSnapshot& a, const VisualSnapshot& b)
{
    return a.drumsPulse == b.drumsPulse && a.bassDepth == b.bassDepth && a.vocalGlow == b.vocalGlow
        && a.otherTexture == b.otherTexture && a.pan == b.pan && a.level == b.level && a.bpm == b.bpm
        && a.beatPhase == b.beatPhase && a.framesAnalysed == b.framesAnalysed;
}

bool inRange (const VisualSnapshot& s)
{
    auto unit = [] (float v) { return std::isfinite (v) && v >= 0.0f && v <= 1.0f; };
    bool ok = unit (s.drumsPulse) && unit (s.bassDepth) && unit (s.vocalGlow) && unit (s.otherTexture)
           && unit (s.beatPhase) && s.beatPhase < 1.0f;
    for (const auto v : s.level)
        ok = ok && unit (v);
    for (const auto v : s.pan)
        ok = ok && std::isfinite (v) && v >= -1.0f && v <= 1.0f;
    return ok;
}

class VisualAnalysisTests final : public juce::UnitTest
{
public:
    VisualAnalysisTests()
        : juce::UnitTest ("Visual analysis", "snds")
    {
    }

    void runTest() override
    {
        testIsolation();
        testPulse();
        testBands();
        testDeterminism();
        testClockAndPan();
        testBoundsAndRobustness();
        testMixProcessorIntegration();
        testNoAllocationAndStore();
    }

private:
    static float peakOf (const std::vector<VisualSnapshot>& v, float VisualSnapshot::* field)
    {
        float peak = 0.0f;
        for (const auto& s : v)
            peak = std::max (peak, s.*field);
        return peak;
    }

    void testIsolation()
    {
        beginTest ("Each stem drives only its own visual role");

        for (const auto rate : { 44100.0, 96000.0 })
        {
            const auto frames = static_cast<int> (rate * 2.0);

            // One stem at a time gets a signal that suits every role (loud, broadband, with hits);
            // the other three are silent.
            for (std::size_t active = 0; active < numStems; ++active)
            {
                Rig rig (rate);
                // Loud and broadband: noise, a bass tone, a bright tone, and a drum hit twice a second.
                auto signal = noise (frames, 7, 0.15f);
                const auto bassTone = sine (rate, 60.0, 2.0, 0.3f);
                const auto brightTone = sine (rate, 6000.0, 2.0, 0.3f);
                for (std::size_t i = 0; i < signal.samples.size(); ++i)
                    signal.samples[i] += bassTone.samples[i] + brightTone.samples[i];
                for (int at = 0; at < frames; at += static_cast<int> (rate * 0.5))
                    addHit (signal, at, 1.0f, rate);
                rig.in[active] = signal;
                const auto snapshots = rig.run (0, frames);

                const auto pulse = peakOf (snapshots, &VisualSnapshot::drumsPulse);
                const auto bass = peakOf (snapshots, &VisualSnapshot::bassDepth);
                const auto texture = peakOf (snapshots, &VisualSnapshot::otherTexture);
                const auto glow = peakOf (snapshots, &VisualSnapshot::vocalGlow);

                expect (active == 0 ? pulse > 0.3f : pulse == 0.0f,
                        "pulse, stem " + juce::String (static_cast<int> (active)));
                expect (active == 1 ? bass > 0.3f : bass == 0.0f,
                        "bass, stem " + juce::String (static_cast<int> (active)));
                expect (active == 2 ? texture > 0.3f : texture == 0.0f,
                        "texture, stem " + juce::String (static_cast<int> (active)));
                expect (active == 3 ? glow > 0.3f : glow == 0.0f,
                        "glow, stem " + juce::String (static_cast<int> (active)));

                // And only that stem has a level.
                for (std::size_t s = 0; s < numStems; ++s)
                    expect (s == active ? snapshots.back().level[s] > 0.2f : snapshots.back().level[s] == 0.0f);
            }
        }

        beginTest ("The bass role is the low band, the texture role the high band");

        {
            const auto rate = 48000.0;
            const auto frames = static_cast<int> (rate * 1.5);

            Rig low (rate), high (rate);
            low.in[1] = sine (rate, 60.0, 1.5, 0.5f);
            high.in[1] = sine (rate, 6000.0, 1.5, 0.5f);
            const auto lowRun = low.run (0, frames);
            const auto highRun = high.run (0, frames);
            expect (lowRun.back().bassDepth > 0.7f);
            expect (highRun.back().bassDepth < 0.05f); // a loud 6 kHz tone is not bass
            expect (highRun.back().level[1] > 0.7f);   // though the stem itself is loud

            Rig lowOther (rate), highOther (rate);
            lowOther.in[2] = sine (rate, 100.0, 1.5, 0.5f);
            highOther.in[2] = sine (rate, 6000.0, 1.5, 0.5f);
            expect (lowOther.run (0, frames).back().otherTexture < 0.1f);
            expect (highOther.run (0, frames).back().otherTexture > 0.7f);
        }
    }

    void testPulse()
    {
        beginTest ("The drums pulse jumps on a hit, decays, grows with the hit, and ignores sustained sound");

        const auto rate = 48000.0;
        const auto frames = static_cast<int> (rate * 3.0);

        {
            Rig rig (rate);
            rig.in[0] = Mono (frames);
            addHit (rig.in[0], static_cast<int> (rate * 1.0), 0.5f, rate);
            const auto s = rig.run (0, frames, 64);

            const auto before = static_cast<std::size_t> (rate * 0.99 / 64);
            const auto onset = static_cast<std::size_t> (rate * 1.0 / 64);
            expectEquals (s[before].drumsPulse, 0.0f);
            expect (peakOf ({ s.begin() + static_cast<std::ptrdiff_t> (onset),
                              s.begin() + static_cast<std::ptrdiff_t> (onset) + 8 },
                            &VisualSnapshot::drumsPulse)
                    > 0.5f); // within ~10 ms
            const auto later = static_cast<std::size_t> (rate * 1.8 / 64);
            expect (s[later].drumsPulse < 0.02f);
        }

        // Bigger hits give a bigger pulse.
        float previous = 0.0f;
        for (const auto amplitude : { 0.05f, 0.1f, 0.2f, 0.4f, 0.8f })
        {
            Rig rig (rate);
            rig.in[0] = Mono (static_cast<int> (rate));
            addHit (rig.in[0], 100, amplitude, rate);
            const auto peak = peakOf (rig.run (0, static_cast<int> (rate)), &VisualSnapshot::drumsPulse);
            expect (peak >= previous);
            previous = peak;
        }
        expect (previous > 0.6f);

        // A sustained loud tone starts the stem but does not keep pulsing it.
        {
            Rig rig (rate);
            rig.in[0] = sine (rate, 200.0, 3.0, 0.5f);
            const auto s = rig.run (0, frames);
            expect (s.back().drumsPulse < 0.02f);
            expect (s.back().level[0] > 0.7f);
        }

        // Hits in a steady groove each make their own pulse.
        {
            Rig rig (rate);
            rig.in[0] = Mono (frames);
            for (int at = 0; at < frames - 4800; at += static_cast<int> (rate * 0.25))
                addHit (rig.in[0], at, 0.5f, rate);
            const auto s = rig.run (0, frames, 128);
            int rises = 0;
            for (std::size_t i = 1; i < s.size(); ++i)
                rises += s[i].drumsPulse > 0.5f && s[i - 1].drumsPulse <= 0.5f;
            expect (rises >= 10 && rises <= 12, "pulses: " + juce::String (rises));
        }
    }

    void testBands()
    {
        beginTest ("Bass depth, vocal glow and texture follow the level of their band, smoothly");

        const auto rate = 48000.0;
        const auto frames = static_cast<int> (rate * 3.0);

        for (std::size_t stem = 1; stem < numStems; ++stem)
        {
            const auto hertz = stem == 1 ? 60.0 : (stem == 2 ? 6000.0 : 440.0);
            float previous = -1.0f;

            for (const auto amplitude : { 0.01f, 0.03f, 0.1f, 0.3f, 0.7f })
            {
                Rig rig (rate);
                rig.in[stem] = sine (rate, hertz, 3.0, amplitude);
                const auto last = rig.run (0, frames).back();
                const auto value = stem == 1 ? last.bassDepth : (stem == 2 ? last.otherTexture : last.vocalGlow);
                expect (value >= previous, "stem " + juce::String (static_cast<int> (stem)));
                previous = value;
            }
            expect (previous > 0.7f);
        }

        // Smooth: a sudden loud tone ramps up and a sudden stop ramps down, never jumping.
        {
            Rig rig (rate);
            rig.in[3] = sine (rate, 440.0, 1.0, 0.5f);
            const auto s = rig.run (0, static_cast<int> (rate * 4.0), 128);
            float worstStep = 0.0f;
            for (std::size_t i = 1; i < s.size(); ++i)
                worstStep = std::max (worstStep, std::abs (s[i].vocalGlow - s[i - 1].vocalGlow));
            expect (worstStep < 0.6f,
                    "step " + juce::String (worstStep)); // the onset is steep in dB, never a jump to full
            expect (s.back().vocalGlow < 0.05f);
            expect (peakOf (s, &VisualSnapshot::vocalGlow) > 0.7f);
        }
    }

    void testDeterminism()
    {
        beginTest ("The same input and block partition give the same snapshot sequence, bit for bit");

        for (const auto rate : sampleRates)
        {
            const auto frames = static_cast<int> (rate * 1.5);
            auto make = [&] (Rig& rig)
            {
                for (std::size_t s = 0; s < numStems; ++s)
                {
                    rig.in[s] = noise (frames, static_cast<unsigned> (3 + s), 0.3f);
                    addHit (rig.in[s], 1000, 0.8f, rate);
                }
                rig.pan = { -0.5f, 0.0f, 0.3f, 1.0f };
                rig.bpm = 133.0f;
            };

            Rig a (rate), b (rate);
            make (a);
            make (b);
            const auto first = a.run (0, frames, 0, 5);
            const auto second = b.run (0, frames, 0, 5);
            expect (first.size() == second.size());
            bool identical = first.size() == second.size();
            for (std::size_t i = 0; identical && i < first.size(); ++i)
                identical = sameValues (first[i], second[i]);
            expect (identical);
        }

        beginTest ("The filter and envelope state does not depend on how the audio is cut into blocks");

        {
            const auto rate = 44100.0;
            const auto frames = 44100;
            Rig a (rate), b (rate);
            for (auto* rig : { &a, &b })
                for (std::size_t s = 0; s < numStems; ++s)
                {
                    rig->in[s] = noise (frames, static_cast<unsigned> (9 + s), 0.4f);
                    addHit (rig->in[s], 5000, 0.7f, rate);
                }

            const auto whole = a.run (0, frames, frames);
            const auto pieces = b.run (0, frames, 1);
            expect (whole.size() == 1 && pieces.size() == static_cast<std::size_t> (frames));
            expect (whole.back().drumsPulse == pieces.back().drumsPulse);
            expect (whole.back().bassDepth == pieces.back().bassDepth);
            expect (whole.back().vocalGlow == pieces.back().vocalGlow);
            expect (whole.back().otherTexture == pieces.back().otherTexture);
            expect (whole.back().level == pieces.back().level);
            expectWithinAbsoluteError (whole.back().beatPhase, pieces.back().beatPhase, 1.0e-6f);
        }

        beginTest ("After a reset the analysis starts over exactly as a fresh one does");

        {
            const auto rate = 48000.0;
            const auto frames = static_cast<int> (rate * 0.5);
            Rig fresh (rate), reused (rate);
            for (auto* rig : { &fresh, &reused })
                for (std::size_t s = 0; s < numStems; ++s)
                    rig->in[s] = noise (frames, static_cast<unsigned> (21 + s), 0.3f);

            reused.run (0, frames); // loud history
            reused.analyzer.reset();
            const auto a = fresh.run (0, frames);
            const auto b = reused.run (0, frames);

            bool identical = true;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                identical = identical && a[i].drumsPulse == b[i].drumsPulse && a[i].bassDepth == b[i].bassDepth
                         && a[i].vocalGlow == b[i].vocalGlow && a[i].otherTexture == b[i].otherTexture
                         && a[i].level == b[i].level && a[i].beatPhase == b[i].beatPhase;
            }
            expect (identical);
        }
    }

    void testClockAndPan()
    {
        beginTest ("The visual clock counts frames monotonically, across resets, and carries the tempo");

        const auto rate = 48000.0;
        Rig rig (rate);
        rig.bpm = 120.0f;
        rig.in[0] = noise (48000, 2, 0.1f);

        auto first = rig.run (0, 12000, 100);
        expectEquals (first.back().framesAnalysed, std::uint64_t (12000));
        expectEquals (first.back().bpm, 120.0f);
        expectEquals (first.back().sampleRate, 48000.0f);
        expectWithinAbsoluteError (first.back().seconds(), 0.25, 1.0e-9);

        // At 120 BPM a beat is 0.5 s, so 12000 frames (0.25 s) is half a beat.
        expectWithinAbsoluteError (first.back().beatPhase, 0.5f, 1.0e-4f);
        const auto second = rig.run (12000, 24000, 100);
        expect (second.back().beatPhase < 2.0e-4f || second.back().beatPhase > 0.9998f);

        const auto before = second.back().framesAnalysed;
        const auto resetsBefore = second.back().resetCount;
        rig.analyzer.reset();
        expectEquals (rig.analyzer.current().resetCount, resetsBefore + 1);
        expectEquals (rig.analyzer.current().framesAnalysed, before); // the clock keeps its place
        expectEquals (rig.analyzer.current().beatPhase, 0.0f);        // the phase restarts
        expectEquals (rig.analyzer.current().drumsPulse, 0.0f);

        rig.bpm = 90.0f;
        const auto after = rig.run (24000, 24100, 100);
        expectEquals (after.back().framesAnalysed, before + 100);
        expectEquals (after.back().bpm, 90.0f);

        std::uint64_t last = 0;
        bool monotonic = true;
        for (const auto& s : rig.run (24100, 40000, 0, 3))
        {
            monotonic = monotonic && s.framesAnalysed > last;
            last = s.framesAnalysed;
        }
        expect (monotonic);

        beginTest ("Pan positions are passed through, bounded, for every stem");

        Rig panRig (rate);
        panRig.pan = { -1.0f, -0.25f, 0.6f, 1.0f };
        expect (panRig.run (0, 256).back().pan == panRig.pan);

        panRig.pan = { -5.0f, std::numeric_limits<float>::quiet_NaN(), 5.0f, std::numeric_limits<float>::infinity() };
        const auto clamped = panRig.run (256, 512).back();
        expectEquals (clamped.pan[0], -1.0f);
        expectEquals (clamped.pan[1], 0.0f);
        expectEquals (clamped.pan[2], 1.0f);
        expectEquals (clamped.pan[3], 0.0f);
    }

    void testBoundsAndRobustness()
    {
        beginTest ("Output stays bounded and finite for loud, hot and hostile input");

        const auto rate = 48000.0;
        const auto frames = static_cast<int> (rate * 2.0);
        Rig rig (rate);
        for (std::size_t s = 0; s < numStems; ++s)
            rig.in[s] = noise (frames, static_cast<unsigned> (50 + s), 8.0f); // far over full scale
        rig.in[0].samples[100] = std::numeric_limits<float>::quiet_NaN();
        rig.in[1].samples[200] = std::numeric_limits<float>::infinity();
        rig.in[2].samples[300] = -1.0e20f;

        bool ranged = true;
        for (const auto& s : rig.run (0, frames, 0, 8))
            ranged = ranged && inRange (s);
        expect (ranged);

        // And it recovers: silence afterwards decays everything back to exactly zero.
        for (auto& stem : rig.in)
            stem.samples.assign (static_cast<std::size_t> (frames * 3), 0.0f);
        const auto tail = rig.run (frames, frames + static_cast<int> (rate * 3.0)).back();
        expect (inRange (tail));
        expectEquals (tail.drumsPulse, 0.0f);
        expectEquals (tail.bassDepth, 0.0f);
        expectEquals (tail.vocalGlow, 0.0f);
        expectEquals (tail.otherTexture, 0.0f);
        for (const auto v : tail.level)
            expectEquals (v, 0.0f);
    }

    //==========================================================================
    struct MixRig
    {
        explicit MixRig (double sampleRate)
            : rate (sampleRate),
              mix (store)
        {
        }

        void start (int totalFrames)
        {
            frames = totalFrames;
            for (auto& stem : in)
                if (stem.samples.size() < static_cast<std::size_t> (frames))
                    stem.samples.resize (static_cast<std::size_t> (frames), 0.0f);
            mix.prepare (rate, 256);
        }

        VisualSnapshot run (int from, int to)
        {
            std::vector<float> left (256), right (256);
            for (int start = from; start < to; start += 256)
            {
                const auto n = std::min (256, to - start);
                std::array<std::array<const float*, 2>, numStems> channels;
                std::array<const float* const*, numStems> pointers {};
                for (std::size_t s = 0; s < numStems; ++s)
                {
                    channels[s] = { in[s].samples.data() + start, in[s].samples.data() + start };
                    pointers[s] = channels[s].data();
                }
                std::array<float*, 2> out { left.data(), right.data() };
                mix.process (pointers, out.data(), n);
            }

            VisualSnapshot snapshot;
            expect (mix.visuals().read (snapshot));
            return snapshot;
        }

        void expect (bool ok) { ok_ = ok_ && ok; }

        double rate;
        ParameterStore store;
        MixProcessor mix;
        std::array<Mono, numStems> in;
        int frames = 0;
        bool ok_ = true;
    };

    void testMixProcessorIntegration()
    {
        beginTest ("Through the mix: mute, solo, fader and pan change what the visuals see");

        const auto rate = 48000.0;
        const auto seconds = snds::test::workload (1.0, 2.0); // every mix variant is analysed from scratch
        const auto frames = static_cast<int> (rate * seconds);

        auto fill = [&] (MixRig& rig)
        {
            for (std::size_t s = 0; s < numStems; ++s)
            {
                rig.in[s] = noise (frames, static_cast<unsigned> (70 + s), 0.12f);
                const auto bassTone = sine (rate, 60.0, seconds, 0.2f);
                const auto brightTone = sine (rate, 6000.0, seconds, 0.2f);
                for (std::size_t i = 0; i < rig.in[s].samples.size(); ++i)
                    rig.in[s].samples[i] += bassTone.samples[i] + brightTone.samples[i];
                for (int at = 0; at < frames; at += 12000)
                    addHit (rig.in[s], at, 0.4f, rate);
            }
        };

        {
            MixRig rig (rate);
            fill (rig);
            rig.start (frames);
            const auto s = rig.run (0, frames);
            expect (s.level[0] > 0.2f && s.level[1] > 0.2f && s.level[2] > 0.2f && s.level[3] > 0.2f);
            expect (s.bassDepth > 0.2f && s.vocalGlow > 0.2f && s.otherTexture > 0.2f);
            expect (s.pan == std::array<float, numStems> {});
        }

        for (std::size_t muted = 0; muted < numStems; ++muted)
        {
            MixRig rig (rate);
            fill (rig);
            rig.store.set (static_cast<Stem> (muted), StemControl::mute, 1.0f);
            rig.start (frames);
            const auto s = rig.run (0, frames);

            expectEquals (s.level[muted], 0.0f);
            if (muted == 0)
                expectEquals (s.drumsPulse, 0.0f);
            if (muted == 1)
                expectEquals (s.bassDepth, 0.0f);
            if (muted == 2)
                expectEquals (s.otherTexture, 0.0f);
            if (muted == 3)
                expectEquals (s.vocalGlow, 0.0f);
            for (std::size_t other = 0; other < numStems; ++other)
                if (other != muted)
                    expect (s.level[other] > 0.2f);
        }

        {
            // Soloing the vocals silences the other three in the visuals as well.
            MixRig rig (rate);
            fill (rig);
            rig.store.set (Stem::vocals, StemControl::solo, 1.0f);
            rig.start (frames);
            const auto s = rig.run (0, frames);
            expect (s.level[3] > 0.2f && s.vocalGlow > 0.2f);
            expectEquals (s.level[0], 0.0f);
            expectEquals (s.level[1], 0.0f);
            expectEquals (s.level[2], 0.0f);
            expectEquals (s.bassDepth, 0.0f);
            expectEquals (s.otherTexture, 0.0f);
            expectEquals (s.drumsPulse, 0.0f);
        }

        {
            // The fader is before the analysis: -12 dB reads lower, -inf reads zero.
            float previous = 2.0f;
            for (const auto gain : { 6.0f, 0.0f, -12.0f, -30.0f, -60.0f })
            {
                MixRig rig (rate);
                fill (rig);
                rig.store.set (Stem::vocals, StemControl::gain, gain);
                rig.start (frames);
                const auto s = rig.run (0, frames);
                expect (s.vocalGlow <= previous);
                previous = s.vocalGlow;
                if (gain == -60.0f)
                    expectEquals (s.vocalGlow, 0.0f);
            }
        }

        {
            // Pan is the smoothed pan of each stem, and the tempo is the tempo control's.
            MixRig rig (rate);
            fill (rig);
            rig.store.set (Stem::drums, StemControl::pan, -1.0f);
            rig.store.set (Stem::bass, StemControl::pan, -0.4f);
            rig.store.set (Stem::other, StemControl::pan, 0.5f);
            rig.store.set (Stem::vocals, StemControl::pan, 1.0f);
            rig.store.set (GlobalControl::tempoBpm, 150.0f);
            rig.start (frames);
            const auto s = rig.run (0, frames);
            expectEquals (s.pan[0], -1.0f);
            expectWithinAbsoluteError (s.pan[1], -0.4f, 1.0e-6f);
            expectWithinAbsoluteError (s.pan[2], 0.5f, 1.0e-6f);
            expectEquals (s.pan[3], 1.0f);
            expectEquals (s.bpm, 150.0f);
            expect (s.level[0] > 0.2f); // a hard-panned stem is still loud (and the pan weights by level)

            // reset(): visuals restart, the clock keeps counting.
            const auto before = s.framesAnalysed;
            rig.mix.reset();
            VisualSnapshot after;
            expect (rig.mix.visuals().read (after));
            expectEquals (after.framesAnalysed, before);
            expectEquals (after.level[0], 0.0f);
            expectEquals (after.drumsPulse, 0.0f);
            expectEquals (after.resetCount, s.resetCount + 1);
        }
    }

    void testNoAllocationAndStore()
    {
        beginTest ("The analysis allocates nothing, in the analyzer or through the mix");

        const auto rate = 48000.0;
        Rig rig (rate);
        for (std::size_t s = 0; s < numStems; ++s)
            rig.in[s] = noise (48000, static_cast<unsigned> (90 + s), 0.3f);

        // Size the inputs first (that allocates), then watch the processing only.
        rig.run (0, 256);
        const auto wasTracking = snds::test::audit::tracking;
        const auto start = snds::test::audit::allocationCount;
        snds::test::audit::tracking = true;
        std::array<std::array<const float*, 2>, numStems> channels;
        std::array<const float* const*, numStems> pointers {};
        for (std::size_t s = 0; s < numStems; ++s)
        {
            channels[s] = { rig.in[s].samples.data(), rig.in[s].samples.data() };
            pointers[s] = channels[s].data();
        }
        VisualSnapshot snapshot;
        for (int i = 0; i < 100; ++i)
        {
            rig.analyzer.process (pointers, rig.pan, 120.0f, 256);
            rig.analyzer.store().read (snapshot);
        }
        rig.analyzer.reset();
        const auto allocations = snds::test::audit::allocationCount - start;
        snds::test::audit::tracking = wasTracking;
        expectEquals (static_cast<long> (allocations), 0L);

        // The hook has to be live for that to mean anything.
        {
            const auto begin = snds::test::audit::allocationCount;
            snds::test::audit::tracking = true;
            delete new int (1);
            snds::test::audit::tracking = wasTracking;
            expect (snds::test::audit::allocationCount > begin);
        }

        beginTest ("Visual snapshots stay consistent while the audio thread publishes");

        VisualStore store;
        std::atomic<bool> stop { false };
        std::atomic<long> published { 0 };

        std::thread writer (
            [&]
            {
                float n = 0.0f;
                std::uint64_t frames = 0;
                while (! stop.load())
                {
                    VisualSnapshot s;
                    n += 1.0f;
                    frames += 1;
                    s.drumsPulse = s.bassDepth = s.vocalGlow = s.otherTexture = n;
                    s.pan.fill (n);
                    s.level.fill (n);
                    s.bpm = s.beatPhase = n;
                    s.framesAnalysed = frames;
                    s.resetCount = static_cast<std::uint32_t> (frames);
                    store.publish (s);
                    ++published;
                }
            });

        VisualSnapshot seen;
        long reads = 0, torn = 0, failed = 0;
        const auto end = juce::Time::getMillisecondCounter() + 300;

        while (juce::Time::getMillisecondCounter() < end)
        {
            if (! store.read (seen))
            {
                ++failed;
                continue;
            }

            ++reads;
            const auto n = seen.drumsPulse;
            bool same = seen.bassDepth == n && seen.vocalGlow == n && seen.otherTexture == n && seen.bpm == n
                     && seen.beatPhase == n && seen.framesAnalysed == static_cast<std::uint64_t> (n)
                     && seen.resetCount == static_cast<std::uint32_t> (n);
            for (std::size_t s = 0; s < numStems; ++s)
                same = same && seen.pan[s] == n && seen.level[s] == n;
            torn += ! same;
        }

        stop = true;
        writer.join();
        logMessage ("reads " + juce::String (reads) + ", failed " + juce::String (failed) + ", published "
                    + juce::String (published.load()));
        expect (reads > 0);
        expectEquals (torn, 0L);
    }
};

VisualAnalysisTests visualAnalysisTests;
} // namespace
} // namespace snds::dsp
