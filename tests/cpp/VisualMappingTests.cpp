#include <juce_core/juce_core.h>

#include "rendering/VisualController.h"
#include "rendering/VisualMapping.h"

#include <cmath>
#include <memory>

namespace snds::rendering
{
namespace
{
dsp::VisualSnapshot makeSnapshot (float pulse = 0.0f, float bass = 0.0f, float vocals = 0.0f, float other = 0.0f)
{
    dsp::VisualSnapshot s;
    s.drumsPulse = pulse;
    s.bassDepth = bass;
    s.vocalGlow = vocals;
    s.otherTexture = other;
    return s;
}

// A renderer that only remembers the last effects it was given.
struct Seen
{
    EffectParams effects;
    int effectCalls = 0;
};

class RecordingRenderer final : public VisualRenderer
{
public:
    RecordingRenderer (RenderTier t, std::shared_ptr<Seen> s)
        : fakeTier (t),
          seen (std::move (s))
    {
    }

    RenderTier tier() const noexcept override { return fakeTier; }
    AttachResult attach (juce::Component&) override { return {}; }
    void detach() override {}
    void setArtwork (const ArtworkInput&) override {}
    void setEffects (const EffectParams& e) override
    {
        seen->effects = e;
        ++seen->effectCalls;
    }
    void paint (juce::Graphics&) override {}
    void tick() override {}
    RendererStatus status() const override { return {}; }
    void setFailureCallback (FailureCallback) override {}

    RenderTier fakeTier;
    std::shared_ptr<Seen> seen;
};

bool differsOnlyIn (
    const EffectParams& a, const EffectParams& b, bool scale, bool depth, bool glow, bool texture, bool pan)
{
    return (scale || a.scale == b.scale) && (depth || a.depth == b.depth) && (glow || a.glow == b.glow)
        && (texture || a.texture == b.texture)
        && (pan
            || (a.leftBrightness == b.leftBrightness && a.rightBrightness == b.rightBrightness && a.shift == b.shift))
        && a.brightness == b.brightness;
}

class VisualMappingTests final : public juce::UnitTest
{
public:
    VisualMappingTests()
        : juce::UnitTest ("Visual mapping", "snds")
    {
    }

    void runTest() override
    {
        testNeutralCases();
        testRoles();
        testPan();
        testIntensityAndBounds();
        testEasing();
        testMapperStates();
        testControllerAndHost();
    }

private:
    void testNeutralCases()
    {
        beginTest ("Silence, intensity 0, effects off and static mode all give exactly the untouched artwork");

        const VisualSettings on;
        expect (targetEffects (dsp::VisualSnapshot(), on).isNeutral());

        auto loud = makeSnapshot (1.0f, 1.0f, 1.0f, 1.0f);
        loud.pan = { -1.0f, -0.5f, 0.5f, 1.0f };
        loud.level = { 1.0f, 1.0f, 1.0f, 1.0f };
        expect (! targetEffects (loud, on).isNeutral());

        VisualSettings s = on;
        s.intensity = 0.0f;
        expect (targetEffects (loud, s).isNeutral());

        s = on;
        s.effectsEnabled = false;
        expect (targetEffects (loud, s).isNeutral());

        s = on;
        s.staticMode = true;
        expect (targetEffects (loud, s).isNeutral());

        s.effectsEnabled = false;
        s.intensity = 1.0f;
        expect (targetEffects (loud, s).isNeutral()); // static wins, whatever else is stored

        expectEquals (defaultVisualIntensity, 0.5f);
        expectEquals (VisualSettings().intensity, defaultVisualIntensity);
        expect (VisualSettings().effectsEnabled && ! VisualSettings().staticMode);
    }

