#include "ui/TempoFxSection.h"

#include <cmath>

namespace snds
{
namespace
{
constexpr int pad = 10;
const juce::Colour warning (0xffd29922);

int tempoIndex()
{
    return dsp::parameterIndex (dsp::GlobalControl::tempoBpm);
}

juce::String bpmText (double bpm)
{
    return juce::String (bpm, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
}
} // namespace

bool ResettableComboBox::keyPressed (const juce::KeyPress& key)
{
    if (isEnabled() && (key.isKeyCode (juce::KeyPress::deleteKey) || key.isKeyCode (juce::KeyPress::backspaceKey)))
    {
        if (onReset)
            onReset();

        return true;
    }

    return juce::ComboBox::keyPressed (key);
}

void ResettableComboBox::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu() && isEnabled())
    {
        juce::PopupMenu menu;
        menu.addItem (1, resetText);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe = juce::Component::SafePointer<ResettableComboBox> (this)] (int chosen)
                            {
                                if (safe != nullptr && chosen == 1 && safe->onReset)
                                    safe->onReset();
                            });
        return;
    }

    juce::ComboBox::mouseDown (event);
}

TempoFxSection::TempoFxSection (dsp::ParameterStore& parameterStore)
    : store (parameterStore),
      reverbReturn (
          parameterStore, dsp::parameterIndex (dsp::GlobalControl::reverbReturn), juce::Slider::LinearHorizontal),
      delayFeedback (
          parameterStore, dsp::parameterIndex (dsp::GlobalControl::delayFeedback), juce::Slider::LinearHorizontal),
      delayReturn (
          parameterStore, dsp::parameterIndex (dsp::GlobalControl::delayReturn), juce::Slider::LinearHorizontal)
{
    setName ("Tempo and effects");

    halve.setButtonText (juce::String::fromUTF8 ("\xc3\x97\xc2\xbd"));
    doubleUp.setButtonText (juce::String::fromUTF8 ("\xc3\x97") + "2");

    tempoLabel.setText ("Tempo (BPM)", juce::dontSendNotification);
    tempoLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    reverbLabel.setText ("Reverb", juce::dontSendNotification);
    reverbLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    delayLabel.setText ("Delay", juce::dontSendNotification);
    delayLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    divisionLabel.setText ("Division", juce::dontSendNotification);
    feedbackLabel.setText ("Feedback", juce::dontSendNotification);
    delayReturnLabel.setText ("Return", juce::dontSendNotification);
    tempoNote.setFont (juce::FontOptions (12.5f));
    tempoNote.setMinimumHorizontalScale (1.0f);

    bpmEntry.setMultiLine (false);
    bpmEntry.setInputRestrictions (7, "0123456789.");
    bpmEntry.setJustification (juce::Justification::centred);
    bpmEntry.setTitle ("Tempo in beats per minute");
    bpmEntry.setTooltip ("Type a tempo from 20 to 400 and press Enter");
    bpmEntry.onReturnKey = [this] { commitTempoText(); };
    bpmEntry.onFocusLost = [this] { commitTempoText(); };
    bpmEntry.onEscapeKey = [this] { showTempo(); };

    halve.setTooltip ("Half time: use half of the current tempo");
    doubleUp.setTooltip ("Double time: use twice the current tempo");
    useDetected.setTooltip ("Go back to the tempo the app detected");
    halve.onClick = [this] { chooseHalfOrDouble (true); };
    doubleUp.onClick = [this] { chooseHalfOrDouble (false); };
    useDetected.onClick = [this]
    {
        if (onTempoCleared)
            onTempoCleared();
    };

    for (int i = 0; i < dsp::numDelayDivisions; ++i)
        division.addItem (
            dsp::formatDisplay (dsp::parameterIndex (dsp::GlobalControl::delayDivision), static_cast<float> (i)),
            i + 1);

    division.setTitle ("Delay division");
    division.setHasFocusOutline (true);
    division.resetText = "Reset division to "
                       + dsp::formatDisplay (
                             dsp::parameterIndex (dsp::GlobalControl::delayDivision),
                             dsp::parameterSpec (dsp::parameterIndex (dsp::GlobalControl::delayDivision)).defaultValue);
    division.onReset = [this]
    {
        store.set (dsp::GlobalControl::delayDivision,
                   dsp::parameterSpec (dsp::parameterIndex (dsp::GlobalControl::delayDivision)).defaultValue);
        syncFromStore();
    };
    division.onChange = [this]
    {
        if (! syncing)
            store.set (dsp::GlobalControl::delayDivision, static_cast<float> (division.getSelectedId() - 1));
    };

    halve.setTitle ("Half time");
    doubleUp.setTitle ("Double time");
    useDetected.setTitle ("Use the detected tempo");
    bpmEntry.setHasFocusOutline (true);

    // Tab order: tempo box, half, double, detected, reverb return, division, delay return, feedback.
    bpmEntry.setExplicitFocusOrder (1);
    halve.setExplicitFocusOrder (2);
    doubleUp.setExplicitFocusOrder (3);
    useDetected.setExplicitFocusOrder (4);
    reverbReturn.setExplicitFocusOrder (5);
    division.setExplicitFocusOrder (6);
    delayReturn.setExplicitFocusOrder (7);
    delayFeedback.setExplicitFocusOrder (8);

    reverbReturn.setTitle ("Reverb return");
    delayFeedback.setTitle ("Delay feedback");
    delayReturn.setTitle ("Delay return");

    for (auto* component : std::initializer_list<juce::Component*> { &tempoLabel,
                                                                     &tempoNote,
                                                                     &reverbLabel,
                                                                     &delayLabel,
                                                                     &divisionLabel,
                                                                     &feedbackLabel,
                                                                     &delayReturnLabel,
                                                                     &bpmEntry,
                                                                     &halve,
                                                                     &doubleUp,
                                                                     &useDetected,
                                                                     &reverbReturn,
                                                                     &delayFeedback,
                                                                     &delayReturn,
                                                                     &division })
        addAndMakeVisible (component);

    useDetected.setVisible (false);
    syncFromStore();
    setTempoInfo ({});
}

