#pragma once

#include "ui/MixControls.h"

#include <functional>
#include <optional>

namespace snds
{
/** A combo box that goes back to its default with Delete or Backspace, or "Reset" in the right-click
    menu (a click with the main button opens the list as usual). */
class ResettableComboBox final : public juce::ComboBox
{
public:
    std::function<void()> onReset;
    juce::String resetText { "Reset" };

    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& event) override;
};

/** The strip under the stem cards: tempo (shown, typed, halved or doubled), reverb return, and
    the delay's division, feedback and return.

    Tempo is the song's one global BPM. Choosing a value reports it through onTempoChosen; the
    owner writes it to the mix (tempo.bpm) and stores it in the song's package as the user's
    override. The detected tempo, or the 120 BPM default when nothing was detected, is flagged.
*/
class TempoFxSection final : public juce::Component
{
public:
    explicit TempoFxSection (dsp::ParameterStore& store);

    /** What is known about the open song's tempo. */
    struct TempoInfo
    {
        std::optional<double> detected; // none: detection failed
        bool overridden = false;        // the user set the tempo
    };

    void setTempoInfo (const TempoInfo& info);
    void syncFromStore();

    /** `mode` is "numeric", "half" or "double", as stored in song.json. */
    std::function<void (double bpm, const juce::String& mode)> onTempoChosen;

    /** The user asked to go back to the detected tempo (or the default). */
    std::function<void()> onTempoCleared;

    /** The one-line tempo note for an info (also used by tests). */
    static juce::String describeTempo (const TempoInfo& info, double currentBpm);

    // Direct access for tests.
    juce::TextEditor& tempoEditor() noexcept { return bpmEntry; }
    juce::Button& halveButton() noexcept { return halve; }
    juce::Button& doubleButton() noexcept { return doubleUp; }
    juce::Button& detectedButton() noexcept { return useDetected; }
    ResettableComboBox& divisionBox() noexcept { return division; }
    const juce::Label& noteLabel() const noexcept { return tempoNote; }

    /** Applies the text in the tempo box as if the user pressed Enter. */
    void commitTempoText();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showTempo();
    void chooseHalfOrDouble (bool half);

    dsp::ParameterStore& store;
    TempoInfo info;

    juce::Label tempoLabel, tempoNote, reverbLabel, delayLabel, divisionLabel, feedbackLabel, delayReturnLabel;
    juce::TextEditor bpmEntry;
    juce::String
        shownTempoText; // what the box showed when SNDS last set it; the user has not edited it while it matches
    KeyButton halve, doubleUp, useDetected { "Detected" };
    ParameterSlider reverbReturn, delayFeedback, delayReturn;
    ResettableComboBox division;
    bool syncing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoFxSection)
};
} // namespace snds
