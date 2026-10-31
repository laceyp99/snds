#include <juce_core/juce_core.h>

#include "rendering/GlCapability.h"
#include "rendering/RenderOverrides.h"
#include "rendering/StaticRenderer.h"

namespace snds::rendering
{
namespace
{
class RenderCapabilityTests final : public juce::UnitTest
{
public:
    RenderCapabilityTests()
        : juce::UnitTest ("Render capability", "snds")
    {
    }

    void runTest() override
    {
        testParsing();
        testEvaluation();
        testOverrides();
        testPaletteAndFit();
    }

private:
    static GpuInfo infoFor (const juce::String& version, const juce::String& glsl, bool created = true)
    {
        GpuInfo info;
        info.contextCreated = created;
        info.version = version;
        info.glslVersion = glsl;

        if (const auto parsed = parseGlVersion (version))
        {
            info.major = parsed->first;
            info.minor = parsed->second;
        }

        info.glslNumber = parseGlslVersion (glsl);
        return info;
    }

    void testParsing()
    {
        beginTest ("Driver version strings parse (the ones the spike saw, and some old ones)");

        const auto version = [] (const char* text) { return parseGlVersion (text); };
        expect (version ("3.2.0 - Build 32.0.101.6733") == std::pair (3, 2));
        expect (version ("4.6.0 - Build 32.0.101.6733") == std::pair (4, 6));
        expect (version ("3.2.0 NVIDIA 560.94") == std::pair (3, 2));
        expect (version ("4.1 Metal - 83.1") == std::pair (4, 1));
        expect (version ("1.1.0") == std::pair (1, 1));
        expect (version ("OpenGL ES 3.0 Mesa 20") == std::pair (3, 0));
        expect (version ("2 3.3") == std::pair (3, 3));
        expect (! version ("").has_value());
        expect (! version ("garbage").has_value());

        expectWithinAbsoluteError (parseGlslVersion ("1.50 - Build 32.0.101.6733"), 1.50, 1.0e-9);
        expectWithinAbsoluteError (parseGlslVersion ("1.50 NVIDIA via Cg compiler"), 1.50, 1.0e-9);
        expectWithinAbsoluteError (parseGlslVersion ("4.60 - Build 32.0.101.6733"), 4.60, 1.0e-9);
        expectWithinAbsoluteError (parseGlslVersion ("1.5"), 1.50, 1.0e-9);
        expectWithinAbsoluteError (parseGlslVersion ("1.20"), 1.20, 1.0e-9);
        expectEquals (parseGlslVersion (""), 0.0);

        expectEquals (glslVersionFor (3, 2), 1.50);
        expectEquals (glslVersionFor (3, 1), 1.40);
        expectEquals (glslVersionFor (2, 1), 1.20);
        expectEquals (glslVersionFor (4, 3), 4.3);
    }

    void testEvaluation()
    {
        beginTest ("The minimum is OpenGL 3.2 with GLSL 1.50, checked on what the driver reports");

        const GlRequirement minimum;
        expectEquals (minimum.major, 3);
        expectEquals (minimum.minor, 2);
        expectEquals (minimum.glsl, 1.50);

        auto check = [&] (const char* version, const char* glsl, bool expected)
        {
            auto info = infoFor (version, glsl);
            evaluateCapability (info, minimum);
            expect (info.meetsMinimum == expected, juce::String (version) + " / " + glsl);
            expect (info.verdict.isEmpty() == expected);
            return info;
        };

        check ("3.2.0 - Build 32.0.101.6733", "1.50 - Build 32.0.101.6733", true); // Intel, core
        check ("3.2.0 NVIDIA 560.94", "1.50 NVIDIA via Cg compiler", true);        // NVIDIA, core
        check ("4.6.0 - Build 32.0.101.6733", "4.60 - Build 32.0.101.6733", true); // Intel, "default"
        check ("4.1 Metal - 83.1", "4.10", true);

        const auto low = check ("3.1.0 Mesa", "1.40", false);
        expect (low.verdict.contains ("3.1") && low.verdict.contains ("3.2") && low.verdict.contains ("1.50"));

        const auto generic = check ("1.1.0", "", false); // "GDI Generic": a context, but a useless one
        expect (generic.verdict.contains ("below the required OpenGL 3.2"));

        check ("3.2.0 fake", "1.40", false); // the right GL version with too old a GLSL

        auto none = infoFor ("", "", false);
        evaluateCapability (none, minimum);
        expect (! none.meetsMinimum && none.verdict == "no OpenGL context");

        // A stricter requirement fails a driver that would otherwise pass (the forced-failure path).
        auto strict = infoFor ("4.6.0", "4.60");
        evaluateCapability (strict, requirementFor (9, 9));
        expect (! strict.meetsMinimum && strict.verdict.contains ("OpenGL 9.9"));

        auto described = infoFor ("3.2.0", "1.50");
        described.renderer = "Iris Xe";
        described.vendor = "Intel";
        described.profile = "core";
        expectEquals (describeGpu (described), juce::String ("OpenGL 3.2 / GLSL 1.50 (Iris Xe, Intel, core)"));
        expectEquals (describeGpu (none), juce::String ("no OpenGL context"));
    }

