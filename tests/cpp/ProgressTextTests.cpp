#include <juce_core/juce_core.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/ProgressText.h"
#include "ui/ImportPanel.h"

namespace snds
{
namespace
{
juce::TextButton* findButton (juce::Component& parent, const juce::String& text)
{
    for (auto* child : parent.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child);
            button != nullptr && button->getButtonText() == text)
            return button;

    return nullptr;
}

SessionStatus preparing (const juce::String& stage, std::optional<double> eta)
{
    SessionStatus status;
    status.state = SessionState::preparing;
    status.stage = stage;
    status.etaSeconds = eta;
    status.device = "cuda";
    return status;
}

class ProgressTextTests final : public juce::UnitTest
{
public:
    ProgressTextTests()
        : juce::UnitTest ("ProgressText", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("ProgressText: spans read as short words and round once past a minute");
        expectEquals (formatSpan (0.2), juce::String ("0 s"));
        expectEquals (formatSpan (45.0), juce::String ("45 s"));
        expectEquals (formatSpan (60.0), juce::String ("1 min"));
        expectEquals (formatSpan (83.0), juce::String ("1 min 25 s"));
        expectEquals (formatSpan (125.0), juce::String ("2 min 5 s"));
        expectEquals (formatSpan (7500.0), juce::String ("2 h 05 min"));

        beginTest ("ProgressText: an estimate is shown only when it is present, sane, and the job is running");
        expect (! credibleEta (preparing ("Decoding", std::nullopt)).has_value());
        expect (credibleEta (preparing ("Separating", 80.0)).has_value());
        expect (! credibleEta (preparing ("Separating", 0.2)).has_value(), "under a second is noise");
        expect (! credibleEta (preparing ("Separating", 7.0 * 3600.0)).has_value(),
                "hours-long guesses are not credible");
        expect (! credibleEta (preparing ("Separating", std::nan (""))).has_value());

        auto cancelling = preparing ("Separating", 80.0);
        cancelling.state = SessionState::cancelling;
        expect (! credibleEta (cancelling).has_value(), "a cancelling job promises nothing");

        beginTest ("ProgressText: the progress line names the step's estimate as a step estimate, never a total");
        const auto line = describeProgress (preparing ("Separating stems (2 of 6)", 80.0), 42.0);
        expect (line.contains ("about 1 min 20 s left in this step"), line);
        expect (line.contains ("GPU"), line);
        expect (line.contains ("42 s so far"), line);
        expect (! line.contains ("remaining"), line);

        const auto early = describeProgress (preparing ("Decoding (1 of 6)", std::nullopt), 0.4);
        expect (! early.contains ("left"), "no estimate before the worker can measure: " + early);
        expect (! early.contains ("so far"), "no elapsed text in the first second: " + early);

        beginTest ("ProgressText: the ready note says how long it took and where");
        expectEquals (describePrepared (130.0, "cuda"), juce::String ("Prepared in 2 min 10 s on the GPU."));
        expectEquals (describePrepared (9.0, "cpu"), juce::String ("Prepared in 9 s on the CPU."));
        expectEquals (describePrepared (9.0, {}), juce::String ("Prepared in 9 s."));

        beginTest (
            "ProgressText: a song in the library opens without a ready worker; Play appears after a fresh preparation");
        ImportPanel panel;
        panel.setSize (900, 500);
        panel.setLibrary ({ { "k1", "Song", "3:00" } });
        panel.setImportAllowed (false);
        panel.setOpenAllowed (true);
        auto* import = findButton (panel, "Import song...");
        auto* open = findButton (panel, "Open");
        auto* play = findButton (panel, "Play");
        expect (import != nullptr && open != nullptr && play != nullptr);

        if (import != nullptr && open != nullptr && play != nullptr)
        {
            expect (! import->isEnabled(), "no worker, no import");
            expect (! play->isVisible());

            SessionStatus ready;
            ready.headline = "Ready: Song";
            ready.justPrepared = true;
            panel.setStatus (ready);
            expect (play->isVisible(), "a fresh preparation offers Play");

            ready.justPrepared = false;
            panel.setStatus (ready);
            expect (! play->isVisible(), "a library open does not");

            panel.setOpenAllowed (false);
            expect (! open->isEnabled());
        }
    }
};

ProgressTextTests progressTextTests;
} // namespace
} // namespace snds
