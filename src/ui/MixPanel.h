#pragma once

#include "app/MixService.h"
#include "app/PlaybackController.h"
#include "app/SongSession.h"
#include "ui/MasterSection.h"
#include "ui/StemCard.h"
#include "ui/TempoFxSection.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace snds
{
/** The interactive mix: four stem cards and the master strip side by side, and the tempo and
    effects strip below. It reads and writes the app's ParameterStore, polls the meters at 30 Hz
    (keeping the last values when a read collides with a write), and pulls its controls back from
    the store whenever the store changes (a reset). Mix state is never saved.

    The controls are for Stem Mix. When Original is selected a note says so, and a standing note
    says Original is unprocessed, so its level can differ from Stem Mix.
*/
class MixPanel final : public juce::Component, private juce::Timer
{
public:
    explicit MixPanel (MixService& mix);
    ~MixPanel() override;

    /** The player, for "is a song loaded" and "is Original playing". */
    void setController (PlaybackController* controller) { playback = controller; }

    /** The open song (its tempo information), or nullopt. */
    void setSong (const std::optional<OpenSong>& song);

    /** Called after the user's tempo choice was applied to the mix: store it for the song. */
    std::function<void (double bpm, const juce::String& mode)> onTempoChosen;

    /** The user asked to go back to the detected tempo. */
    std::function<void()> onTempoCleared;

    /** Fires when expanding a card changes the panel's preferred height. */
    std::function<void()> onPreferredHeightChanged;

    /** Asks the user to confirm, then resets the whole mix (see resetMix). */
    void requestResetMix();

    /** Every control to its default and the tempo to the open song's effective tempo (the
        user's setting, else the detected one, else 120 BPM). The song's stored tempo setting is
        kept. No confirmation: that is requestResetMix. */
    void resetMix();

    /** For tests and other front ends: replaces the confirmation dialog. It receives the question
        and runs `proceed` if the user agrees. */
    std::function<void (const juce::String& question, std::function<void()> proceed)> confirmReset;

    /** The tempo a reset goes back to for the open song. */
    double getResetTempo() const;

    /** The confirmation question for a reset to `bpm` (also used by tests). */
    static juce::String describeReset (double bpm, const TempoFxSection::TempoInfo& info);

    juce::Button& resetButton() noexcept { return resetMixButton; }

    int getPreferredHeight() const noexcept;

    /** Reads meters and store once, as the timer does (tests call it directly). */
    void refresh();

    std::array<std::unique_ptr<StemCard>, 4>& stemCards() noexcept { return cards; }
    MasterSection& master() noexcept { return masterSection; }
    TempoFxSection& tempoFx() noexcept { return tempoSection; }

    /** The note under the panel title for a player view (also used by tests). */
    static juce::String describeNote (const PlaybackView& view);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { refresh(); }
    void applyTempo (double bpm, const juce::String& mode);

    MixService& mixService;
    PlaybackController* playback = nullptr;

    std::array<std::unique_ptr<StemCard>, 4> cards;
    MasterSection masterSection;
    TempoFxSection tempoSection;
    juce::Label noteLabel;
    KeyButton resetMixButton { "Reset mix" };

    std::uint32_t lastVersion = 0;
    bool haveSong = false;
    std::optional<OpenSong> song;

    static constexpr int headerHeight = 28;
    static constexpr int gap = 8;
    static constexpr int masterWidth = 150;
    static constexpr int fxHeight = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixPanel)
};
} // namespace snds