    void testRoles()
    {
        beginTest ("Each stem role drives only its own effect, and more of it drives more");

        VisualSettings s;
        s.intensity = 1.0f;
        const EffectParams neutral;

        const auto pulse = targetEffects (makeSnapshot (1.0f), s);
        expect (pulse.scale > 1.0f && differsOnlyIn (pulse, neutral, true, false, false, false, false));

        const auto bass = targetEffects (makeSnapshot (0.0f, 1.0f), s);
        expect (bass.depth > 0.0f && differsOnlyIn (bass, neutral, false, true, false, false, false));

        const auto glow = targetEffects (makeSnapshot (0.0f, 0.0f, 1.0f), s);
        expect (glow.glow > 0.0f && differsOnlyIn (glow, neutral, false, false, true, false, false));

        const auto texture = targetEffects (makeSnapshot (0.0f, 0.0f, 0.0f, 1.0f), s);
        expect (texture.texture > 0.0f && differsOnlyIn (texture, neutral, false, false, false, true, false));

        // Monotonic in the analysis value.
        float last[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int step = 1; step <= 10; ++step)
        {
            const auto v = static_cast<float> (step) / 10.0f;
            const auto e = std::array { targetEffects (makeSnapshot (v), s).scale - 1.0f,
                                        targetEffects (makeSnapshot (0, v), s).depth,
                                        targetEffects (makeSnapshot (0, 0, v), s).glow,
                                        targetEffects (makeSnapshot (0, 0, 0, v), s).texture };
            for (std::size_t i = 0; i < 4; ++i)
            {
                expect (e[i] > last[i]);
                last[i] = e[i];
            }
        }

        // The mix action is the cause: the same snapshot always gives the same parameters.
        const auto again = targetEffects (makeSnapshot (0.3f, 0.6f, 0.2f, 0.9f), s);
        expect (again == targetEffects (makeSnapshot (0.3f, 0.6f, 0.2f, 0.9f), s));
    }

    void testPan()
    {
        beginTest ("Pan pushes brightness and drift towards the panned side, weighted by level");

        VisualSettings s;
        s.intensity = 1.0f;

        auto make = [] (std::array<float, 4> pan, std::array<float, 4> level)
        {
            dsp::VisualSnapshot snapshot;
            snapshot.pan = pan;
            snapshot.level = level;
            return snapshot;
        };

        const auto left = targetEffects (make ({ -1.0f, 0, 0, 0 }, { 1.0f, 0, 0, 0 }), s);
        expect (left.leftBrightness > 0.5f && left.rightBrightness == 0.0f && left.shift < 0.0f);
        expect (differsOnlyIn (left, EffectParams(), false, false, false, false, true));

        const auto right = targetEffects (make ({ 0, 0, 0, 1.0f }, { 0, 0, 0, 1.0f }), s);
        expect (right.rightBrightness > 0.5f && right.leftBrightness == 0.0f && right.shift > 0.0f);
        expectWithinAbsoluteError (right.rightBrightness, left.leftBrightness, 1.0e-6f);
        expectWithinAbsoluteError (right.shift, -left.shift, 1.0e-6f);

        // Centred, or silent, adds nothing: a muted stem has level 0 however far it is panned.
        expect (targetEffects (make ({ 0, 0, 0, 0 }, { 1.0f, 1.0f, 1.0f, 1.0f }), s).isNeutral());
        expect (targetEffects (make ({ -1.0f, -1.0f, 1.0f, 1.0f }, { 0, 0, 0, 0 }), s).isNeutral());

        // Louder or further out is brighter; opposite pans cancel the drift.
        const auto quiet = targetEffects (make ({ -1.0f, 0, 0, 0 }, { 0.3f, 0, 0, 0 }), s);
        const auto near = targetEffects (make ({ -0.4f, 0, 0, 0 }, { 1.0f, 0, 0, 0 }), s);
        expect (quiet.leftBrightness < left.leftBrightness && near.leftBrightness < left.leftBrightness);
        const auto both = targetEffects (make ({ -1.0f, 0, 0, 1.0f }, { 1.0f, 0, 0, 1.0f }), s);
        expect (both.leftBrightness > 0.5f && both.rightBrightness > 0.5f);
        expectWithinAbsoluteError (both.shift, 0.0f, 1.0e-6f);

        // Several stems on one side stack, but saturate below the limit.
        const auto stacked = targetEffects (make ({ -1.0f, -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }), s);
        expect (stacked.leftBrightness > left.leftBrightness && stacked.leftBrightness <= limits::side);
    }

