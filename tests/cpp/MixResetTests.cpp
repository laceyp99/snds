// Exact entry, keyboard operation, per-control reset, focus order and "Reset mix" for the mix
// controls. Fast, offline; the UI components run without a window.

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FakeAudioDevice.h"
#include "app/MixService.h"
#include "ui/MixPanel.h"
#include "ui/StemCard.h"

#include <vector>

namespace snds
{
namespace
{
using namespace snds::test;

int indexOf (dsp::Stem stem, dsp::StemControl control)
{
    return dsp::parameterIndex (stem, control);
}

int indexOf (dsp::GlobalControl control)
{
    return dsp::parameterIndex (control);
}

juce::KeyPress key (int code, int modifiers = 0)
{
    return juce::KeyPress (code, juce::ModifierKeys (modifiers), 0);
}

juce::MouseEvent doubleClickOn (juce::Component& component)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                             juce::Point<float> (4.0f, 4.0f),
                             juce::ModifierKeys(),
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             &component,
                             &component,
                             now,
                             juce::Point<float> (4.0f, 4.0f),
                             now,
                             2,
                             false);
}

juce::Label* textBoxOf (juce::Slider& slider)
{
    for (auto* child : slider.getChildren())
        if (auto* label = dynamic_cast<juce::Label*> (child))
            return label;

    return nullptr;
}

class MixResetTests final : public juce::UnitTest
{
public:
    MixResetTests()
        : juce::UnitTest ("MixReset", "snds")
    {
    }

    void runTest() override
    {
        MessageLoop loop;
        testExactEntry();
        testTypingStartsFromTheExactValue();
        testSliderKeyboard();
        testToggleKeyboard();
        testDoubleClickAndMenu();
        testComboBoxReset();
        testFocusOrder();
        testResetMix();
    }

private:
    //==============================================================================
    struct EntryCase
    {
        int index;
        const char* text;
        float expected;
        float tolerance;
    };

    void testExactEntry()
    {
        beginTest ("MixReset: every control takes exact typed values in its own words");
        dsp::ParameterStore store;

        const EntryCase cases[] = {
            { indexOf (dsp::Stem::bass, dsp::StemControl::gain), "-6", -6.0f, 1e-3f },
            { indexOf (dsp::Stem::bass, dsp::StemControl::gain), "-3.25 dB", -3.25f, 1e-3f },
            { indexOf (dsp::Stem::bass, dsp::StemControl::gain), "-inf", dsp::gainMinDb, 0.0f },
            { indexOf (dsp::Stem::bass, dsp::StemControl::gain), "12", dsp::gainMaxDb, 1e-3f }, // clamped
            { indexOf (dsp::Stem::drums, dsp::StemControl::pan), "L30", -0.3f, 1e-3f },
            { indexOf (dsp::Stem::drums, dsp::StemControl::pan), "r100", 1.0f, 1e-3f },
            { indexOf (dsp::Stem::drums, dsp::StemControl::pan), "c", 0.0f, 1e-3f },
            { indexOf (dsp::Stem::other, dsp::StemControl::lowCut), "250", 250.0f, 0.5f },
            { indexOf (dsp::Stem::other, dsp::StemControl::lowCut), "0.4k", 400.0f, 0.5f },
            { indexOf (dsp::Stem::other, dsp::StemControl::lowCut), "off", dsp::lowCutMinHz, 0.0f },
            { indexOf (dsp::Stem::other, dsp::StemControl::highCut), "8 kHz", 8000.0f, 0.5f },
            { indexOf (dsp::Stem::other, dsp::StemControl::highCut), "off", dsp::highCutMaxHz, 0.0f },
            { indexOf (dsp::Stem::vocals, dsp::StemControl::reverbSend), "35%", 0.35f, 1e-3f },
            { indexOf (dsp::Stem::vocals, dsp::StemControl::delaySend), "35", 0.35f, 1e-3f },
            { indexOf (dsp::GlobalControl::reverbReturn), "50", 0.5f, 1e-3f },
            { indexOf (dsp::GlobalControl::delayReturn), "0", 0.0f, 0.0f },
            { indexOf (dsp::GlobalControl::delayFeedback), "45", 0.45f, 1e-3f },
            { indexOf (dsp::GlobalControl::delayFeedback), "90", dsp::maxFeedback, 1e-3f }, // the safe maximum
            { indexOf (dsp::GlobalControl::masterGain), "-3.5", -3.5f, 1e-3f },
        };

        for (const auto& c : cases)
        {
            ParameterSlider slider (store, c.index, juce::Slider::LinearHorizontal);
            slider.setValue (slider.getValueFromText (c.text), juce::sendNotificationSync);
            expectWithinAbsoluteError (store.get (c.index),
                                       c.expected,
                                       c.tolerance,
                                       juce::String (c.text) + " -> parameter " + juce::String (c.index));
        }

        ParameterSlider gain (store, indexOf (dsp::Stem::bass, dsp::StemControl::gain), juce::Slider::LinearVertical);
        gain.setValue (gain.getValueFromText ("-4"), juce::sendNotificationSync);
        gain.setValue (gain.getValueFromText ("loud"), juce::sendNotificationSync);
        gain.setValue (gain.getValueFromText (""), juce::sendNotificationSync);
        expectWithinAbsoluteError (store.get (indexOf (dsp::Stem::bass, dsp::StemControl::gain)),
                                   -4.0f,
                                   1e-3f,
                                   "unreadable text changes nothing");
    }

