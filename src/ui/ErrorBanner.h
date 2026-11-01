#pragma once

#include "app/ErrorReport.h"
#include "ui/MixControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace snds
{
/** Shows one ErrorReport the same way everywhere: a severity dot, the message, what to do next,
    one button per recovery action, and "Show details" for the technical text. It only displays
    the report and reports which action was pressed; the owner carries it out.
*/
class ErrorBanner final : public juce::Component
{
public:
    ErrorBanner();

    /** Shows `report`, or hides the banner for nullopt. Details start collapsed. */
    void setReport (const std::optional<ErrorReport>& report);
    const std::optional<ErrorReport>& getReport() const noexcept { return current; }

    /** Height for the current content (0 when there is no report). */
    int getPreferredHeight() const;

    std::function<void (RecoveryKind)> onAction;
    std::function<void()> onPreferredHeightChanged;

    // For tests.
    bool areDetailsShown() const noexcept { return detailsShown; }
    void setDetailsShown (bool shown);
    juce::Button* findActionButton (RecoveryKind kind);
    juce::Button& detailsToggle() noexcept { return detailsButton; }
    const juce::Label& messageLabel() const noexcept { return message; }
    const juce::Label& guidanceLabel() const noexcept { return guidance; }

    void paint (juce::Graphics&) override;
    void resized() override;

    static juce::Colour colourFor (Severity severity);

private:
    std::optional<ErrorReport> current;
    juce::Label message, guidance;
    std::vector<std::pair<RecoveryKind, std::unique_ptr<KeyButton>>> buttons;
    KeyButton detailsButton { "Show details" };
    juce::TextEditor details;
    bool detailsShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ErrorBanner)
};

/** Opens a window with the report's full text (message, next step, details) that can be selected
    and copied. For places too small to expand in place. */
void showErrorDetails (const ErrorReport& report, juce::Component* relativeTo = nullptr);
} // namespace snds