juce::String TempoFxSection::describeTempo (const TempoInfo& tempo, double currentBpm)
{
    if (tempo.overridden)
        return "Set by you"
             + (tempo.detected ? " (detected: " + bpmText (*tempo.detected) + ")" : juce::String (" (none detected)"));

    if (tempo.detected)
        return "Detected from the song";

    return "Not detected: using " + bpmText (currentBpm) + " BPM until you set one";
}

void TempoFxSection::setTempoInfo (const TempoInfo& tempo)
{
    info = tempo;
    useDetected.setVisible (info.overridden);
    showTempo();
    resized();
}

void TempoFxSection::showTempo()
{
    const auto current = static_cast<double> (store.get (tempoIndex()));

    if (! bpmEntry.hasKeyboardFocus (true))
    {
        shownTempoText = bpmText (current);
        bpmEntry.setText (shownTempoText, false);
    }

    tempoNote.setText (describeTempo (info, current), juce::dontSendNotification);
    tempoNote.setColour (
        juce::Label::textColourId,
        (! info.overridden && ! info.detected) ? warning : findColour (juce::Label::textColourId).withAlpha (0.8f));
}

void TempoFxSection::syncFromStore()
{
    const juce::ScopedValueSetter<bool> guard (syncing, true);
    reverbReturn.syncFromStore();
    delayFeedback.syncFromStore();
    delayReturn.syncFromStore();
    division.setSelectedId (
        static_cast<int> (std::lround (store.get (dsp::parameterIndex (dsp::GlobalControl::delayDivision)))) + 1,
        juce::dontSendNotification);
    showTempo();
}

void TempoFxSection::commitTempoText()
{
    const auto text = bpmEntry.getText().trim();
    const auto value = text.getDoubleValue();

    if (text.isEmpty()
        || ! (value >= static_cast<double> (dsp::tempoMinBpm) && value <= static_cast<double> (dsp::tempoMaxBpm)))
    {
        tempoNote.setText ("Tempo must be from " + juce::String (static_cast<int> (dsp::tempoMinBpm)) + " to "
                               + juce::String (static_cast<int> (dsp::tempoMaxBpm)) + " BPM.",
                           juce::dontSendNotification);
        tempoNote.setColour (juce::Label::textColourId, warning);
        shownTempoText = bpmText (static_cast<double> (store.get (tempoIndex())));
        bpmEntry.setText (shownTempoText, false);
        return;
    }

    // Return and losing focus both commit. Only text the user changed counts: the box shows the tempo
    // rounded to one decimal, so committing an untouched box would turn 179.84 into a "set by you" 179.8.
    if (text == shownTempoText)
        return;

    if (std::abs (value - static_cast<double> (store.get (tempoIndex()))) < 1.0e-3)
        return; // typed the tempo it already has

    if (onTempoChosen)
        onTempoChosen (value, "numeric");
}

void TempoFxSection::chooseHalfOrDouble (bool half)
{
    const auto current = store.get (tempoIndex());
    const auto next = half ? dsp::halveTempo (current) : dsp::doubleTempo (current);

    if (std::abs (next - current) < 1.0e-3f)
        return; // already at the end of the range

    if (onTempoChosen)
        onTempoChosen (static_cast<double> (next), half ? "half" : "double");
}

void TempoFxSection::paint (juce::Graphics& g)
{
    g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.1f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}

void TempoFxSection::resized()
{
    auto area = getLocalBounds().reduced (pad);
    const auto third = (area.getWidth() - 2 * pad) / 3;

    auto tempoArea = area.removeFromLeft (third + 40);
    area.removeFromLeft (pad);
    tempoLabel.setBounds (tempoArea.removeFromTop (20));
    auto row = tempoArea.removeFromTop (28);
    bpmEntry.setBounds (row.removeFromLeft (72));
    row.removeFromLeft (6);
    halve.setBounds (row.removeFromLeft (44));
    row.removeFromLeft (4);
    doubleUp.setBounds (row.removeFromLeft (44));
    row.removeFromLeft (6);
    useDetected.setBounds (row.removeFromLeft (80));
    tempoNote.setBounds (tempoArea.removeFromTop (22));

    auto reverbArea = area.removeFromLeft (third - 40);
    area.removeFromLeft (pad);
    reverbLabel.setBounds (reverbArea.removeFromTop (20));
    reverbReturn.setBounds (reverbArea.removeFromTop (28));

    delayLabel.setBounds (area.removeFromTop (20));
    auto first = area.removeFromTop (28);
    divisionLabel.setBounds (first.removeFromLeft (64));
    division.setBounds (first.removeFromLeft (110));
    first.removeFromLeft (10);
    delayReturnLabel.setBounds (first.removeFromLeft (48));
    delayReturn.setBounds (first);
    auto second = area.removeFromTop (28);
    feedbackLabel.setBounds (second.removeFromLeft (64));
    delayFeedback.setBounds (second);
}
} // namespace snds