    void testTypingStartsFromTheExactValue()
    {
        beginTest ("MixReset: the number opens for typing with the precise value, not the rounded one");
        dsp::ParameterStore store;
        const auto index = indexOf (dsp::Stem::drums, dsp::StemControl::gain);
        store.set (index, -3.25f);
        ParameterSlider slider (store, index, juce::Slider::LinearVertical);
        slider.setSize (60, 160);
        expectEquals (slider.exactText(), dsp::formatExact (index, -3.25f));
        expect (slider.exactText() != slider.getTextFromValue (slider.getValue()), "display rounds, typing does not");

        auto* label = textBoxOf (slider);
        expect (label != nullptr);

        if (label != nullptr)
        {
            label->showEditor();
            expect (label->getCurrentTextEditor() != nullptr);

            if (auto* editor = label->getCurrentTextEditor())
                expectEquals (editor->getText(), slider.exactText());

            label->hideEditor (true);
        }

        const auto cutIndex = indexOf (dsp::Stem::drums, dsp::StemControl::highCut);
        store.set (cutIndex, 1203.5f);
        ParameterSlider cut (store, cutIndex, juce::Slider::LinearHorizontal);
        expectEquals (cut.exactText(), dsp::formatExact (cutIndex, 1203.5f));
    }

    void testSliderKeyboard()
    {
        beginTest ("MixReset: arrows, Shift and Page keys, Home/End, Delete and Return work on a focused slider");
        dsp::ParameterStore store;
        const auto gain = indexOf (dsp::Stem::bass, dsp::StemControl::gain);
        ParameterSlider slider (store, gain, juce::Slider::LinearVertical);
        slider.setSize (60, 160);

        expect (slider.keyPressed (key (juce::KeyPress::downKey)));
        expectWithinAbsoluteError (store.get (gain), -0.5f, 1e-3f);
        expect (slider.keyPressed (key (juce::KeyPress::leftKey)));
        expectWithinAbsoluteError (store.get (gain), -1.0f, 1e-3f);
        expect (slider.keyPressed (key (juce::KeyPress::upKey)));
        expect (slider.keyPressed (key (juce::KeyPress::rightKey)));
        expectWithinAbsoluteError (store.get (gain), 0.0f, 1e-3f);
        expect (slider.keyPressed (key (juce::KeyPress::pageDownKey)));
        expectWithinAbsoluteError (store.get (gain), -3.0f, 1e-3f);
        expect (slider.keyPressed (key (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier)));
        expectWithinAbsoluteError (store.get (gain), 0.0f, 1e-3f, "Shift makes the step coarse");
        expect (slider.keyPressed (key (juce::KeyPress::pageUpKey)));
        expectWithinAbsoluteError (store.get (gain), 3.0f, 1e-3f);
        expect (slider.keyPressed (key (juce::KeyPress::endKey)));
        expectEquals (store.get (gain), dsp::gainMaxDb);
        expect (slider.keyPressed (key (juce::KeyPress::upKey)));
        expectEquals (store.get (gain), dsp::gainMaxDb, "stops at the top");
        expect (slider.keyPressed (key (juce::KeyPress::homeKey)));
        expectEquals (store.get (gain), dsp::gainMinDb);
        expectEquals (slider.getTextFromValue (slider.getValue()), juce::String ("-inf dB"));
        expect (slider.keyPressed (key (juce::KeyPress::deleteKey)));
        expectEquals (store.get (gain), 0.0f, "Delete resets");
        slider.setValue (0.3, juce::sendNotificationSync);
        expect (slider.keyPressed (key (juce::KeyPress::backspaceKey)));
        expectEquals (store.get (gain), 0.0f, "so does Backspace");
        expect (! slider.keyPressed (key (juce::KeyPress::tabKey)), "Tab is left for the focus system");
        expect (! slider.keyPressed (key ('c', juce::ModifierKeys::commandModifier)), "shortcuts are not swallowed");

        expect (slider.keyPressed (key (juce::KeyPress::returnKey)));
        auto* label = textBoxOf (slider);
        expect (label != nullptr && label->getCurrentTextEditor() != nullptr, "Return opens the number for typing");

        if (label != nullptr)
            label->hideEditor (true);

        // A log-frequency control moves in even musical steps and stops at its ends.
        const auto cut = indexOf (dsp::Stem::bass, dsp::StemControl::lowCut);
        ParameterSlider lowCut (store, cut, juce::Slider::LinearHorizontal);
        expect (lowCut.keyPressed (key (juce::KeyPress::leftKey)));
        expectEquals (store.get (cut), dsp::lowCutMinHz, "already at the bypassed end");
        expect (lowCut.keyPressed (key (juce::KeyPress::rightKey)));
        const auto first = store.get (cut);
        expect (first > dsp::lowCutMinHz && first < 30.0f, "one fine step up from 20 Hz: " + juce::String (first));
        expect (lowCut.keyPressed (key (juce::KeyPress::pageUpKey)));
        expect (store.get (cut) > first * 1.3f);
        expect (lowCut.keyPressed (key (juce::KeyPress::endKey)));
        expectEquals (store.get (cut), dsp::lowCutMaxHz);

        const auto pan = indexOf (dsp::Stem::bass, dsp::StemControl::pan);
        ParameterSlider panSlider (store, pan, juce::Slider::LinearHorizontal);
        expect (panSlider.keyPressed (key (juce::KeyPress::rightKey)));
        expectWithinAbsoluteError (store.get (pan), 0.05f, 1e-3f);
        expect (panSlider.keyPressed (key (juce::KeyPress::pageDownKey)));
        expectWithinAbsoluteError (store.get (pan), -0.2f, 1e-3f);

        const auto send = indexOf (dsp::Stem::bass, dsp::StemControl::reverbSend);
        ParameterSlider sendSlider (store, send, juce::Slider::LinearHorizontal);
        expect (sendSlider.keyPressed (key (juce::KeyPress::upKey)));
        expectWithinAbsoluteError (store.get (send), 0.01f, 1e-3f);

        slider.setEnabled (false);
        expect (! slider.keyPressed (key (juce::KeyPress::deleteKey)), "a disabled control ignores keys");
    }

