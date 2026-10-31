#include <juce_core/juce_core.h>

#include <iostream>

#include "SpikeInfo.h"

namespace
{
class SpikeInfoTests final : public juce::UnitTest
{
public:
    SpikeInfoTests() : juce::UnitTest ("SNDS JUCE fetch spike", "snds") {}

    void runTest() override
    {
        beginTest ("Compiled JUCE headers match the FetchContent pin");
        expectEquals (snds::spike::compiledJuceVersion(), juce::String (SNDS_EXPECTED_JUCE_VERSION));
        expect (snds::spike::juceVersionMatchesPin());

        beginTest ("Linked JUCE runtime reports the pinned version");
        expect (juce::SystemStats::getJUCEVersion().endsWith (SNDS_EXPECTED_JUCE_VERSION),
                juce::SystemStats::getJUCEVersion());

        beginTest ("Shared app logic is testable without the GUI");
        expectWithinAbsoluteError (snds::spike::samplesToSeconds (44100, 44100.0), 1.0, 1.0e-12);
        expectWithinAbsoluteError (snds::spike::samplesToSeconds (96000, 48000.0), 2.0, 1.0e-12);
        expectEquals (snds::spike::samplesToSeconds (1000, 0.0), 0.0);
    }
};

SpikeInfoTests spikeInfoTests;
} // namespace

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory ("snds");

    int failures = 0;
    int passes = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult (i)->failures;
        passes += runner.getResult (i)->passes;
    }

    std::cout << "JUCE " << juce::SystemStats::getJUCEVersion() << ": " << passes << " passed, "
              << failures << " failed" << std::endl;

    return (failures == 0 && passes > 0) ? 0 : 1;
}
