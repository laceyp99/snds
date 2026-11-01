#include <juce_core/juce_core.h>

#include "dsp/StemLevelStage.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace snds::dsp
{
namespace
{
constexpr double sampleRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };

struct Stereo
{
    std::vector<float> left, right;

    explicit Stereo (int frames, float l = 1.0f, float r = 1.0f)
        : left (static_cast<std::size_t> (frames), l),
          right (static_cast<std::size_t> (frames), r)
    {
    }

    int size() const { return static_cast<int> (left.size()); }
    const float* const* in() const
    {
        pointers = { left.data(), right.data() };
        return pointers.data();
    }
    float* const* out()
    {
        outPointers = { left.data(), right.data() };
        return outPointers.data();
    }

private:
    mutable std::array<const float*, 2> pointers {};
    std::array<float*, 2> outPointers {};
};

class DspStemLevelTests final : public juce::UnitTest
{
public:
    DspStemLevelTests()
        : juce::UnitTest ("DSP stem level", "snds")
    {
    }

    void runTest() override
    {
        testSoloTruthTable();
        testSnapshotAudibility();
        testGain();
        testPanLaw();
        testClickFree();
        testPositionAndMisc();
    }

private:
    using Flags = std::array<bool, numStems>;

    static Flags flags (unsigned bits)
    {
        return { (bits & 1u) != 0, (bits & 2u) != 0, (bits & 4u) != 0, (bits & 8u) != 0 };
    }

    void testSoloTruthTable()
    {
        beginTest ("Solo truth table: named cases");

        // Stems are ordered drums, bass, other, vocals.
        expect (stemAudibility (flags (0), flags (0)) == Flags { true, true, true, true });               // nothing set
        expect (stemAudibility (flags (0), flags (0b0100)) == Flags { false, false, true, false });       // one solo
        expect (stemAudibility (flags (0), flags (0b1001)) == Flags { true, false, false, true });        // two solos
        expect (stemAudibility (flags (0), flags (0b1111)) == Flags { true, true, true, true });          // all soloed
        expect (stemAudibility (flags (0b0010), flags (0)) == Flags { true, false, true, true });         // one mute
        expect (stemAudibility (flags (0b1111), flags (0)) == Flags { false, false, false, false });      // all muted
        expect (stemAudibility (flags (0b0001), flags (0b0001)) == Flags { false, false, false, false }); // mute wins
        expect (stemAudibility (flags (0b0001), flags (0b0011)) == Flags { false, true, false, false });  // solo + mute
        expect (stemAudibility (flags (0b0100), flags (0b0001))
                == Flags { true, false, false, false }); // mute elsewhere

        beginTest ("Solo truth table: every mute x solo combination");

        for (unsigned muteBits = 0; muteBits < 16; ++muteBits)
        {
            for (unsigned soloBits = 0; soloBits < 16; ++soloBits)
            {
                const auto audible = stemAudibility (flags (muteBits), flags (soloBits));

                for (unsigned i = 0; i < 4; ++i)
                {
                    const bool muted = (muteBits >> i) & 1u;
                    const bool soloed = (soloBits >> i) & 1u;
                    const bool expected = muted ? false : (soloBits == 0 ? true : soloed);
                    expect (audible[i] == expected,
                            "mute " + juce::String (muteBits) + " solo " + juce::String (soloBits) + " stem "
                                + juce::String (i));
                }
            }
        }
    }

    void testSnapshotAudibility()
    {
        beginTest ("Audibility follows the parameter snapshot");

        ParameterStore store;
        ParameterSnapshot snapshot;
        expect (stemAudibility (snapshot) == Flags { true, true, true, true });

        store.set (Stem::bass, StemControl::solo, 1.0f);
        store.set (Stem::vocals, StemControl::solo, 1.0f);
        store.set (Stem::vocals, StemControl::mute, 1.0f);
        store.read (snapshot);
        expect (stemAudibility (snapshot) == Flags { false, true, false, false });

        store.resetToDefaults();
        store.read (snapshot);
        expect (stemAudibility (snapshot) == Flags { true, true, true, true });
    }

    //==========================================================================
    static void settle (StemLevelStage& stage, double sampleRate)
    {
        Stereo scratch (static_cast<int> (sampleRate * 0.2));
        stage.process (scratch.in(), scratch.out(), scratch.size());
    }

    void testGain()
    {
        beginTest ("Defaults pass the signal through bit-exactly");

        for (const auto rate : sampleRates)
        {
            StemLevelStage stage;
            stage.prepare (rate);
            stage.setTargets (0.0f, true, 0.0f);

            Stereo signal (1000);
            juce::Random random (7);
            for (int i = 0; i < signal.size(); ++i)
            {
                signal.left[static_cast<std::size_t> (i)] = random.nextFloat() * 2.0f - 1.0f;
                signal.right[static_cast<std::size_t> (i)] = random.nextFloat() * 2.0f - 1.0f;
            }

            auto copy = signal;
            stage.process (signal.in(), copy.out(), signal.size());
            expect (copy.left == signal.left && copy.right == signal.right);
        }

        beginTest ("Gain reaches its targets: +6 dB, -6 dB, and exact silence at -inf");

        for (const auto rate : sampleRates)
        {
            StemLevelStage stage;
            stage.prepare (rate);

            const std::pair<float, float> cases[] { { 6.0f, 1.99526f },
                                                    { -6.0f, 0.501187f },
                                                    { -60.0f, 0.0f },
                                                    { 100.0f, 1.99526f }, // clamped to +6
                                                    { 0.0f, 1.0f } };
            for (const auto& [db, gain] : cases)
            {
                stage.setTargets (db, true, 0.0f);
                settle (stage, rate);
                Stereo block (64);
                stage.process (block.in(), block.out(), block.size());
                expectWithinAbsoluteError (block.left.back(), gain, 1.0e-5f);
                expectWithinAbsoluteError (block.right.back(), gain, 1.0e-5f);

                if (gain == 0.0f)
                    expectEquals (block.left.back(), 0.0f);
            }
        }

        beginTest ("Mute fades to exact silence and back; unmuting restores the fader gain");

        StemLevelStage stage;
        stage.prepare (48000.0);
        stage.setTargets (-12.0f, false, 0.0f);
        settle (stage, 48000.0);
        Stereo block (32);
        stage.process (block.in(), block.out(), block.size());
        expectEquals (block.left.back(), 0.0f);

        stage.setTargets (-12.0f, true, 0.0f);
        settle (stage, 48000.0);
        Stereo again (32);
        stage.process (again.in(), again.out(), again.size());
        expectWithinAbsoluteError (again.left.back(), std::pow (10.0f, -12.0f / 20.0f), 1.0e-5f);
    }

    void testPanLaw()
    {
        beginTest ("Pan law: unity centre, constant power, symmetric, monotonic");

        const auto centre = StemLevelStage::panGains (0.0f);
        expectEquals (centre[0], 1.0f);
        expectEquals (centre[1], 1.0f);

        const auto left = StemLevelStage::panGains (-1.0f);
        expectWithinAbsoluteError (left[0], std::sqrt (2.0f), 1.0e-6f);
        expectWithinAbsoluteError (left[1], 0.0f, 1.0e-6f);
        const auto right = StemLevelStage::panGains (1.0f);
        expectWithinAbsoluteError (right[0], 0.0f, 1.0e-6f);
        expectWithinAbsoluteError (right[1], std::sqrt (2.0f), 1.0e-6f);

        float previousLeft = 10.0f;
        float previousRight = -1.0f;
        for (int i = -20; i <= 20; ++i)
        {
            const auto pan = static_cast<float> (i) / 20.0f;
            const auto g = StemLevelStage::panGains (pan);
            const auto mirrored = StemLevelStage::panGains (-pan);

            expectWithinAbsoluteError (g[0] * g[0] + g[1] * g[1], 2.0f, 1.0e-5f); // constant power
            expectWithinAbsoluteError (g[0], mirrored[1], 1.0e-6f);
            expect (g[0] <= previousLeft && g[1] >= previousRight);
            previousLeft = g[0];
            previousRight = g[1];
        }

        expect (StemLevelStage::panGains (5.0f) == StemLevelStage::panGains (1.0f)); // clamped
        expect (StemLevelStage::panGains (-5.0f) == StemLevelStage::panGains (-1.0f));

        beginTest ("Pan applied to a signal matches the law, per channel");

        for (const auto pan : { -1.0f, -0.5f, 0.25f, 1.0f })
        {
            StemLevelStage stage;
            stage.prepare (44100.0);
            stage.setTargets (0.0f, true, pan);
            settle (stage, 44100.0);

            Stereo block (16, 0.5f, 0.25f);
            stage.process (block.in(), block.out(), block.size());
            const auto g = StemLevelStage::panGains (pan);
            expectWithinAbsoluteError (block.left.back(), 0.5f * g[0], 1.0e-6f);
            expectWithinAbsoluteError (block.right.back(), 0.25f * g[1], 1.0e-6f);
        }
    }

    //==========================================================================
    // The largest per-sample change a ramp of this kind may produce at a given rate.
    static float stepBound (double rate)
    {
        // d(fader * gate * panGain): fader <= 2 (ramps 2 per 20 ms), gate <= 1 (1 per 10 ms), pan gain
        // <= sqrt2 (ramps 2.22 per 20 ms), summed worst case, in units of 1 / rate.
        return 700.0f / static_cast<float> (rate);
    }

    void testClickFree()
    {
        beginTest ("Ramps are click-free: bounded per-sample delta under abrupt control changes");

        for (const auto rate : sampleRates)
        {
            StemLevelStage stage;
            stage.prepare (rate);

            // A full-scale constant makes any gain step show up directly in the output.
            const auto chunk = std::max (1, static_cast<int> (rate * 0.003));
            const auto bound = stepBound (rate);
            float last = 1.0f;
            float worst = 0.0f;
            juce::Random random (11);

            for (int step = 0; step < 200; ++step)
            {
                const bool audible = random.nextInt (4) != 0;
                stage.setTargets (random.nextFloat() * 66.0f - 60.0f, audible, random.nextFloat() * 2.0f - 1.0f);

                Stereo block (chunk);
                stage.process (block.in(), block.out(), block.size());

                for (int i = 0; i < block.size(); ++i)
                {
                    const auto value = block.left[static_cast<std::size_t> (i)];
                    expect (std::isfinite (value) && value >= 0.0f);
                    worst = std::max (worst, std::abs (value - last));
                    last = value;
                }
            }

            logMessage ("rate " + juce::String (rate) + ": worst step " + juce::String (worst) + ", bound "
                        + juce::String (bound));
            expect (worst <= bound);
            expect (worst < 0.02f); // against 1.0 for a hard mute
        }

        beginTest ("Mute and solo changes ramp: a full-scale stem goes silent without a step");

        for (const auto rate : sampleRates)
        {
            StemLevelStage stage;
            stage.prepare (rate);
            stage.setTargets (0.0f, false, 0.0f);

            Stereo block (static_cast<int> (rate * 0.05));
            stage.process (block.in(), block.out(), block.size());

            float worst = 0.0f;
            float last = 1.0f;
            for (const auto v : block.left)
            {
                worst = std::max (worst, std::abs (v - last));
                last = v;
            }

            expect (worst <= 1.1f / (0.010f * static_cast<float> (rate)));
            expectEquals (block.left.back(), 0.0f);
        }

        beginTest ("Output does not depend on the block size");

        StemLevelStage a, b;
        a.prepare (48000.0);
        b.prepare (48000.0);
        a.setTargets (-9.0f, true, 0.7f);
        b.setTargets (-9.0f, true, 0.7f);
        Stereo whole (4000), pieces (4000);
        a.process (whole.in(), whole.out(), whole.size());

        for (int start = 0; start < 4000; start += 1)
        {
            std::array<const float*, 2> in { pieces.left.data() + start, pieces.right.data() + start };
            std::array<float*, 2> out { pieces.left.data() + start, pieces.right.data() + start };
            b.process (in.data(), out.data(), 1);
        }

        expect (whole.left == pieces.left && whole.right == pieces.right);
    }

    void testPositionAndMisc()
    {
        beginTest ("Visual pan position is bounded and follows the smoothed pan");

        StemLevelStage stage;
        stage.prepare (48000.0);
        expectEquals (stage.getPanPosition(), 0.0f);

        stage.setTargets (0.0f, true, 1.0f);
        Stereo half (480); // 10 ms of a 20 ms ramp
        stage.process (half.in(), half.out(), half.size());
        expect (stage.getPanPosition() > 0.3f && stage.getPanPosition() < 0.7f);

        settle (stage, 48000.0);
        expectEquals (stage.getPanPosition(), 1.0f);

        for (const auto wild :
             { 50.0f, -50.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
        {
            stage.setTargets (0.0f, true, wild);
            settle (stage, 48000.0);
            expect (std::abs (stage.getPanPosition()) <= 1.0f);
        }

        beginTest ("Snap jumps to the targets; in-place processing works");

        stage.setTargets (-20.0f, true, -1.0f);
        stage.snapToTargets();
        expectEquals (stage.getPanPosition(), -1.0f);

        Stereo block (8);
        stage.process (block.in(), block.out(), block.size()); // in-place on the same buffers
        expectWithinAbsoluteError (block.left.front(), 0.1f * std::sqrt (2.0f), 1.0e-5f);
        expectEquals (block.right.front(), 0.0f);
    }
};

DspStemLevelTests dspStemLevelTests;
} // namespace
} // namespace snds::dsp