    void testToggleKeyboard()
    {
        beginTest ("MixReset: Space and Return toggle mute and solo; Delete turns them off");
        dsp::ParameterStore store;
        const auto mute = indexOf (dsp::Stem::vocals, dsp::StemControl::mute);
        ParameterToggle button (store, mute, "M");

        expect (button.keyPressed (key (juce::KeyPress::spaceKey)));
        expectEquals (store.get (mute), 1.0f);
        expect (button.getToggleState());
        expect (button.keyPressed (key (juce::KeyPress::spaceKey)));
        expectEquals (store.get (mute), 0.0f);
        expect (button.keyPressed (key (juce::KeyPress::returnKey)));
        expectEquals (store.get (mute), 1.0f);
        expect (button.keyPressed (key (juce::KeyPress::deleteKey)));
        expectEquals (store.get (mute), 0.0f);
        expect (! button.getToggleState());
        expect (! button.keyPressed (key (juce::KeyPress::tabKey)));

        button.setEnabled (false);
        expect (! button.keyPressed (key (juce::KeyPress::spaceKey)));
        expectEquals (store.get (mute), 0.0f);

        // The More button also answers Space.
        KeyButton more ("More");
        int clicks = 0;
        more.onClick = [&] { ++clicks; };
        expect (more.keyPressed (key (juce::KeyPress::spaceKey)));
        MessageLoop::pump (40);
        expectEquals (clicks, 1);
    }

