#include "app/ProgressText.h"

#include <cmath>

namespace snds
{
juce::String formatSpan (double seconds)
{
    auto total = static_cast<int> (std::llround (juce::jmax (0.0, seconds)));

    if (total >= 3600)
        return juce::String (total / 3600) + " h " + juce::String ((total % 3600) / 60).paddedLeft ('0', 2) + " min";

    if (total > 60)
        total = ((total + 2) / 5) * 5;

    if (total >= 60)
        return juce::String (total / 60) + " min"
             + (total % 60 != 0 ? " " + juce::String (total % 60) + " s" : juce::String());

    return juce::String (total) + " s";
}

std::optional<double> credibleEta (const SessionStatus& status)
{
    if (status.state != SessionState::preparing || ! status.etaSeconds.has_value())
        return std::nullopt;

    const auto eta = *status.etaSeconds;
    return std::isfinite (eta) && eta >= 1.0 && eta <= 6.0 * 3600.0 ? std::optional<double> (eta) : std::nullopt;
}

juce::String describeProgress (const SessionStatus& status, double elapsedSeconds)
{
    auto line = status.stage;

    if (const auto eta = credibleEta (status))
        line += "  -  about " + formatSpan (*eta) + " left in this step";

    if (status.device.isNotEmpty() && line.isNotEmpty())
        line += "  -  " + juce::String (status.device == "cuda" ? "GPU" : "CPU");

    if (status.state == SessionState::preparing && elapsedSeconds >= 1.0 && line.isNotEmpty())
        line += "  -  " + formatSpan (elapsedSeconds) + " so far";

    return line;
}

juce::String describePrepared (double elapsedSeconds, const juce::String& device)
{
    return "Prepared in " + formatSpan (elapsedSeconds)
         + (device.isNotEmpty() ? " on the " + juce::String (device == "cuda" ? "GPU" : "CPU") : juce::String()) + ".";
}
} // namespace snds
