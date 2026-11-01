#pragma once

#include "dsp/MixMeters.h"
#include "dsp/MixParameters.h"
#include "dsp/ParameterStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace snds
{
/** A slider bound to one mix parameter of a ParameterStore.

    The slider runs 0..1 along the parameter's UI scale (toNormalised / fromNormalised: dB, log
    frequency or linear), so equal travel means equal musical change. Moving it writes the store;
    syncFromStore() brings it back when something else changed the value (a reset).

    Exact values: double-click the number (or press Return on the focused slider) to type. The
    box opens with the precise value (`exactText`) and takes the parameter's own words ("-3.25",
    "-inf", "1.2k", "L30", "35%"); text it cannot read is ignored.

    Keyboard (when focused): arrows move by a fine step, Shift+arrows and Page Up/Down by a coarse
    step, Home/End jump to the ends, Delete or Backspace returns to the default, Return edits the
    number. Double-click on the track, or "Reset" in the right-click menu, also returns to the
    default.
*/
class ParameterSlider : public juce::Slider
{
public:
    ParameterSlider (dsp::ParameterStore& store,
                     int parameterIndex,
                     juce::Slider::SliderStyle style,
                     bool withTextBox = true);

    int getParameterIndex() const noexcept { return index; }

    /** The value now held by the store, in the parameter's own unit. */
    float getParameterValue() const noexcept { return store.get (index); }

    /** Takes the store's value unless the user is dragging. */
    void syncFromStore();

    /** Back to the parameter's default, through the store. */
    void resetToDefault();

    /** The precise text of the current value, shown while typing ("-3.25 dB"). */
    juce::String exactText() const;

    /** Keyboard step sizes: in the parameter's unit, or (for log scales) in 0..1 travel. */
    struct KeyboardSteps
    {
        float fine = 0.0f;
        float coarse = 0.0f;
        bool inNormalisedTravel = false;
    };
    static KeyboardSteps keyboardSteps (int parameterIndex) noexcept;

    /** Right-click menu: Reset (with the default's value) and Enter exact value. */
    enum MenuItem
    {
        menuReset = 1,
        menuEnterValue = 2
    };
    juce::PopupMenu buildContextMenu() const;
    void performContextAction (int menuItemId);

    juce::String getTextFromValue (double normalised) override;
    double getValueFromText (const juce::String& text) override;
    void valueChanged() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;
    void lookAndFeelChanged() override;

private:
    void hookTextBox();
    void moveBy (float amount, bool normalisedTravel);

    dsp::ParameterStore& store;
    const int index;
    bool syncing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterSlider)
};

/** A button that also answers Space (JUCE buttons answer Return only) and shows focus. */
class KeyButton : public juce::TextButton
{
public:
    KeyButton() { setHasFocusOutline (true); }
    explicit KeyButton (const juce::String& text)
        : juce::TextButton (text)
    {
        setHasFocusOutline (true);
    }

    bool keyPressed (const juce::KeyPress& key) override;
};

/** A toggle button (mute, solo) bound to a 0/1 parameter. Space and Return toggle it, Delete or
    Backspace turns it off, right-click offers Reset. */
class ParameterToggle : public juce::TextButton
{
public:
    ParameterToggle (dsp::ParameterStore& store, int parameterIndex, const juce::String& text);

    int getParameterIndex() const noexcept { return index; }
    void syncFromStore();

    /** Back to the default (off), through the store. */
    void resetToDefault();

    juce::PopupMenu buildContextMenu() const;
    void performContextAction (int menuItemId);

    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& event) override;

private:
    void toggleFromKeyboard();

    dsp::ParameterStore& store;
    const int index;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterToggle)
};

/** A vertical level meter: a filled bar for RMS and a line for the peak, on a -60 to +6 dBFS
    scale with the 0 dBFS mark drawn. Levels above 0 dBFS turn red. */
class LevelMeter final : public juce::Component
{
public:
    LevelMeter() = default;

    static constexpr float floorDb = -60.0f;
    static constexpr float ceilingDb = 6.0f;

    /** Linear level to 0..1 position on the meter scale. */
    static float position (float linear) noexcept;

    void setLevels (float peak, float rms);
    void clear() { setLevels (0.0f, 0.0f); }
    float getPeak() const noexcept { return peakLevel; }
    float getRms() const noexcept { return rmsLevel; }

    void setAccent (juce::Colour colour) { accent = colour; }
    void paint (juce::Graphics&) override;

private:
    float peakLevel = 0.0f;
    float rmsLevel = 0.0f;
    juce::Colour accent { 0xff58b368 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};
} // namespace snds