    void testOverrides()
    {
        beginTest ("The SNDS_RENDER switches parse; nothing is set by default");

        const RenderOverrides none;
        expect (! none.anySet());
        expect (! RenderOverrides::parse ("").anySet());

        const auto all = RenderOverrides::parse (
            "no-opengl, shader-failure,context-loss=900,gl-hang=2500,cpu-failure,require-gl=4.1");
        expect (all.forceNoOpenGL && all.forceShaderFailure && all.forceContextLoss && all.forceGlHang
                && all.forceCpuFailure);
        expectEquals (all.contextLossAfterMs, 900.0);
        expectEquals (all.glHangMs, 2500.0);
        expectEquals (all.requirement.major, 4);
        expectEquals (all.requirement.minor, 1);
        expectEquals (all.requirement.glsl, 4.1);
        expect (all.anySet());

        const auto defaults = RenderOverrides::parse ("context-loss,gl-hang");
        expectEquals (defaults.contextLossAfterMs, 1500.0);
        expectEquals (defaults.glHangMs, 4000.0);

        juce::StringArray unknown;
        const auto partial = RenderOverrides::parse ("SHADER-FAILURE, wobble, require-gl=nonsense", &unknown);
        expect (partial.forceShaderFailure);
        expectEquals (unknown.size(), 2);
        expect (unknown.contains ("wobble") && unknown.contains ("require-gl=nonsense"));
        expect (RenderOverrides::parse ("require-gl=9.9").anySet());
    }

    void testPaletteAndFit()
    {
        beginTest ("The palette is the average colour and the most vivid colour of the artwork");

        juce::Image image (juce::Image::ARGB, 64, 64, true);
        {
            juce::Graphics g (image);
            g.fillAll (juce::Colour (0xff202020));
            g.setColour (juce::Colour (0xffff2000));
            g.fillRect (0, 0, 32, 64);
        }

        const auto palette = derivePalette (image);
        expect (palette.accent.getHue() < 0.1f && palette.accent.getSaturation() > 0.9f);
        expect (palette.average.getFloatRed() > palette.average.getFloatBlue());
        expect (palette.average.getBrightness() > 0.2f && palette.average.getBrightness() < 0.9f);

        const auto invalid = derivePalette ({});
        expect (invalid.average == ArtworkPalette().average && invalid.accent == ArtworkPalette().accent);

        beginTest ("The static renderer fits the artwork without cropping or stretching");

        using R = juce::Rectangle<float>;
        const R wide (0.0f, 0.0f, 400.0f, 200.0f);
        expect (StaticRenderer::fitRectangle (wide, 100, 100) == R (100.0f, 0.0f, 200.0f, 200.0f));
        const R tall (0.0f, 0.0f, 200.0f, 400.0f);
        expect (StaticRenderer::fitRectangle (tall, 100, 50) == R (0.0f, 150.0f, 200.0f, 100.0f));
        expect (StaticRenderer::fitRectangle (wide, 0, 100).isEmpty());
        expect (StaticRenderer::fitRectangle ({}, 100, 100).isEmpty());

        beginTest ("Effect parameters default to the untouched artwork");

        EffectParams params;
        expect (params.isNeutral());
        params.glow = 0.1f;
        expect (! params.isNeutral());
        expect (tierName (RenderTier::gl) == "gl" && tierName (RenderTier::staticImage) == "static");
        expect (causeName (FallbackCause::shaderCompileFailed) == "shader-compile-failed");
        expect (RenderTier::gl < RenderTier::cpu && RenderTier::cpu < RenderTier::staticImage);
    }
};

RenderCapabilityTests renderCapabilityTests;
} // namespace
} // namespace snds::rendering