    void testIntensityAndBounds()
    {
        beginTest ("Intensity scales every effect, and the artwork stays recognisable at the maximum");

        auto full = makeSnapshot (1.0f, 1.0f, 1.0f, 1.0f);
        full.pan = { -1.0f, -1.0f, -1.0f, -1.0f };
        full.level = { 1.0f, 1.0f, 1.0f, 1.0f };

        EffectParams previous;
        for (int step = 1; step <= 10; ++step)
        {
            VisualSettings s;
            s.intensity = static_cast<float> (step) / 10.0f;
            const auto e = targetEffects (full, s);
            expect (e.scale > previous.scale && e.depth > previous.depth && e.glow > previous.glow
                    && e.texture > previous.texture && e.leftBrightness > previous.leftBrightness);
            previous = e;
        }

        VisualSettings maximum;
        maximum.intensity = 1.0f;
        const auto top = targetEffects (full, maximum);
        expectWithinAbsoluteError (top.scale, 1.05f, 1.0e-6f); // never more than +5 % zoom
        expect (top.depth <= limits::depth && top.glow <= 0.8f + 1.0e-6f && top.texture <= 0.6f + 1.0e-6f);
        expect (top.leftBrightness <= 1.0f && std::abs (top.shift) <= 1.0f && top.brightness == 1.0f);

        // The default is conservative: a half-strength version of the maximum.
        const auto standard = targetEffects (full, VisualSettings());
        expectWithinAbsoluteError (standard.scale - 1.0f, (top.scale - 1.0f) * 0.5f, 1.0e-6f);
        expectWithinAbsoluteError (standard.glow, top.glow * 0.5f, 1.0e-6f);

        // Out-of-range intensity and hostile analysis values stay in bounds.
        VisualSettings wild;
        wild.intensity = 7.0f;
        expect (targetEffects (full, wild) == top);
        wild.intensity = -3.0f;
        expect (targetEffects (full, wild).isNeutral());
        wild.intensity = std::numeric_limits<float>::quiet_NaN();
        expect (targetEffects (full, wild).isNeutral());

        auto hostile =
            makeSnapshot (9.0f, -2.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
        hostile.pan = { 40.0f, std::numeric_limits<float>::quiet_NaN(), -40.0f, 0.0f };
        hostile.level = { 5.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f };
        const auto bounded = targetEffects (hostile, maximum);
        expect (bounded.scale >= 1.0f && bounded.scale <= 1.05f + 1.0e-6f);
        expect (bounded.depth == 0.0f && bounded.texture == 0.0f);
        expect (std::isfinite (bounded.glow) && bounded.glow <= 0.8f + 1.0e-6f);
        expect (std::isfinite (bounded.shift) && std::abs (bounded.shift) <= 1.0f);

        juce::Random random (4);
        bool inRange = true;
        for (int i = 0; i < 5000; ++i)
        {
            dsp::VisualSnapshot s =
                makeSnapshot (random.nextFloat(), random.nextFloat(), random.nextFloat(), random.nextFloat());
            for (std::size_t k = 0; k < 4; ++k)
            {
                s.pan[k] = random.nextFloat() * 2.0f - 1.0f;
                s.level[k] = random.nextFloat();
            }
            VisualSettings v;
            v.intensity = random.nextFloat();
            const auto e = targetEffects (s, v);
            inRange = inRange && e.scale >= 1.0f && e.scale <= 1.05f + 1.0e-6f && e.depth >= 0.0f && e.depth <= 1.0f
                   && e.glow >= 0.0f && e.glow <= 0.8f + 1.0e-6f && e.texture >= 0.0f && e.texture <= 0.6f + 1.0e-6f
                   && e.leftBrightness >= 0.0f && e.leftBrightness <= 1.0f && e.rightBrightness >= 0.0f
                   && e.rightBrightness <= 1.0f && std::abs (e.shift) <= 1.0f;
        }
        expect (inRange);
    }

    void testEasing()
    {
        beginTest ("Easing follows time, not frame rate; rising is faster than falling; it lands exactly");

        const EasingTimes times { 0.05f, 0.25f };

        // After t seconds the value is 1 - exp(-t / tau), whatever the frame rate.
        for (const auto fps : { 30.0, 60.0, 144.0 })
        {
            float value = 0.0f;
            const auto frames = static_cast<int> (std::lround (fps * 0.05));
            for (int i = 0; i < frames; ++i)
                value = easeValue (value, 1.0f, 0.0f, times, 1.0 / fps);
            expectWithinAbsoluteError (
                value, 1.0f - std::exp (-static_cast<float> (frames / fps) / 0.05f), 1.0e-4f); // whole frames only
        }

        auto timeToHalf = [&] (float from, float to)
        {
            float value = from;
            int frames = 0;
            while (std::abs (value - to) > std::abs (from - to) * 0.5f && frames < 100000)
            {
                value = easeValue (value, to, 0.0f, times, 1.0 / 240.0);
                ++frames;
            }
            return frames;
        };
        expect (timeToHalf (0.0f, 1.0f) < timeToHalf (1.0f, 0.0f)); // up is quicker than down

        // No overshoot, ever, even with a huge step; and it reaches the target exactly.
        float value = 0.0f;
        value = easeValue (value, 1.0f, 0.0f, times, 100.0);
        expect (value <= 1.0f && value > 0.9f); // a long frame is clamped, not a jump
        for (int i = 0; i < 2000; ++i)
        {
            value = easeValue (value, 1.0f, 0.0f, times, 1.0 / 60.0);
            expect (value <= 1.0f);
        }
        expectEquals (value, 1.0f);

        expectEquals (easeValue (0.4f, 0.8f, 0.0f, times, 0.0), 0.4f);  // no time, no movement
        expectEquals (easeValue (0.4f, 0.8f, 0.0f, times, -1.0), 0.4f); // time never runs backwards
        expectEquals (easeValue (0.4f, 0.8f, 0.0f, times, std::numeric_limits<double>::quiet_NaN()), 0.4f);

        // Signed values rise away from neutral on either side.
        const auto leftward = easeValue (0.0f, -1.0f, 0.0f, times, 1.0 / 60.0);
        const auto rightward = easeValue (0.0f, 1.0f, 0.0f, times, 1.0 / 60.0);
        expectWithinAbsoluteError (leftward, -rightward, 1.0e-7f);
    }

    void testMapperStates()
    {
        beginTest ("The mapper eases in and out, reaches neutral exactly, and static mode is immediate");

        VisualMapper mapper;
        const VisualSettings on;
        const auto hit = makeSnapshot (1.0f, 1.0f, 1.0f, 1.0f);

        expect (mapper.update (dsp::VisualSnapshot(), on, 1.0 / 60.0).isNeutral());

        const auto first = mapper.update (hit, on, 1.0 / 60.0);
        const auto target = targetEffects (hit, on);
        expect (first.scale > 1.0f && first.scale < target.scale); // moving, not jumping
        expect (first.glow > 0.0f && first.glow < target.glow);

        EffectParams settled;
        for (int i = 0; i < 600; ++i)
            settled = mapper.update (hit, on, 1.0 / 60.0);
        expect (settled == target);

        // Effects off: the picture eases back to the plain artwork, exactly, not forever.
        VisualSettings off = on;
        off.effectsEnabled = false;
        auto fading = mapper.update (hit, off, 1.0 / 60.0);
        expect (fading.scale < settled.scale && fading.scale > 1.0f);
        for (int i = 0; i < 300; ++i)
            fading = mapper.update (hit, off, 1.0 / 60.0);
        expect (fading.isNeutral());

        // Back on, then static mode: neutral at once, with no tail, even mid-pulse.
        for (int i = 0; i < 120; ++i)
            mapper.update (hit, on, 1.0 / 60.0);
        expect (! mapper.current().isNeutral());
        VisualSettings fixed = on;
        fixed.staticMode = true;
        expect (mapper.update (hit, fixed, 1.0 / 60.0).isNeutral());
        expect (mapper.current().isNeutral());

        // Leaving static mode starts again from neutral and eases in (no jump back to the old value).
        const auto resumed = mapper.update (hit, on, 1.0 / 60.0);
        expect (resumed.scale < target.scale && resumed.scale > 1.0f);

        // Determinism: the same sequence of frames always gives the same pictures.
        auto run = [&]
        {
            VisualMapper m;
            std::vector<EffectParams> frames;
            juce::Random random (11);
            for (int i = 0; i < 300; ++i)
            {
                auto s = makeSnapshot (random.nextFloat(), random.nextFloat(), random.nextFloat(), random.nextFloat());
                s.pan = { random.nextFloat() - 0.5f, 0.0f, 0.4f, -0.9f };
                s.level = { 0.5f, 0.2f, random.nextFloat(), 1.0f };
                frames.push_back (m.update (s, on, 0.004 + 0.016 * random.nextFloat()));
            }
            return frames;
        };
        expect (run() == run());

        mapper.reset();
        expect (mapper.current().isNeutral());
    }

    void testControllerAndHost()
    {
        beginTest ("Original mode forces static artwork through the host, whatever the settings and analysis say");

        auto seen = std::make_shared<Seen>();
        VisualHost host ({},
                         [seen] (RenderTier tier) -> std::unique_ptr<VisualRenderer>
                         { return std::make_unique<RecordingRenderer> (tier, seen); });
        host.start();
        expect (host.tier() == RenderTier::gl);

        VisualController controller (host);
        auto loud = makeSnapshot (1.0f, 1.0f, 1.0f, 1.0f);
        loud.pan = { -1.0f, 0.0f, 0.0f, 1.0f };
        loud.level = { 1.0f, 1.0f, 1.0f, 1.0f };

        double now = 1000.0;
        for (int i = 0; i < 120; ++i)
            controller.update (loud, now += 16.0);
        expect (! seen->effects.isNeutral());
        const auto moving = seen->effects;

        controller.setOriginalSelected (true);
        expect (host.isStaticMode());
        expect (seen->effects.isNeutral()); // immediately, no easing tail

        // Everything else changes while the Original plays: still the plain artwork.
        controller.setIntensity (1.0f);
        controller.setEffectsEnabled (true);
        for (int i = 0; i < 60; ++i)
        {
            controller.update (loud, now += 16.0);
            expect (seen->effects.isNeutral());
        }

        // Back to the Stem Mix: the effects ease in again from the plain artwork.
        controller.setOriginalSelected (false);
        expect (! host.isStaticMode());
        expect (seen->effects.isNeutral());
        controller.update (loud, now += 16.0);
        expect (! seen->effects.isNeutral() && seen->effects.scale < moving.scale + 0.05f);

        // Intensity and effects-on are independent controls.
        controller.setEffectsEnabled (false);
        for (int i = 0; i < 200; ++i)
            controller.update (loud, now += 16.0);
        expect (seen->effects.isNeutral());
        expectEquals (controller.getSettings().intensity, 1.0f); // kept while off
        controller.setEffectsEnabled (true);
        for (int i = 0; i < 200; ++i)
            controller.update (loud, now += 16.0);
        expectWithinAbsoluteError (seen->effects.scale, 1.05f, 1.0e-4f); // at the stored intensity

        controller.setIntensity (2.0f);
        expectEquals (controller.getSettings().intensity, 1.0f);
        controller.setIntensity (-1.0f);
        expectEquals (controller.getSettings().intensity, 0.0f);

        // A new renderer after a fallback is told about static mode too.
        controller.setOriginalSelected (true);
        host.stop();
        host.start();
        expect (seen->effects.isNeutral());

        // The store path: the snapshot comes from the audio thread's lock-free store.
        dsp::VisualStore store;
        controller.setOriginalSelected (false);
        controller.setIntensity (1.0f);
        dsp::VisualSnapshot published = loud;
        store.publish (published);
        for (int i = 0; i < 200; ++i)
            controller.update (store, now += 16.0);
        expectWithinAbsoluteError (seen->effects.scale, 1.05f, 1.0e-4f);

        controller.reset();
        expect (seen->effects.isNeutral());
    }
};

VisualMappingTests visualMappingTests;
} // namespace
} // namespace snds::rendering
