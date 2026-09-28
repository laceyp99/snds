#include "TestUtilities.h"
#include "rendering/OpenGLCapability.h"

namespace snds::test
{

using namespace snds::rendering;

class OpenGLCapabilityTests final : public juce::UnitTest
{
public:
    OpenGLCapabilityTests()
        : juce::UnitTest ("OpenGL capability", testCategory)
    {
    }

    void runTest() override
    {
        beginTest ("GL_VERSION parsing");
        {
            expect (parseGlVersion ("4.6.0 NVIDIA 560.94") == Version { 4, 6 });
            expect (parseGlVersion ("3.3 (Core Profile) Mesa 24.0.5") == Version { 3, 3 });
            expect (parseGlVersion ("1.1.0") == Version { 1, 1 });
            expect (parseGlVersion ("OpenGL ES 3.2 v1.r32p1") == Version { 3, 2 });
            expect (! parseGlVersion ({}).isValid());
            expect (! parseGlVersion ("garbage").isValid());
        }

        beginTest ("GLSL version parsing");
        {
            expect (parseGlslVersion ("4.60 NVIDIA") == Version { 4, 60 });
            expect (parseGlslVersion ("1.50") == Version { 1, 50 });
            expect (parseGlslVersion ("1.5") == Version { 1, 50 });
            expect (parseGlslVersion ("OpenGL ES GLSL ES 3.20") == Version { 3, 20 });
            expect (! parseGlslVersion ({}).isValid());
        }

        beginTest ("Hardware OpenGL 4.6 meets the minimum");
        {
            const auto verdict = evaluateCapability ({ "NVIDIA Corporation",
                                                       "NVIDIA GeForce RTX 3060/PCIe/SSE2",
                                                       "4.6.0 NVIDIA 560.94",
                                                       "4.60 NVIDIA",
                                                       true });
            expect (verdict.meetsMinimum, verdict.reasons.joinIntoString ("; "));
            expect (! verdict.softwareRenderer);
        }

        beginTest ("Exactly 3.2 / GLSL 1.50 meets the minimum");
        {
            expect (evaluateCapability ({ "Vendor", "Some GPU", "3.2.0", "1.50", true }).meetsMinimum);
        }

        beginTest ("GDI Generic is rejected as software OpenGL 1.1");
        {
            const auto verdict = evaluateCapability ({ "Microsoft Corporation", "GDI Generic", "1.1.0", {}, false });
            expect (! verdict.meetsMinimum);
            expect (verdict.softwareRenderer);
            expectEquals (verdict.reasons.size(), 3);
        }

        beginTest ("Old versions and software rasterisers are rejected individually");
        {
            expect (! evaluateCapability ({ "Intel", "Old iGPU", "3.1.0", "1.40", false }).meetsMinimum);
            expect (! evaluateCapability ({ "Intel", "GPU", "3.3.0", "1.30", false }).meetsMinimum);

            const OpenGLInfo llvmpipe { "Mesa",
                                        "llvmpipe (LLVM 17.0, 256 bits)",
                                        "4.5 (Core Profile) Mesa 24.0",
                                        "4.50",
                                        true };
            expect (! evaluateCapability (llvmpipe).meetsMinimum);

            CapabilityRequirements permissive;
            permissive.rejectSoftwareRenderers = false;
            expect (evaluateCapability (llvmpipe, permissive).meetsMinimum);
        }
    }
};

static OpenGLCapabilityTests openGLCapabilityTests;

} // namespace snds::test