    void testDoubleClickAndMenu()
    {
        beginTest ("MixReset: double-click returns a control to its default; the right-click menu offers Reset");
        dsp::ParameterStore store;

        for (const auto& [index, value] :
             { std::pair { indexOf (dsp::Stem::drums, dsp::StemControl::gain), -9.0f },
               std::pair { indexOf (dsp::Stem::drums, dsp::StemControl::pan), 0.6f },
               std::pair { indexOf (dsp::Stem::drums, dsp::StemControl::lowCut), 300.0f },
               std::pair { indexOf (dsp::Stem::drums, dsp::StemControl::highCut), 4000.0f },
               std::pair { indexOf (dsp::Stem::drums, dsp::StemControl::reverbSend), 0.5f },
               std::pair { indexOf (dsp::GlobalControl::delayFeedback), 0.5f },
               std::pair { indexOf (dsp::GlobalControl::masterGain), 4.0f } })
        {
            ParameterSlider slider (store, index, juce::Slider::LinearHorizontal);
            slider.setSize (200, 30);
            store.set (index, value);
            slider.syncFromStore();
            expect (store.get (index) != dsp::parameterSpec (index).defaultValue);
            slider.mouseDoubleClick (doubleClickOn (slider));
            expectEquals (store.get (index),
                          dsp::parameterSpec (index).defaultValue,
                          "double-click: parameter " + juce::String (index));
            expectWithinAbsoluteError (static_cast<float> (slider.getValue()),
                                       dsp::toNormalised (index, dsp::parameterSpec (index).defaultValue),
                                       1e-6f);
        }

        const auto gain = indexOf (dsp::Stem::other, dsp::StemControl::gain);
        ParameterSlider slider (store, gain, juce::Slider::LinearVertical);
        const auto menu = slider.buildContextMenu();
        expectEquals (menu.getNumItems(), 2);
        juce::PopupMenu::MenuItemIterator items (menu);
        juce::StringArray labels;

        while (items.next())
            labels.add (items.getItem().text);

        expect (labels[0].startsWith ("Reset") && labels[0].contains ("0.0 dB"), labels[0]);
        expect (labels[1].contains ("exact value"));

        store.set (gain, -20.0f);
        slider.syncFromStore();
        slider.performContextAction (ParameterSlider::menuReset);
        expectEquals (store.get (gain), 0.0f, "menu Reset");
        slider.performContextAction (ParameterSlider::menuEnterValue);
        auto* label = textBoxOf (slider);
        expect (label != nullptr && label->getCurrentTextEditor() != nullptr, "menu: enter exact value");

        if (label != nullptr)
            label->hideEditor (true);

        const auto solo = indexOf (dsp::Stem::other, dsp::StemControl::solo);
        ParameterToggle toggle (store, solo, "S");
        store.set (solo, 1.0f);
        toggle.syncFromStore();
        expectEquals (toggle.buildContextMenu().getNumItems(), 1);
        toggle.performContextAction (ParameterSlider::menuReset);
        expectEquals (store.get (solo), 0.0f);
        expect (! toggle.getToggleState());

        slider.setEnabled (false);
        store.set (gain, -5.0f);
        slider.mouseDoubleClick (doubleClickOn (slider));
        expectEquals (store.get (gain), -5.0f, "a disabled control ignores double-click");
    }

    void testComboBoxReset()
    {
        beginTest ("MixReset: the delay division returns to 1/8 with Delete or its menu");
        dsp::ParameterStore store;
        TempoFxSection section (store);
        section.setSize (1000, 100);
        const auto division = indexOf (dsp::GlobalControl::delayDivision);

        section.divisionBox().setSelectedId (5, juce::sendNotificationSync);
        expectEquals (store.get (division), 4.0f);
        expect (section.divisionBox().keyPressed (key (juce::KeyPress::deleteKey)));
        expectEquals (store.get (division), static_cast<float> (dsp::DelayDivision::eighth));
        expectEquals (section.divisionBox().getSelectedId(), static_cast<int> (dsp::DelayDivision::eighth) + 1);
        expect (section.divisionBox().resetText.contains ("1/8"), section.divisionBox().resetText);

        section.divisionBox().setSelectedId (2, juce::sendNotificationSync);
        section.divisionBox().onReset();
        expectEquals (store.get (division), static_cast<float> (dsp::DelayDivision::eighth));
    }

