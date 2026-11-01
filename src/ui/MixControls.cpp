#include "ui/MixControls.h"

#include <cmath>

namespace snds
{
namespace
{
bool isResetKey (const juce::KeyPress& key)
{
    return key.isKeyCode (juce::KeyPress::deleteKey) || key.isKeyCode (juce::KeyPress::backspaceKey);
}

juce::String parameterName (int index)
{
    const auto view = dsp::parameterSpec (index).name.view();
    return juce::String (view.data(), view.size());
}
} // namespace

ParameterSlider::ParameterSlider (dsp::ParameterStore& parameterStore,
                                  int parameterIndex,
                                  juce::Slider::SliderStyle style,
                                  bool withTextBox)
    : juce::Slider (style, withTextBox ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox),
      store (parameterStore),
      index (parameterIndex)
{
    setRange (0.0, 1.0, 0.0);
    setName (parameterName (index));
    setWantsKeyboardFocus (true);
    setHasFocusOutline (true);

    if (withTextBox)
        setTextBoxStyle (style == juce::Slider::LinearHorizontal ? juce::Slider::TextBoxRight
                                                                 : juce::Slider::TextBoxBelow,
                         false,
                         style == juce::Slider::LinearHorizontal ? 66 : 72,
                         20);

    hookTextBox();
    syncFromStore();
}

void ParameterSlider::hookTextBox()
{
    for (auto* child : getChildren())
    {
        auto* label = dynamic_cast<juce::Label*> (child);

        if (label == nullptr)
            continue;

        // The number is not a tab stop of its own (the slider is); Return or double-click opens it.
        label->setWantsKeyboardFocus (false);

        // Typing starts from the precise value, not the rounded one on display.
        label->onEditorShow = [this, safe = juce::Component::SafePointer<juce::Label> (label)]
        {
            if (safe == nullptr)
                return;

            if (auto* editor = safe->getCurrentTextEditor())
            {
                editor->setText (exactText(), false);
                editor->selectAll();
            }
        };
    }
}

void ParameterSlider::lookAndFeelChanged()
{
    juce::Slider::lookAndFeelChanged();
    hookTextBox(); // the look and feel may have replaced the text box
}

void ParameterSlider::syncFromStore()
{
    if (isMouseButtonDown())
        return;

    const juce::ScopedValueSetter<bool> guard (syncing, true);
    setValue (dsp::toNormalised (index, store.get (index)), juce::dontSendNotification);
    updateText();
}

void ParameterSlider::resetToDefault()
{
    setValue (static_cast<double> (dsp::toNormalised (index, dsp::parameterSpec (index).defaultValue)),
              juce::sendNotificationSync);
}

juce::String ParameterSlider::exactText() const
{
    return dsp::formatExact (index, store.get (index));
}

ParameterSlider::KeyboardSteps ParameterSlider::keyboardSteps (int parameterIndex) noexcept
{
    switch (dsp::parameterSpec (parameterIndex).unit)
    {
        case dsp::ParameterUnit::decibels:
            return { 0.5f, 3.0f, false };
        case dsp::ParameterUnit::pan:
            return { 0.05f, 0.25f, false };
        case dsp::ParameterUnit::hertz:
            return { 1.0f / 48.0f, 1.0f / 12.0f, true }; // log scale: a twelfth of the travel is about half an octave
        case dsp::ParameterUnit::percent:
            return { 0.01f, 0.05f, false };
        default:
            return { 0.01f, 0.1f, true };
    }
}

void ParameterSlider::moveBy (float amount, bool normalisedTravel)
{
    const auto current = store.get (index);
    float target;

    if (normalisedTravel)
        target = dsp::fromNormalised (index, dsp::toNormalised (index, current) + amount);
    else
        target = current + amount;

    setValue (static_cast<double> (dsp::toNormalised (index, dsp::clampParameterValue (index, target))),
              juce::sendNotificationSync);
}

bool ParameterSlider::keyPressed (const juce::KeyPress& key)
{
    if (! isEnabled())
        return false;

    if (isResetKey (key))
    {
        resetToDefault();
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::returnKey))
    {
        showTextBox();
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::homeKey))
    {
        setValue (0.0, juce::sendNotificationSync);
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::endKey))
    {
        setValue (1.0, juce::sendNotificationSync);
        return true;
    }

    const auto steps = keyboardSteps (index);
    float direction = 0.0f;
    bool coarse = key.getModifiers().isShiftDown();

    if (key.isKeyCode (juce::KeyPress::rightKey) || key.isKeyCode (juce::KeyPress::upKey))
    {
        direction = 1.0f;
    }
    else if (key.isKeyCode (juce::KeyPress::leftKey) || key.isKeyCode (juce::KeyPress::downKey))
    {
        direction = -1.0f;
    }
    else if (key.isKeyCode (juce::KeyPress::pageUpKey))
    {
        direction = 1.0f;
        coarse = true;
    }
    else if (key.isKeyCode (juce::KeyPress::pageDownKey))
    {
        direction = -1.0f;
        coarse = true;
    }

    if (direction == 0.0f || key.getModifiers().isCommandDown() || key.getModifiers().isAltDown())
        return false;

    moveBy (direction * (coarse ? steps.coarse : steps.fine), steps.inNormalisedTravel);
    return true;
}

juce::PopupMenu ParameterSlider::buildContextMenu() const
{
    juce::PopupMenu menu;
    const auto& spec = dsp::parameterSpec (index);
    menu.addItem (menuReset,
                  "Reset " + parameterName (index).toLowerCase() + " to "
                      + dsp::formatDisplay (index, spec.defaultValue),
                  isEnabled());
    menu.addItem (
        menuEnterValue, "Enter exact value...", isEnabled() && getTextBoxPosition() != juce::Slider::NoTextBox);
    return menu;
}

