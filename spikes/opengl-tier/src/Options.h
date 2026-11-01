#pragma once

#include <juce_core/juce_core.h>

namespace snds::spike
{
// Command-line switches. GUI apps have no console, so results go to --report.
struct Options
{
    double selfTestSeconds = 0.0;          // --self-test <seconds>; 0 = interactive
    juce::File reportFile;                 // --report <path>
    bool forceNoOpenGL = false;            // --force-no-opengl
    bool forceShaderFailure = false;       // --force-shader-failure
    bool forceContextLoss = false;         // --force-context-loss [ms]
    double contextLossAfterMs = 1500.0;
    bool forceCpuFailure = false;          // --force-cpu-failure
    bool forceGlHang = false;              // --force-gl-hang [ms] (diagnostic: render thread hangs)
    double glHangMs = 4000.0;
    juce::String requestGl = "3.2";        // --request-gl default|3.2|4.1|4.3
    int requireMajor = 3, requireMinor = 2; // --require-gl <major.minor>
    juce::String expectTier;               // --expect-tier gl|cpu|static (self-test expectation override)

    static Options parse (const juce::StringArray& args)
    {
        Options o;

        for (int i = 0; i < args.size(); ++i)
        {
            const auto arg = args[i].unquoted();
            const auto next = [&] { return i + 1 < args.size() ? args[++i].unquoted() : juce::String(); };
            const auto nextIsValue = [&] { return i + 1 < args.size() && ! args[i + 1].startsWith ("--"); };

            if (arg == "--self-test")                 o.selfTestSeconds = next().getDoubleValue();
            else if (arg == "--report")               o.reportFile = juce::File (next());
            else if (arg == "--force-no-opengl")      o.forceNoOpenGL = true;
            else if (arg == "--force-shader-failure") o.forceShaderFailure = true;
            else if (arg == "--force-cpu-failure")    o.forceCpuFailure = true;
            else if (arg == "--request-gl")           o.requestGl = next();
            else if (arg == "--expect-tier")          o.expectTier = next();
            else if (arg == "--force-context-loss")
            {
                o.forceContextLoss = true;

                if (nextIsValue())
                    o.contextLossAfterMs = next().getDoubleValue();
            }
            else if (arg == "--force-gl-hang")
            {
                o.forceGlHang = true;

                if (nextIsValue())
                    o.glHangMs = next().getDoubleValue();
            }
            else if (arg == "--require-gl")
            {
                const auto v = next();
                o.requireMajor = v.upToFirstOccurrenceOf (".", false, false).getIntValue();
                o.requireMinor = v.fromFirstOccurrenceOf (".", false, false).getIntValue();
            }
        }

        if (o.reportFile == juce::File())
            o.reportFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                               .getSiblingFile ("opengl-tier-report.json");

        return o;
    }

    juce::String requiredVersionString() const { return juce::String (requireMajor) + "." + juce::String (requireMinor); }

    // GLSL version that ships with a given core GL version (3.2 -> 1.50, 3.3+ -> same number).
    double requiredGlslVersion() const
    {
        const auto gl = requireMajor * 10 + requireMinor;

        if (gl <= 20) return 1.10;
        if (gl == 21) return 1.20;
        if (gl == 30) return 1.30;
        if (gl == 31) return 1.40;
        if (gl == 32) return 1.50;
        return requireMajor + requireMinor / 10.0;
    }
};
} // namespace snds::spike