    //==============================================================================
    static juce::StringArray tabOrder (juce::Component& root)
    {
        juce::FocusTraverser traverser;
        juce::StringArray titles;

        for (auto* component : traverser.getAllComponents (&root))
            if (component->getWantsKeyboardFocus())
                titles.add (component->getTitle().isNotEmpty() ? component->getTitle() : component->getName());

        return titles;
    }

    void testFocusOrder()
    {
        beginTest ("MixReset: Tab walks card by card, then master, tempo and effects, then Reset mix");
        MixService mix;
        const auto stage = mix.stageFactory()(); // a processor exists: a song is "loaded"
        MixPanel panel (mix);
        panel.setSize (1100, panel.getPreferredHeight());
        panel.refresh();
        expect (panel.isEnabled());

        const auto order = tabOrder (panel);
        const juce::StringArray expected { "Drums gain",    "Drums mute",
                                           "Drums solo",    "Drums more controls",
                                           "Bass gain",     "Bass mute",
                                           "Bass solo",     "Bass more controls",
                                           "Other gain",    "Other mute",
                                           "Other solo",    "Other more controls",
                                           "Vocals gain",   "Vocals mute",
                                           "Vocals solo",   "Vocals more controls",
                                           "Master gain",   "Tempo in beats per minute",
                                           "Half time",     "Double time",
                                           "Reverb return", "Delay division",
                                           "Delay return",  "Delay feedback",
                                           "Reset mix" };
        expectEquals (order.joinIntoString (" | "), expected.joinIntoString (" | "));

        // An expanded card adds its five controls right after its own More button.
        panel.stemCards()[1]->setExpanded (true);
        const auto expanded = tabOrder (panel);
        const auto more = expanded.indexOf ("Bass more controls");
        expect (more >= 0);
        expectEquals (expanded[more + 1], juce::String ("Bass Pan"));
        expectEquals (expanded[more + 2], juce::String ("Bass Low cut"));
        expectEquals (expanded[more + 3], juce::String ("Bass High cut"));
        expectEquals (expanded[more + 4], juce::String ("Bass Reverb"));
        expectEquals (expanded[more + 5], juce::String ("Bass Delay"));
        expectEquals (expanded[more + 6], juce::String ("Other gain"));

        // The Detected button only takes focus when it is shown.
        TempoFxSection::TempoInfo info;
        info.detected = 140.0;
        info.overridden = true;
        panel.tempoFx().setTempoInfo (info);
        expect (tabOrder (panel).contains ("Use the detected tempo"));

        // Without a song the controls are disabled and out of the tab order.
        MixService empty;
        MixPanel idle (empty);
        idle.setSize (1100, idle.getPreferredHeight());
        idle.refresh();
        expect (! idle.isEnabled());
    }

    //==============================================================================
    static void disturb (dsp::ParameterStore& store)
    {
        for (int stem = 0; stem < 4; ++stem)
        {
            const auto s = static_cast<dsp::Stem> (stem);
            store.set (s, dsp::StemControl::gain, -6.0f - static_cast<float> (stem));
            store.set (s, dsp::StemControl::pan, 0.4f);
            store.set (s, dsp::StemControl::lowCut, 300.0f);
            store.set (s, dsp::StemControl::highCut, 5000.0f);
            store.set (s, dsp::StemControl::reverbSend, 0.5f);
            store.set (s, dsp::StemControl::delaySend, 0.5f);
        }

        store.set (dsp::Stem::drums, dsp::StemControl::mute, 1.0f);
        store.set (dsp::Stem::bass, dsp::StemControl::solo, 1.0f);
        store.set (dsp::GlobalControl::reverbReturn, 0.3f);
        store.set (dsp::GlobalControl::delayReturn, 0.2f);
        store.set (dsp::GlobalControl::delayFeedback, 0.5f);
        store.set (dsp::GlobalControl::delayDivision, 4.0f);
        store.set (dsp::GlobalControl::masterGain, -4.0f);
        store.set (dsp::GlobalControl::tempoBpm, 77.0f);
    }

