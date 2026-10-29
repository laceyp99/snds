// Console runner for SNDS JUCE unit tests.
//
// Usage: snds_tests [--list | --benchmark] [name-filter]
//   --list        print the registered test names and exit
//   --benchmark   run the opt-in benchmarks (e.g. fingerprint throughput) instead of the unit tests
//   name-filter   run only tests whose name contains the filter (case-insensitive)
//
// Exit codes: 0 all passed, 1 failures, 2 no tests matched.

#include "TestUtilities.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <iostream>

namespace
{
class ConsoleTestRunner final : public juce::UnitTestRunner
{
    void logMessage (const juce::String& message) override { std::cout << message << std::endl; }
};
} // namespace

int main (int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    auto argument = argc > 1 ? juce::String::fromUTF8 (argv[1]) : juce::String();
    const auto benchmarks = argument == "--benchmark";

    if (benchmarks)
        argument = argc > 2 ? juce::String::fromUTF8 (argv[2]) : juce::String();

    const auto registered =
        juce::UnitTest::getTestsInCategory (benchmarks ? snds::test::benchmarkCategory : snds::test::testCategory);

    if (argument == "--list")
    {
        for (auto* test : registered)
            std::cout << test->getName() << std::endl;

        return 0;
    }

    juce::Array<juce::UnitTest*> selected;

    for (auto* test : registered)
        if (argument.isEmpty() || test->getName().containsIgnoreCase (argument))
            selected.add (test);

    if (selected.isEmpty())
    {
        std::cerr << "No SNDS tests match \"" << argument << "\"." << std::endl;
        return 2;
    }

    ConsoleTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.setPassesAreLogged (false);
    runner.runTests (selected);

    auto passes = 0;
    auto failures = 0;

    for (auto i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);
        passes += result->passes;
        failures += result->failures;
    }

    std::cout << std::endl
              << "SNDS C++ tests: " << selected.size() << " suites, " << passes << " checks passed, " << failures
              << " failed." << std::endl;

    return failures == 0 ? 0 : 1;
}
