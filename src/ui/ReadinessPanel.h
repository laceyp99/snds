#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/Readiness.h"

namespace snds
{
/** Shows the worker readiness: a status dot, a headline, what to do next, expandable details,
    and a Re-check button.
*/
class ReadinessPanel final : public juce::Component, private juce::Timer
{
public:
    ReadinessPanel();

    void setReadiness (const Readiness& readiness);

    /** Height for the current content; changes when details are shown or hidden. */
    int getPreferredHeight() const;

    std::function<void()> onRecheck;
    std::function<void()> onPreferredHeightChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateHeadline();
    void setDetailsShown (bool shouldShow);
    juce::Colour statusColour() const;

    Readiness current;
    double checkStartedMs = 0.0;
    bool detailsShown = false;

    juce::Label headline, guidance;
    juce::TextButton detailsButton { "Show details" }, recheckButton { "Re-check" };
    juce::TextEditor details;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReadinessPanel)
};
} // namespace snds
