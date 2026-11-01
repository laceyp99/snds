#include <juce_core/juce_core.h>

#include "dsp/MixParameters.h"
#include "dsp/ParameterStore.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace snds::dsp
{
namespace
{
class DspParameterTests final : public juce::UnitTest
{
public:
    DspParameterTests()
        : juce::UnitTest ("DSP parameters", "snds")
    {
    }

    void runTest() override
    {
        testIds();
        testRangesAndDefaults();
        testNormalisedRoundTrip();
        testHelpers();
        testText();
        testStore();
        testConcurrentSnapshots();
    }

private:
    void testIds()
    {
        beginTest ("IDs are stable, unique and findable");

        expectEquals (numParameters, 38);
        expect (parameterSpec (parameterIndex (Stem::drums, StemControl::gain)).id.view() == "stem.drums.gain");
        expect (parameterSpec (parameterIndex (Stem::vocals, StemControl::delaySend)).id.view()
                == "stem.vocals.delaySend");
        expect (parameterSpec (parameterIndex (Stem::bass, StemControl::lowCut)).id.view() == "stem.bass.lowCut");
        expect (parameterSpec (parameterIndex (Stem::other, StemControl::highCut)).id.view() == "stem.other.highCut");
        expect (parameterSpec (parameterIndex (GlobalControl::reverbReturn)).id.view() == "fx.reverb.return");
        expect (parameterSpec (parameterIndex (GlobalControl::delayDivision)).id.view() == "fx.delay.division");
        expect (parameterSpec (parameterIndex (GlobalControl::delayFeedback)).id.view() == "fx.delay.feedback");
        expect (parameterSpec (parameterIndex (GlobalControl::delayReturn)).id.view() == "fx.delay.return");
        expect (parameterSpec (parameterIndex (GlobalControl::masterGain)).id.view() == "master.gain");
        expect (parameterSpec (parameterIndex (GlobalControl::tempoBpm)).id.view() == "tempo.bpm");

        std::set<std::string> seen;
        for (int i = 0; i < numParameters; ++i)
        {
            const auto id = std::string (parameterSpec (i).id.view());
            expect (! id.empty());
            expect (seen.insert (id).second, "duplicate id " + juce::String (id));
            expect (parameterSpec (i).name.length > 0);

            const auto found = findParameter (id);
            expect (found.has_value() && *found == i);
        }

        expect (! findParameter ("stem.drums.volume").has_value());
        expect (! findParameter ("").has_value());
    }

    void testRangesAndDefaults()
    {
        beginTest ("Ranges and defaults follow the plan");

        for (int s = 0; s < numStems; ++s)
        {
            const auto stem = static_cast<Stem> (s);
            const auto spec = [stem] (StemControl c) -> const ParameterSpec&
            { return parameterSpec (parameterIndex (stem, c)); };

            expectEquals (spec (StemControl::gain).minimum, -60.0f);
            expectEquals (spec (StemControl::gain).maximum, 6.0f);
            expectEquals (spec (StemControl::gain).defaultValue, 0.0f);
            expectEquals (spec (StemControl::pan).minimum, -1.0f);
            expectEquals (spec (StemControl::pan).maximum, 1.0f);
            expectEquals (spec (StemControl::pan).defaultValue, 0.0f);
            expectEquals (spec (StemControl::lowCut).minimum, 20.0f);
            expectEquals (spec (StemControl::lowCut).maximum, 1000.0f);
            expectEquals (spec (StemControl::lowCut).defaultValue, 20.0f);
            expectEquals (spec (StemControl::highCut).minimum, 500.0f);
            expectEquals (spec (StemControl::highCut).maximum, 20000.0f);
            expectEquals (spec (StemControl::highCut).defaultValue, 20000.0f);
            expect (spec (StemControl::lowCut).scale == ParameterScale::logarithmic);
            expect (spec (StemControl::highCut).scale == ParameterScale::logarithmic);

            for (auto c : { StemControl::mute, StemControl::solo })
            {
                expect (spec (c).kind == ParameterKind::toggle);
                expectEquals (spec (c).defaultValue, 0.0f);
            }

            for (auto c : { StemControl::reverbSend, StemControl::delaySend })
            {
                expectEquals (spec (c).minimum, 0.0f);
                expectEquals (spec (c).maximum, 1.0f);
                expectEquals (spec (c).defaultValue, 0.0f);
            }

            expect (isLowCutBypassed (spec (StemControl::lowCut).defaultValue));
            expect (isHighCutBypassed (spec (StemControl::highCut).defaultValue));
            expect (! isLowCutBypassed (21.0f));
            expect (! isHighCutBypassed (19000.0f));
        }

        const auto global = [] (GlobalControl c) -> const ParameterSpec& { return parameterSpec (parameterIndex (c)); };
        expectEquals (global (GlobalControl::masterGain).minimum, -60.0f);
        expectEquals (global (GlobalControl::masterGain).maximum, 6.0f);
        expectEquals (global (GlobalControl::masterGain).defaultValue, 0.0f);
        expectEquals (global (GlobalControl::delayFeedback).maximum, 0.6f);
        expectEquals (global (GlobalControl::reverbReturn).maximum, 1.0f);
        expectEquals (global (GlobalControl::delayReturn).maximum, 1.0f);
        expectEquals (global (GlobalControl::tempoBpm).minimum, 20.0f);
        expectEquals (global (GlobalControl::tempoBpm).maximum, 400.0f);
        expect (global (GlobalControl::delayDivision).kind == ParameterKind::choice);
        expectEquals (global (GlobalControl::delayDivision).maximum, 4.0f);

        for (int i = 0; i < numParameters; ++i)
        {
            const auto& spec = parameterSpec (i);
            expect (spec.minimum < spec.maximum);
            expect (spec.defaultValue >= spec.minimum && spec.defaultValue <= spec.maximum);
            expect (spec.smoothingSeconds >= 0.0f && spec.smoothingSeconds <= 0.1f);
            expectEquals (clampParameterValue (i, spec.defaultValue), spec.defaultValue);
        }

        // Audible continuous controls are smoothed; tempo/division are handled by the delay.
        expect (parameterSpec (parameterIndex (Stem::drums, StemControl::gain)).smoothingSeconds > 0.0f);
        expect (parameterSpec (parameterIndex (Stem::drums, StemControl::mute)).smoothingSeconds > 0.0f);
        expect (parameterSpec (parameterIndex (Stem::drums, StemControl::lowCut)).smoothingSeconds > 0.0f);
        expectEquals (parameterSpec (parameterIndex (GlobalControl::tempoBpm)).smoothingSeconds, 0.0f);

        beginTest ("Clamping sanitises every input");
        const auto gain = parameterIndex (Stem::drums, StemControl::gain);
        expectEquals (clampParameterValue (gain, 100.0f), 6.0f);
        expectEquals (clampParameterValue (gain, -1000.0f), -60.0f);
        expectEquals (clampParameterValue (gain, std::numeric_limits<float>::quiet_NaN()), 0.0f);
        expectEquals (clampParameterValue (gain, std::numeric_limits<float>::infinity()), 0.0f);
        expectEquals (clampParameterValue (parameterIndex (GlobalControl::delayFeedback), 2.0f), 0.6f);
        expectEquals (clampParameterValue (parameterIndex (GlobalControl::delayFeedback), -1.0f), 0.0f);
        expectEquals (clampParameterValue (parameterIndex (Stem::bass, StemControl::mute), 0.7f), 1.0f);
        expectEquals (clampParameterValue (parameterIndex (Stem::bass, StemControl::solo), 0.3f), 0.0f);
        expectEquals (clampParameterValue (parameterIndex (GlobalControl::delayDivision), 2.6f), 3.0f);
        expectEquals (clampParameterValue (parameterIndex (GlobalControl::tempoBpm), 1000.0f), 400.0f);
        expectEquals (clampParameterValue (parameterIndex (GlobalControl::tempoBpm), 1.0f), 20.0f);
    }

    void testNormalisedRoundTrip()
    {
        beginTest ("Normalised mapping round-trips and is monotonic");

        for (int i = 0; i < numParameters; ++i)
        {
            const auto& spec = parameterSpec (i);
            expectWithinAbsoluteError (toNormalised (i, spec.minimum), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (toNormalised (i, spec.maximum), 1.0f, 1.0e-6f);

            if (spec.kind == ParameterKind::continuous)
            {
                float previous = -1.0f;
                for (int step = 0; step <= 20; ++step)
                {
                    const auto normalised = static_cast<float> (step) / 20.0f;
                    const auto value = fromNormalised (i, normalised);
                    expect (toNormalised (i, value) >= previous);
                    expectWithinAbsoluteError (toNormalised (i, value), normalised, 1.0e-4f);
                    previous = toNormalised (i, value);
                }
            }
        }

        // Log scale: the geometric middle of 20 Hz..1 kHz is the 50 % point.
        const auto low = parameterIndex (Stem::drums, StemControl::lowCut);
        expectWithinAbsoluteError (fromNormalised (low, 0.5f), std::sqrt (20.0f * 1000.0f), 0.01f);
        expectEquals (fromNormalised (low, std::numeric_limits<float>::quiet_NaN()), 20.0f);
    }

    void testHelpers()
    {
        beginTest ("Gain, tempo and delay-division helpers");

        expectEquals (decibelsToGain (-60.0f), 0.0f);
        expectEquals (decibelsToGain (-100.0f), 0.0f);
        expectWithinAbsoluteError (decibelsToGain (0.0f), 1.0f, 1.0e-6f);
        expectWithinAbsoluteError (decibelsToGain (6.0f), 1.9953f, 1.0e-3f);
        expectWithinAbsoluteError (decibelsToGain (-6.0f), 0.5012f, 1.0e-3f);
        expectEquals (gainToDecibels (0.0f), -60.0f);
        expectWithinAbsoluteError (gainToDecibels (1.0f), 0.0f, 1.0e-5f);

        expectEquals (halveTempo (120.0f), 60.0f);
        expectEquals (doubleTempo (120.0f), 240.0f);
        expectEquals (halveTempo (30.0f), 20.0f);
        expectEquals (doubleTempo (300.0f), 400.0f);

        expectWithinAbsoluteError (delayTimeSeconds (120.0, DelayDivision::quarter), 0.5, 1.0e-9);
        expectWithinAbsoluteError (delayTimeSeconds (120.0, DelayDivision::eighth), 0.25, 1.0e-9);
        expectWithinAbsoluteError (delayTimeSeconds (120.0, DelayDivision::dottedEighth), 0.375, 1.0e-9);
        expectWithinAbsoluteError (delayTimeSeconds (120.0, DelayDivision::eighthTriplet), 1.0 / 6.0, 1.0e-6);
        expectWithinAbsoluteError (delayTimeSeconds (120.0, DelayDivision::sixteenth), 0.125, 1.0e-9);
        expectWithinAbsoluteError (maxDelayTimeSeconds(), 3.0, 1.0e-9);

        for (int d = 0; d < numDelayDivisions; ++d)
            expect (delayTimeSeconds (20.0, static_cast<DelayDivision> (d)) <= maxDelayTimeSeconds());
    }

    void testText()
    {
        beginTest ("Display and exact text, and parsing");

        const auto gain = parameterIndex (Stem::drums, StemControl::gain);
        expectEquals (formatDisplay (gain, -3.0f), juce::String ("-3.0 dB"));
        expectEquals (formatDisplay (gain, -60.0f), juce::String ("-inf dB"));
        expectEquals (formatExact (gain, -3.25f), juce::String ("-3.25 dB"));

        const auto low = parameterIndex (Stem::bass, StemControl::lowCut);
        const auto high = parameterIndex (Stem::bass, StemControl::highCut);
        expectEquals (formatDisplay (low, 20.0f), juce::String ("Off"));
        expectEquals (formatDisplay (low, 250.0f), juce::String ("250 Hz"));
        expectEquals (formatDisplay (high, 20000.0f), juce::String ("Off"));
        expectEquals (formatDisplay (high, 1200.0f), juce::String ("1.2 kHz"));
        expectEquals (formatExact (high, 1203.5f), juce::String ("1.204 kHz"));

        const auto pan = parameterIndex (Stem::other, StemControl::pan);
        expectEquals (formatDisplay (pan, 0.0f), juce::String ("Center"));
        expectEquals (formatDisplay (pan, -0.3f), juce::String ("L 30"));
        expectEquals (formatDisplay (pan, 1.0f), juce::String ("R 100"));

        const auto send = parameterIndex (Stem::vocals, StemControl::reverbSend);
        expectEquals (formatDisplay (send, 0.35f), juce::String ("35%"));
        expectEquals (formatExact (send, 0.355f), juce::String ("35.5%"));

        const auto division = parameterIndex (GlobalControl::delayDivision);
        expectEquals (formatDisplay (division, 2.0f), juce::String ("Dotted 1/8"));
        expectEquals (formatDisplay (division, 3.0f), juce::String ("1/8T"));
        expectEquals (formatDisplay (parameterIndex (GlobalControl::tempoBpm), 128.0f), juce::String ("128.0 BPM"));
        expectEquals (formatDisplay (parameterIndex (Stem::drums, StemControl::mute), 1.0f), juce::String ("On"));

        // Parsing typed values.
        expectEquals (parseValue (gain, "-6").value_or (99.0f), -6.0f);
        expectEquals (parseValue (gain, " -6.5 dB ").value_or (99.0f), -6.5f);
        expectEquals (parseValue (gain, "-inf").value_or (99.0f), -60.0f);
        expectEquals (parseValue (gain, "40").value_or (99.0f), 6.0f); // clamped
        expect (! parseValue (gain, "loud").has_value());
        expect (! parseValue (gain, "").has_value());
        expectEquals (parseValue (high, "1.5k").value_or (0.0f), 1500.0f);
        expectEquals (parseValue (high, "800 Hz").value_or (0.0f), 800.0f);
        expectEquals (parseValue (high, "off").value_or (0.0f), 20000.0f);
        expectEquals (parseValue (low, "off").value_or (0.0f), 20.0f);
        expectWithinAbsoluteError (parseValue (send, "35%").value_or (0.0f), 0.35f, 1.0e-6f);
        expectWithinAbsoluteError (parseValue (pan, "L30").value_or (0.0f), -0.3f, 1.0e-6f);
        expectWithinAbsoluteError (parseValue (pan, "r 100").value_or (0.0f), 1.0f, 1.0e-6f);
        expectEquals (parseValue (pan, "center").value_or (9.0f), 0.0f);
        expectEquals (parseValue (division, "1/8T").value_or (-1.0f), 3.0f);
        expectEquals (parseValue (division, "dotted 1/8").value_or (-1.0f), 2.0f);
        expectEquals (parseValue (parameterIndex (GlobalControl::tempoBpm), "128 bpm").value_or (0.0f), 128.0f);

        beginTest ("Exact text parses back to the value (within the displayed precision)");

        for (int i = 0; i < numParameters; ++i)
        {
            const auto& spec = parameterSpec (i);
            for (int step = 0; step <= 10; ++step)
            {
                const auto value = fromNormalised (i, static_cast<float> (step) / 10.0f);
                const auto text = formatExact (i, value);
                const auto parsed = parseValue (i, text);
                expect (parsed.has_value(),
                        "cannot parse '" + text + "' for " + juce::String (std::string (spec.id.view())));

                if (parsed.has_value())
                {
                    const auto tolerance = std::max (0.0006f * std::abs (value), 0.006f);
                    expectWithinAbsoluteError (
                        *parsed, value, std::max (tolerance, spec.unit == ParameterUnit::hertz ? 0.3f : 0.0f));
                }
            }
        }
    }

    void testStore()
    {
        beginTest ("Store starts at defaults and reads them back");

        ParameterStore store;
        ParameterSnapshot snapshot;

        expect (store.read (snapshot) == ParameterStore::ReadResult::unchanged);
        expectEquals (store.version(), 0u);

        for (int i = 0; i < numParameters; ++i)
        {
            expectEquals (snapshot[i], parameterSpec (i).defaultValue);
            expectEquals (store.get (i), parameterSpec (i).defaultValue);
        }

        beginTest ("Set clamps, reports changes, and the reader sees updates once");

        const auto drumsGain = parameterIndex (Stem::drums, StemControl::gain);
        expect (store.set (Stem::drums, StemControl::gain, -12.0f));
        expect (! store.set (Stem::drums, StemControl::gain, -12.0f));
        expect (store.read (snapshot) == ParameterStore::ReadResult::updated);
        expectEquals (snapshot[drumsGain], -12.0f);
        expectEquals (snapshot.get (Stem::drums, StemControl::gain), -12.0f);
        expect (store.read (snapshot) == ParameterStore::ReadResult::unchanged);

        expect (store.set (GlobalControl::delayFeedback, 5.0f));
        expectEquals (store.get (parameterIndex (GlobalControl::delayFeedback)), 0.6f);
        expect (store.set (GlobalControl::tempoBpm, std::numeric_limits<float>::quiet_NaN()) == false);
        expectEquals (store.get (parameterIndex (GlobalControl::tempoBpm)), 120.0f);

        beginTest ("Round trip: every parameter keeps an exact value");

        for (int i = 0; i < numParameters; ++i)
        {
            const auto value = fromNormalised (i, 0.37f);
            store.set (i, value);
            expectEquals (store.get (i), value);
        }

        store.read (snapshot);
        for (int i = 0; i < numParameters; ++i)
            expectEquals (snapshot[i], fromNormalised (i, 0.37f));

        beginTest ("Reset restores defaults and is a no-op on defaults");

        store.resetToDefaults();
        expect (store.read (snapshot) == ParameterStore::ReadResult::updated);
        for (int i = 0; i < numParameters; ++i)
            expectEquals (snapshot[i], parameterSpec (i).defaultValue);

        const auto versionAtDefaults = store.version();
        store.resetToDefaults();
        expectEquals (store.version(), versionAtDefaults);

        store.set (drumsGain, 3.0f);
        expect (store.resetToDefault (drumsGain));
        expectEquals (store.get (drumsGain), 0.0f);

        beginTest ("setMany is one transaction");

        const auto before = store.version();
        const std::pair<int, float> changes[] { { drumsGain, -6.0f },
                                                { parameterIndex (Stem::bass, StemControl::mute), 1.0f } };
        store.setMany (changes);
        expectEquals (store.version(), before + 1);
    }

    void testConcurrentSnapshots()
    {
        beginTest ("Snapshots stay consistent while another thread writes");

        // Each transaction writes a related set of values; a reader must never see a mixture.
        const int gains[] = { parameterIndex (Stem::drums, StemControl::gain),
                              parameterIndex (Stem::bass, StemControl::gain),
                              parameterIndex (Stem::other, StemControl::gain),
                              parameterIndex (Stem::vocals, StemControl::gain) };
        const auto pan = parameterIndex (Stem::drums, StemControl::pan);
        const auto master = parameterIndex (GlobalControl::masterGain);

        ParameterStore store;
        std::atomic<bool> stop { false };
        std::atomic<long> writes { 0 };

        const auto writer = [&]
        {
            long n = 0;
            while (! stop.load())
            {
                const auto g = static_cast<float> (n % 66) - 60.0f; // -60..+5
                const std::pair<int, float> changes[] { { gains[0], g }, { gains[1], g },    { gains[2], g },
                                                        { gains[3], g }, { pan, g / 60.0f }, { master, g } };
                store.setMany (changes);
                writes = ++n;
            }
        };

        std::thread writerThread (writer);
        std::thread secondWriter (writer); // two writers: transactions still must not interleave

        ParameterSnapshot snapshot;
        long reads = 0, updates = 0, incoherent = 0, outOfRange = 0, contended = 0;
        const auto end = juce::Time::getMillisecondCounter() + 400;

        while (juce::Time::getMillisecondCounter() < end)
        {
            const auto result = store.read (snapshot);
            ++reads;
            contended += result == ParameterStore::ReadResult::contended;
            updates += result == ParameterStore::ReadResult::updated;

            const auto g = snapshot[gains[0]];
            if (snapshot[gains[1]] != g || snapshot[gains[2]] != g || snapshot[gains[3]] != g || snapshot[master] != g
                || snapshot[pan] != g / 60.0f)
                ++incoherent;

            for (int i = 0; i < numParameters; ++i)
                outOfRange += (snapshot[i] < parameterSpec (i).minimum || snapshot[i] > parameterSpec (i).maximum);
        }

        stop = true;
        writerThread.join();
        secondWriter.join();

        logMessage ("reads " + juce::String (reads) + ", updates " + juce::String (updates) + ", contended "
                    + juce::String (contended) + ", writes " + juce::String (writes.load()));
        expect (updates > 0 && writes.load() > 0);
        expectEquals (incoherent, 0L);
        expectEquals (outOfRange, 0L);
    }
};

DspParameterTests dspParameterTests;
} // namespace
} // namespace snds::dsp
