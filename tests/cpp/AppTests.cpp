#include <juce_core/juce_core.h>

#include "app/AppInfo.h"
#include "app/AppOptions.h"

namespace snds
{
namespace
{
class AppInfoTests final : public juce::UnitTest
{
public:
    AppInfoTests()
        : juce::UnitTest ("AppInfo", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Compiled JUCE headers match the CMake pin");
        expectEquals (appinfo::compiledJuceVersion(), appinfo::pinnedJuceVersion());

        beginTest ("Linked JUCE runtime matches the CMake pin");
        expectEquals (juce::SystemStats::getJUCEVersion(), "JUCE v" + appinfo::pinnedJuceVersion());
        expect (appinfo::juceMatchesPin());

        beginTest ("App version is a dotted triple");
        const auto parts = juce::StringArray::fromTokens (appinfo::version(), ".", {});
        expectEquals (parts.size(), 3);

        for (const auto& part : parts)
            expect (part.containsOnly ("0123456789") && part.isNotEmpty(), appinfo::version());
    }
};

class AppOptionsTests final : public juce::UnitTest
{
public:
    AppOptionsTests()
        : juce::UnitTest ("AppOptions", "snds")
    {
    }

    void runTest() override
    {
        const juce::File cwd ("C:\\snds-test\\cwd");

        beginTest ("No arguments gives defaults");
        {
            const auto options = AppOptions::parse ({}, cwd);
            expect (! options.smokeExit);
            expect (options.workerRoot == juce::File());
            expect (options.problems.isEmpty());
        }

        beginTest ("--smoke-exit is recognised");
        {
            const auto options = AppOptions::parse ({ "--smoke-exit" }, cwd);
            expect (options.smokeExit);
            expect (options.problems.isEmpty());
        }

        beginTest ("--worker-root accepts a separate or '=' value");
        {
            const auto separate = AppOptions::parse ({ "--worker-root", "D:\\work\\snds\\worker" }, cwd);
            expectEquals (separate.workerRoot.getFullPathName(), juce::String ("D:\\work\\snds\\worker"));
            expect (separate.problems.isEmpty());

            const auto assigned = AppOptions::parse ({ "--worker-root=\"D:\\My Projects\\worker\"" }, cwd);
            expectEquals (assigned.workerRoot.getFullPathName(), juce::String ("D:\\My Projects\\worker"));
            expect (assigned.problems.isEmpty());
        }

        beginTest ("Relative --worker-root resolves against the working directory");
        {
            const auto options = AppOptions::parse ({ "--worker-root", "..\\worker" }, cwd);
            expectEquals (options.workerRoot.getFullPathName(), juce::String ("C:\\snds-test\\worker"));
        }

        beginTest ("--worker-root without a value is a problem, not a crash");
        {
            const auto trailing = AppOptions::parse ({ "--worker-root" }, cwd);
            expect (trailing.workerRoot == juce::File());
            expectEquals (trailing.problems.size(), 1);

            const auto followedByOption = AppOptions::parse ({ "--worker-root", "--smoke-exit" }, cwd);
            expect (followedByOption.smokeExit);
            expect (followedByOption.workerRoot == juce::File());
            expectEquals (followedByOption.problems.size(), 1);
        }

        beginTest ("Unknown arguments are reported");
        {
            const auto options = AppOptions::parse ({ "--smoke-exti", "song.mp3" }, cwd);
            expect (! options.smokeExit);
            expectEquals (options.problems.size(), 2);
        }

        beginTest ("--smoke-exit-after-readiness is recognised");
        {
            const auto options = AppOptions::parse ({ "--smoke-exit-after-readiness" }, cwd);
            expect (options.smokeExitAfterReadiness);
            expect (! options.smokeExit);
            expect (options.problems.isEmpty());
        }

        beginTest ("--readiness-timeout takes 1 to 3600 seconds");
        {
            expectEquals (AppOptions::parse ({}, cwd).readinessTimeoutSeconds,
                          AppOptions::defaultReadinessTimeoutSeconds);
            expectEquals (AppOptions::parse ({ "--readiness-timeout", "30" }, cwd).readinessTimeoutSeconds, 30);
            expectEquals (AppOptions::parse ({ "--readiness-timeout=3600" }, cwd).readinessTimeoutSeconds, 3600);

            for (const auto* bad : { "0", "3601", "abc", "-5", "1.5" })
            {
                const auto options = AppOptions::parse ({ "--readiness-timeout", bad }, cwd);
                expectEquals (options.readinessTimeoutSeconds, AppOptions::defaultReadinessTimeoutSeconds, bad);
                expectEquals (options.problems.size(), 1, bad);
            }

            const auto missing = AppOptions::parse ({ "--readiness-timeout" }, cwd);
            expectEquals (missing.problems.size(), 1);
        }
    }
};

// Constructing a juce::UnitTest registers it with the runner.
AppInfoTests appInfoTests;
AppOptionsTests appOptionsTests;
} // namespace
} // namespace snds
