#pragma once

#include <juce_core/juce_core.h>

#include <initializer_list>
#include <vector>

namespace snds::test
{
/** The DSP, visual and render tests run a reduced but still meaningful workload by default, so the
    whole suite stays quick. Set SNDS_HEAVY_TESTS=1 for the full workload (every sample rate, the
    long runs): do that before a release or when touching the DSP. */
inline bool heavyTests()
{
    static const bool heavy = juce::SystemStats::getEnvironmentVariable ("SNDS_HEAVY_TESTS", {}) == "1";
    return heavy;
}

/** The sample rates to test: `reduced` normally, `full` with SNDS_HEAVY_TESTS=1. */
inline std::vector<double> ratesFor (std::initializer_list<double> reduced, std::initializer_list<double> full)
{
    return heavyTests() ? std::vector<double> (full) : std::vector<double> (reduced);
}

/** A duration or count: `reduced` normally, `full` with SNDS_HEAVY_TESTS=1. */
template <typename T>
T workload (T reduced, T full)
{
    return heavyTests() ? full : reduced;
}
} // namespace snds::test
