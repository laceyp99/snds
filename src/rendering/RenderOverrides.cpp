#include "rendering/RenderOverrides.h"

#include "rendering/GlCapability.h"

namespace snds::rendering
{
RenderOverrides RenderOverrides::parse (const juce::String& list, juce::StringArray* unknown)
{
    RenderOverrides overrides;

    for (const auto& raw : juce::StringArray::fromTokens (list, ",", ""))
    {
        const auto item = raw.trim();

        if (item.isEmpty())
            continue;

        const auto name = item.upToFirstOccurrenceOf ("=", false, false).trim().toLowerCase();
        const auto value = item.fromFirstOccurrenceOf ("=", false, false).trim();

        if (name == "no-opengl")
        {
            overrides.forceNoOpenGL = true;
        }
        else if (name == "shader-failure")
        {
            overrides.forceShaderFailure = true;
        }
        else if (name == "cpu-failure")
        {
            overrides.forceCpuFailure = true;
        }
        else if (name == "context-loss")
        {
            overrides.forceContextLoss = true;

            if (value.isNotEmpty())
                overrides.contextLossAfterMs = value.getDoubleValue();
        }
        else if (name == "gl-hang")
        {
            overrides.forceGlHang = true;

            if (value.isNotEmpty())
                overrides.glHangMs = value.getDoubleValue();
        }
        else if (name == "require-gl" && value.containsChar ('.'))
        {
            overrides.requirement = requirementFor (value.upToFirstOccurrenceOf (".", false, false).getIntValue(),
                                                    value.fromFirstOccurrenceOf (".", false, false).getIntValue());
        }
        else if (unknown != nullptr)
        {
            unknown->add (item);
        }
    }

    return overrides;
}

RenderOverrides RenderOverrides::fromEnvironment()
{
    return parse (juce::SystemStats::getEnvironmentVariable ("SNDS_RENDER", {}));
}
} // namespace snds::rendering
