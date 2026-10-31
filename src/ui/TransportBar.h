#pragma once

#include "app/ErrorReport.h"
#include "app/PlaybackController.h"
#include "ui/MixControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace snds
{
/** Play/pause, stop, seek slider, time, loop, Original / Stem Mix, and a line saying what the
    player is doing. It displays PlaybackController::view() (polled at 20 Hz, because the
    position moves continuously) and forwards user actions; it holds no playback logic.
*/
class TransportBar final : public juce::Component, private juce::Timer
{
public:
    TransportBar();
    ~TransportBar() override;

    /** The controller to show and drive; null shows an idle, disabled bar. */
    void setController (PlaybackController* controller);

    /** A recovery action of the current problem (device lost, song cannot be played) was pressed. */
    std::function<void (RecoveryKind)> onRecovery;

    /** The problem being shown, if any (also used by tests). */
    const std::optional<ErrorReport>& getReport() const noexcept { return report; }
    juce::Button& fixButton() noexcept { return fix; }

    /** Re-reads the controller now (it is also polled). */
    void refresh();

    void resized() override;

    static constexpr int preferredHeight = 92;

    /** The one-line state text for a view, also used by tests. */
    static juce::String describeState (const PlaybackView& view);

    /** Why Original/Stem Mix is unavailable, or empty when it is available or there is no song. */
    static juce::String describeOriginal (const OriginalAvailability& availability);

private:
    void timerCallback() override;
    void commitSeek();

    PlaybackController* controller = nullptr;
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, stemMixButton { "Stem Mix" },
        originalButton { "Original" };
    juce::ToggleButton loopToggle { "Loop" };
    juce::Slider position;
    juce::Label timeLabel, originalNote, stateLabel;
    bool updatingFromView = false;
    bool dragging = false;
    std::optional<ErrorReport> report;
    KeyButton fix, moreInfo { "Details..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};
} // namespace snds
