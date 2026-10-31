#pragma once

#include "ui/MixControls.h"

namespace snds
{
/** The master strip: master gain, the output meter, and the limiter indicator.

    The limiter is metered, not hidden. The meter reads the output, which never exceeds the
    ceiling, so the lamp is what shows the mix is too hot: it lights when the limiter takes off at
    least 0.1 dB (and holds with the meters' release), shows the reduction, and, if it stays lit
    for a while, says what to turn down.
*/
class MasterSection final : public juce::Component
{
public:
    explicit MasterSection (dsp::ParameterStore& store);

    /** One meter reading; `nowMs` is a monotonic clock in milliseconds. */
    void update (const dsp::MeterSnapshot& snapshot, double nowMs);
    void clearLevels();
    void syncFromStore() { gain.syncFromStore(); }

    bool isLimiterLit() const noexcept { return lit; }
    bool isAdviceShown() const noexcept { return adviceShown; }
    ParameterSlider& gainSlider() noexcept { return gain; }

    /** "Limiter -2.4 dB" while it works, "Limiter idle" otherwise. */
    static juce::String limiterText (const dsp::MeterSnapshot& snapshot);

    /** How long the limiter must stay lit before the advice appears. */
    static constexpr double adviceAfterMs = 1500.0;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Label title;
    ParameterSlider gain;
    LevelMeter meterBar;
    juce::Rectangle<int> lampArea, adviceArea;

    bool lit = false;
    bool adviceShown = false;
    double litSince = -1.0;
    juce::String lampText { "Limiter idle" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterSection)
};
} // namespace snds
