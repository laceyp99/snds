#include "ui/ErrorBanner.h"

namespace snds
{
namespace
{
constexpr int pad = 12;
constexpr int buttonHeight = 30;
constexpr int detailsHeight = 140;
} // namespace

ErrorBanner::ErrorBanner()
{
    message.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    message.setMinimumHorizontalScale (1.0f);
    guidance.setFont (juce::FontOptions (14.0f));
    guidance.setMinimumHorizontalScale (1.0f);

    details.setMultiLine (true, true);
    details.setReadOnly (true);
    details.setScrollbarsShown (true);
    details.setCaretVisible (false);
    details.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));

    detailsButton.setTitle ("Show details");
    detailsButton.onClick = [this] { setDetailsShown (! detailsShown); };

    addAndMakeVisible (message);
    addAndMakeVisible (guidance);
    addChildComponent (detailsButton);
    addChildComponent (details);
    setVisible (false);
}

juce::Colour ErrorBanner::colourFor (Severity severity)
{
    return severity == Severity::error   ? juce::Colour (0xfff85149)
         : severity == Severity::warning ? juce::Colour (0xffd29922)
                                         : juce::Colour (0xff58a6ff);
}

void ErrorBanner::setReport (const std::optional<ErrorReport>& report)
{
    const auto before = getPreferredHeight();
    current = report;
    detailsShown = false;
    buttons.clear();
    details.setVisible (false);
    setVisible (current.has_value());

    if (current)
    {
        message.setText (current->message, juce::dontSendNotification);
        guidance.setText (current->guidance, juce::dontSendNotification);
        message.setTitle (current->message);
        details.setText (current->details, false);
        detailsButton.setVisible (current->hasDetails());
        detailsButton.setButtonText ("Show details");

        for (const auto& action : current->actions)
        {
            auto button = std::make_unique<KeyButton> (action.label);
            button->setTitle (action.label);
            button->onClick = [this, kind = action.kind]
            {
                if (onAction)
                    onAction (kind);
            };
            addAndMakeVisible (*button);
            buttons.emplace_back (action.kind, std::move (button));
        }
    }
    else
    {
        message.setText ({}, juce::dontSendNotification);
        guidance.setText ({}, juce::dontSendNotification);
        detailsButton.setVisible (false);
    }

    resized();
    repaint();

    if (before != getPreferredHeight() && onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void ErrorBanner::setDetailsShown (bool shown)
{
    if (! current || ! current->hasDetails() || detailsShown == shown)
        return;

    detailsShown = shown;
    details.setVisible (shown);
    detailsButton.setButtonText (shown ? "Hide details" : "Show details");
    resized();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

juce::Button* ErrorBanner::findActionButton (RecoveryKind kind)
{
    for (auto& [actionKind, button] : buttons)
        if (actionKind == kind)
            return button.get();

    return nullptr;
}

int ErrorBanner::getPreferredHeight() const
{
    if (! current)
        return 0;

    const auto guidanceHeight = current->guidance.isEmpty() ? 0 : 24;
    const auto actionRow = (buttons.empty() && ! current->hasDetails()) ? 0 : buttonHeight + 8;
    return pad + 24 + guidanceHeight + actionRow + (detailsShown ? detailsHeight + 8 : 0) + pad;
}

void ErrorBanner::paint (juce::Graphics& g)
{
    if (! current)
        return;

    g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.12f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
    g.setColour (colourFor (current->severity));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, 5.0f, static_cast<float> (getHeight())), 2.5f);
}

void ErrorBanner::resized()
{
    if (! current)
        return;

    auto area = getLocalBounds().reduced (pad).withTrimmedLeft (6);
    message.setBounds (area.removeFromTop (24));

    if (current->guidance.isNotEmpty())
        guidance.setBounds (area.removeFromTop (24));
    else
        guidance.setBounds ({});

    if (buttons.empty() && ! current->hasDetails())
        return;

    area.removeFromTop (8);
    auto row = area.removeFromTop (buttonHeight);
    int x = row.getX();

    for (auto& [kind, button] : buttons)
    {
        const auto width = juce::jmax (
            90, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (14.0f), button->getButtonText()) + 28);
        button->setBounds (x, row.getY(), width, buttonHeight);
        x += width + 8;
    }

    detailsButton.setBounds (row.getRight() - 130, row.getY(), 130, buttonHeight);

    if (detailsShown)
    {
        area.removeFromTop (8);
        details.setBounds (area.removeFromTop (detailsHeight));
    }
}

//==============================================================================
namespace
{
class DetailsWindow final : public juce::DialogWindow
{
public:
    explicit DetailsWindow (const ErrorReport& report)
        : DialogWindow (
              report.message,
              juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
              true)
    {
        auto* text = new juce::TextEditor();
        text->setMultiLine (true, true);
        text->setReadOnly (true);
        text->setScrollbarsShown (true);
        text->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        text->setText (report.toText(), false);
        text->setSize (640, 360);
        setContentOwned (text, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
    }

    void closeButtonPressed() override { delete this; }
};
} // namespace

void showErrorDetails (const ErrorReport& report, juce::Component* relativeTo)
{
    auto* window = new DetailsWindow (report);
    window->centreAroundComponent (relativeTo, window->getWidth(), window->getHeight());
    window->setVisible (true);
    window->toFront (true);
}
} // namespace snds