void ParameterSlider::performContextAction (int menuItemId)
{
    if (menuItemId == menuReset)
        resetToDefault();
    else if (menuItemId == menuEnterValue)
        showTextBox();
}

void ParameterSlider::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu() && isEnabled())
    {
        buildContextMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                                          [safe = juce::Component::SafePointer<ParameterSlider> (this)] (int chosen)
                                          {
                                              if (safe != nullptr && chosen != 0)
                                                  safe->performContextAction (chosen);
                                          });
        return;
    }

    juce::Slider::mouseDown (event);
}

void ParameterSlider::mouseDoubleClick (const juce::MouseEvent&)
{
    if (isEnabled())
        resetToDefault();
}

juce::String ParameterSlider::getTextFromValue (double normalised)
{
    return dsp::formatDisplay (index, dsp::fromNormalised (index, static_cast<float> (normalised)));
}

double ParameterSlider::getValueFromText (const juce::String& text)
{
    if (const auto value = dsp::parseValue (index, text))
        return static_cast<double> (dsp::toNormalised (index, *value));

    return getValue(); // unreadable text: keep the current value
}

void ParameterSlider::valueChanged()
{
    if (! syncing)
        store.set (index, dsp::fromNormalised (index, static_cast<float> (getValue())));
}

//==============================================================================
bool KeyButton::keyPressed (const juce::KeyPress& key)
{
    if (isEnabled() && key.isKeyCode (juce::KeyPress::spaceKey))
    {
        triggerClick();
        return true;
    }

    return juce::TextButton::keyPressed (key);
}

//==============================================================================
ParameterToggle::ParameterToggle (dsp::ParameterStore& parameterStore, int parameterIndex, const juce::String& text)
    : juce::TextButton (text),
      store (parameterStore),
      index (parameterIndex)
{
    setClickingTogglesState (true);
    setHasFocusOutline (true);
    onClick = [this] { store.set (index, getToggleState() ? 1.0f : 0.0f); };
    syncFromStore();
}

void ParameterToggle::syncFromStore()
{
    setToggleState (store.get (index) >= 0.5f, juce::dontSendNotification);
}

void ParameterToggle::resetToDefault()
{
    store.set (index, dsp::parameterSpec (index).defaultValue);
    syncFromStore();
}

void ParameterToggle::toggleFromKeyboard()
{
    setToggleState (! getToggleState(), juce::dontSendNotification);
    store.set (index, getToggleState() ? 1.0f : 0.0f);
}

bool ParameterToggle::keyPressed (const juce::KeyPress& key)
{
    if (! isEnabled())
        return false;

    if (key.isKeyCode (juce::KeyPress::spaceKey) || key.isKeyCode (juce::KeyPress::returnKey))
    {
        toggleFromKeyboard();
        return true;
    }

    if (isResetKey (key))
    {
        resetToDefault();
        return true;
    }

    return false;
}

juce::PopupMenu ParameterToggle::buildContextMenu() const
{
    juce::PopupMenu menu;
    menu.addItem (ParameterSlider::menuReset, "Reset " + parameterName (index).toLowerCase() + " (off)", isEnabled());
    return menu;
}

void ParameterToggle::performContextAction (int menuItemId)
{
    if (menuItemId == ParameterSlider::menuReset)
        resetToDefault();
}

void ParameterToggle::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu() && isEnabled())
    {
        buildContextMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                                          [safe = juce::Component::SafePointer<ParameterToggle> (this)] (int chosen)
                                          {
                                              if (safe != nullptr && chosen != 0)
                                                  safe->performContextAction (chosen);
                                          });
        return;
    }

    juce::TextButton::mouseDown (event);
}

//==============================================================================
float LevelMeter::position (float linear) noexcept
{
    const auto db = linear <= 1.0e-6f ? floorDb : 20.0f * std::log10 (linear);
    return juce::jlimit (0.0f, 1.0f, (db - floorDb) / (ceilingDb - floorDb));
}

void LevelMeter::setLevels (float peak, float rms)
{
    if (peak == peakLevel && rms == rmsLevel)
        return;

    peakLevel = peak;
    rmsLevel = rms;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (area, 2.0f);

    const auto bottom = area.getBottom();
    const auto height = area.getHeight();
    const auto zero = position (1.0f);

    // RMS body, with the part above 0 dBFS in red.
    const auto rmsTop = bottom - height * position (rmsLevel);
    g.setColour (accent.withAlpha (0.85f));
    g.fillRect (area.getX() + 1.0f,
                std::max (rmsTop, bottom - height * zero),
                area.getWidth() - 2.0f,
                bottom - std::max (rmsTop, bottom - height * zero));

    if (position (rmsLevel) > zero)
    {
        g.setColour (juce::Colour (0xffe5534b));
        g.fillRect (area.getX() + 1.0f, rmsTop, area.getWidth() - 2.0f, (bottom - height * zero) - rmsTop);
    }

    // Peak line.
    const auto peakY = bottom - height * position (peakLevel);
    g.setColour (peakLevel > 1.0f ? juce::Colour (0xffe5534b) : juce::Colours::white.withAlpha (0.9f));
    g.fillRect (area.getX() + 1.0f, peakY - 1.0f, area.getWidth() - 2.0f, 2.0f);

    // The 0 dBFS mark.
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.fillRect (area.getX(), bottom - height * zero, area.getWidth(), 1.0f);
}
} // namespace snds
