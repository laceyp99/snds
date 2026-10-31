#pragma once

#include "ui/MixControls.h"

#include <array>
#include <functional>

namespace snds
{
/** One stem's strip: name, gain fader beside a level meter, mute and solo, and a "More" section
    (pan, low cut, high cut, reverb send, delay send) that expands below. The always-visible part
    is what a listener reaches for first; the rest is one click away and says what it is.

    The card only reads and writes the ParameterStore; levels arrive from the panel's timer.
*/
class StemCard final : public juce::Component
{
public:
    StemCard (dsp::ParameterStore& store, dsp::Stem stem);

    dsp::Stem getStem() const noexcept { return stem; }

    void setLevels (const dsp::StemMeter& meter) { meterBar.setLevels (meter.peak, meter.rms); }
    void clearLevels() { meterBar.clear(); }
    float meterPeak() const noexcept { return meterBar.getPeak(); }

    /** Pulls every control back from the store (after a reset or a change from elsewhere). */
    void syncFromStore();

    void setExpanded (bool shouldExpand);
    bool isExpanded() const noexcept { return expanded; }

    static constexpr int collapsedHeight = 246;
    static constexpr int expandedExtra = 148;
    int getPreferredHeight() const noexcept { return collapsedHeight + (expanded ? expandedExtra : 0); }

    /** Fires when expanding or collapsing changes the preferred height. */
    std::function<void()> onPreferredHeightChanged;

    // Direct access for tests and the keyboard layer.
    ParameterSlider& gainSlider() noexcept { return gain; }
    ParameterToggle& muteButton() noexcept { return mute; }
    ParameterToggle& soloButton() noexcept { return solo; }
    ParameterSlider& secondarySlider (dsp::StemControl control);
    juce::Button& moreButton() noexcept { return moreToggle; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int secondaryCount = 5;

    dsp::ParameterStore& store;
    const dsp::Stem stem;
    bool expanded = false;
    juce::Colour accent;

    juce::Label title;
    ParameterSlider gain;
    LevelMeter meterBar;
    ParameterToggle mute, solo;
    KeyButton moreToggle { "More" };

    std::array<std::unique_ptr<ParameterSlider>, secondaryCount> secondary;
    std::array<juce::Label, secondaryCount> secondaryLabels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemCard)
};

/** The accent colour of a stem. */
juce::Colour stemAccent (dsp::Stem stem);
} // namespace snds
