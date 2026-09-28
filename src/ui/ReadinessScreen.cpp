#include "ui/ReadinessScreen.h"

namespace snds::ui
{

namespace
{
juce::Colour colourFor (ReadinessStatus status)
{
    switch (status)
    {
        case ReadinessStatus::ok:
            return juce::Colour (0xff4caf50);
        case ReadinessStatus::warning:
            return juce::Colour (0xffffb300);
        case ReadinessStatus::missing:
            return juce::Colour (0xffef6c00);
    }

    return juce::Colours::grey;
}

const char* labelFor (ReadinessStatus status)
{
    switch (status)
    {
        case ReadinessStatus::ok:
            return "READY";
        case ReadinessStatus::warning:
            return "CHECK";
        case ReadinessStatus::missing:
            return "SETUP";
    }

    return "?";
}
} // namespace

//==============================================================================
class ReadinessScreen::CheckList final : public juce::Component
{
public:
    static constexpr int rowHeight = 124;

    void setReport (const ReadinessReport& newReport)
    {
        report = newReport;
        repaint();
    }

    int getPreferredHeight() const noexcept { return static_cast<int> (report.checks.size()) * rowHeight; }

    void paint (juce::Graphics& g) override
    {
        const auto text = findColour (juce::Label::textColourId);
        const auto panel = findColour (juce::ResizableWindow::backgroundColourId).brighter (0.08f);
        auto area = getLocalBounds();

        for (const auto& check : report.checks)
        {
            const auto row = area.removeFromTop (rowHeight).reduced (0, 5);
            g.setColour (panel);
            g.fillRoundedRectangle (row.toFloat(), 6.0f);

            auto inner = row.reduced (16, 12);
            auto badge = inner.removeFromLeft (84).removeFromTop (24);
            g.setColour (colourFor (check.status));
            g.fillRoundedRectangle (badge.toFloat(), 4.0f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (labelFor (check.status), badge, juce::Justification::centred);

            inner.removeFromLeft (16);
            g.setColour (text);
            g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
            g.drawText (check.title, inner.removeFromTop (24), juce::Justification::centredLeft, true);

            g.setColour (text.withAlpha (0.75f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawFittedText (check.detail, inner.removeFromTop (36), juce::Justification::topLeft, 2, 1.0f);

            if (check.action.isNotEmpty())
            {
                g.setColour (colourFor (check.status).brighter (0.3f));
                g.drawFittedText (check.action, inner, juce::Justification::topLeft, 2, 1.0f);
            }
        }
    }

private:
    ReadinessReport report;
};

//==============================================================================
ReadinessScreen::ReadinessScreen (std::shared_ptr<ReadinessProbe> probeIn, juce::File logsDirectoryIn)
    : probe (std::move (probeIn)),
      logsDirectory (std::move (logsDirectoryIn)),
      checkList (std::make_unique<CheckList>())
{
    titleLabel.setText ("Environment readiness", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    summaryLabel.setText ("Checking your setup...", juce::dontSendNotification);
    summaryLabel.setFont (juce::FontOptions (16.0f));
    addAndMakeVisible (summaryLabel);

    viewport.setViewedComponent (checkList.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    recheckButton.onClick = [this] { refresh(); };
    addAndMakeVisible (recheckButton);

    logsButton.onClick = [this]
    {
        if (logsDirectory.createDirectory().wasOk())
            logsDirectory.startAsProcess();
    };
    addAndMakeVisible (logsButton);

    continueButton.setButtonText ("Continue to import");
    continueButton.onClick = [this]
    {
        if (onContinue)
            onContinue();
    };
    addAndMakeVisible (continueButton);
}

ReadinessScreen::~ReadinessScreen()
{
    probeThread.removeAllJobs (true, 10000);
}

void ReadinessScreen::refresh()
{
    if (probeRunning || probe == nullptr)
        return;

    probeRunning = true;
    recheckButton.setEnabled (false);
    summaryLabel.setText ("Checking your setup...", juce::dontSendNotification);

    probeThread.addJob (
        [probeToRun = probe, safeThis = SafePointer<ReadinessScreen> (this)]
        {
            auto report = probeToRun->run();

            juce::MessageManager::callAsync (
                [safeThis, report]
                {
                    if (auto* screen = safeThis.getComponent())
                        screen->showReport (report);
                });
        });
}

void ReadinessScreen::showReport (const ReadinessReport& report)
{
    probeRunning = false;
    recheckButton.setEnabled (true);

    summaryLabel.setText (report.isSeparationAvailable()
                              ? "Local setup looks complete."
                              : "Setup is needed before songs can be separated. Follow the steps below, then choose "
                                "Check again.",
                          juce::dontSendNotification);

    checkList->setReport (report);
    resized();

    if (onReportReady)
        onReportReady (report);
}

void ReadinessScreen::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void ReadinessScreen::resized()
{
    auto area = getLocalBounds().reduced (32);

    titleLabel.setBounds (area.removeFromTop (44));
    summaryLabel.setBounds (area.removeFromTop (28));
    area.removeFromTop (12);

    auto buttonRow = area.removeFromBottom (36);
    area.removeFromBottom (16);
    viewport.setBounds (area);
    checkList->setSize (juce::jmax (0, area.getWidth() - viewport.getScrollBarThickness()),
                        checkList->getPreferredHeight());

    recheckButton.setBounds (buttonRow.removeFromLeft (160));
    buttonRow.removeFromLeft (12);
    logsButton.setBounds (buttonRow.removeFromLeft (180));
    continueButton.setBounds (buttonRow.removeFromRight (200));
}

} // namespace snds::ui