    static OpenSong makeSong (std::optional<double> detected, std::optional<double> override)
    {
        OpenSong song;
        song.entry.song = SongMetadata {};
        song.entry.song->bpm.value = detected;

        if (override)
            song.entry.song->bpm.userOverride = SongMetadata::BpmOverride { *override, "numeric" };

        return song;
    }

    void testResetMix()
    {
        beginTest ("MixReset: Reset mix asks first; Yes sets every control to its default and the tempo to the song's");
        MixService mix;
        const auto stage = mix.stageFactory()();
        MixPanel panel (mix);
        panel.setSize (1100, panel.getPreferredHeight());
        auto& store = mix.parameters();

        panel.setSong (makeSong (143.2, std::nullopt));
        panel.refresh();
        disturb (store);
        panel.refresh();
        expectEquals (panel.stemCards()[0]->gainSlider().getParameterValue(), -6.0f);
        expect (panel.stemCards()[0]->muteButton().getToggleState());

        juce::String asked;
        std::function<void()> pending;
        panel.confirmReset = [&] (const juce::String& question, std::function<void()> proceed)
        {
            asked = question;
            pending = std::move (proceed);
        };

        // The button asks; nothing changes until the user agrees.
        panel.resetButton().triggerClick();
        MessageLoop::pump (40);
        expect (asked.contains ("143.2 BPM") && asked.contains ("detected tempo")
                    && asked.contains ("cannot be undone"),
                asked);
        expectEquals (
            store.get (indexOf (dsp::GlobalControl::masterGain)), -4.0f, "declined or pending: nothing changed");
        pending = {}; // the user said no

        panel.requestResetMix();
        expect (static_cast<bool> (pending));
        const auto before = store.version();
        pending();
        expectEquals (static_cast<int> (store.version() - before), 1, "one transaction");

        for (int i = 0; i < dsp::numParameters; ++i)
            if (i != indexOf (dsp::GlobalControl::tempoBpm))
                expectEquals (store.get (i), dsp::parameterSpec (i).defaultValue, "parameter " + juce::String (i));

        expectWithinAbsoluteError (store.get (indexOf (dsp::GlobalControl::tempoBpm)), 143.2f, 1e-3f);

        // The controls on screen follow at once.
        expectEquals (panel.stemCards()[0]->gainSlider().getParameterValue(), 0.0f);
        expect (! panel.stemCards()[0]->muteButton().getToggleState());
        expect (! panel.stemCards()[1]->soloButton().getToggleState());
        expectEquals (panel.master().gainSlider().getParameterValue(), 0.0f);
        expectEquals (panel.tempoFx().divisionBox().getSelectedId(), static_cast<int> (dsp::DelayDivision::eighth) + 1);
        expectEquals (panel.tempoFx().tempoEditor().getText(), juce::String ("143.2"));

        beginTest ("MixReset: the tempo a reset returns to is the user's setting, else the detected one, else 120");
        panel.setSong (makeSong (143.2, 90.0));
        disturb (store);
        panel.resetMix();
        expectWithinAbsoluteError (store.get (indexOf (dsp::GlobalControl::tempoBpm)), 90.0f, 1e-3f);
        expect (MixPanel::describeReset (90.0, { 143.2, true }).contains ("the tempo you set"));

        panel.setSong (makeSong (std::nullopt, std::nullopt));
        disturb (store);
        panel.resetMix();
        expectWithinAbsoluteError (store.get (indexOf (dsp::GlobalControl::tempoBpm)), 120.0f, 1e-3f);
        expect (MixPanel::describeReset (120.0, {}).contains ("the default tempo"));

        panel.setSong (std::nullopt);
        disturb (store);
        panel.resetMix();
        expectWithinAbsoluteError (
            store.get (indexOf (dsp::GlobalControl::tempoBpm)), 120.0f, 1e-3f, "no song: the default");

        beginTest ("MixReset: MixService::resetMix is a single transaction with the given tempo");
        disturb (store);
        const auto version = store.version();
        mix.resetMix (133.0);
        expectEquals (static_cast<int> (store.version() - version), 1);
        expectWithinAbsoluteError (store.get (indexOf (dsp::GlobalControl::tempoBpm)), 133.0f, 1e-3f);
        expectEquals (store.get (indexOf (dsp::Stem::vocals, dsp::StemControl::gain)), 0.0f);
    }
};

MixResetTests mixResetTests;
} // namespace
} // namespace snds
