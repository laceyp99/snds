#pragma once

#include "app/ReadinessProbe.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace snds::ui
{

/**
    Shows environment readiness with actionable guidance.

    The probe runs on a private background thread (it may block on the file system, and on a worker
    process once worker readiness is added); results are delivered back on the message thread.
*/
class ReadinessScreen final : public juce::Component
{
public:
    ReadinessScreen (std::shared_ptr<ReadinessProbe> probe, juce::File logsDirectory);
    ~ReadinessScreen() override;

    /** Starts a new probe run unless one is already in flight. */
    void refresh();

    /** Called on the message thread with each completed report. */
    std::function<void (const ReadinessReport&)> onReportReady;

    /** Called when the user chooses to continue to song import. */
    std::function<void()> onContinue;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class CheckList;

    void showReport (const ReadinessReport& report);

    std::shared_ptr<ReadinessProbe> probe;
    juce::File logsDirectory;
    bool probeRunning = false;

    juce::Label titleLabel;
    juce::Label summaryLabel;
    std::unique_ptr<CheckList> checkList;
    juce::Viewport viewport;
    juce::TextButton recheckButton { "Check again" };
    juce::TextButton logsButton { "Open logs folder" };
    juce::TextButton continueButton { "Continue" };

    // Declared last so it is destroyed first: waits for an in-flight probe before members go away.
    juce::ThreadPool probeThread {
        juce::ThreadPoolOptions {}.withThreadName ("SNDS readiness").withNumberOfThreads (1)
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReadinessScreen)
};

} // namespace snds::ui
