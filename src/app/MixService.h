#pragma once

#include "audio/StemMixStage.h"
#include "dsp/MixMeters.h"
#include "dsp/MixProcessor.h"
#include "dsp/ParameterStore.h"

#include <functional>
#include <memory>

namespace snds
{
/** The app's interactive mix: the one ParameterStore the UI writes, and the MixProcessor the
    engine runs for the loaded song.

    The engine asks stageFactory() for a stage at every load (a new MixProcessor, prepared for
    the song's sample rate). The service keeps a reference to the newest processor so the UI can
    read its meters; the processor therefore outlives the session that runs it.

    Message thread only (the factory is called by PlaybackEngine::load on that thread). The audio
    thread touches only the processor, through the parameter snapshot and lock-free meter store.
    Mix state is ephemeral: beginSong() resets every control, and nothing here is persisted.
*/
class MixService
{
public:
    MixService() = default;

    dsp::ParameterStore& parameters() noexcept { return store; }
    const dsp::ParameterStore& parameters() const noexcept { return store; }

    /** For PlaybackEngine::setMixStageFactory. */
    std::function<std::unique_ptr<audio::StemMixStage>()> stageFactory();

    /** The processor of the most recent load, or null before the first. */
    std::shared_ptr<dsp::MixProcessor> processor() const noexcept { return current; }

    /** Latest meters. False when no song was loaded yet or a write kept colliding (keep the
        previous values). */
    bool readMeters (dsp::MeterSnapshot& out) const noexcept;

    /** Every control back to its default and the tempo to `bpm`, as ONE transaction (the audio
        thread sees the old mix or the new one, never a mixture, and the delay sees no 120 BPM blip). */
    void resetMix (double bpm);

    /** A song is being opened: every control back to its default, tempo set to the song's. */
    void beginSong (double bpm);

    /** The song is gone: meters read as silence until the next load. */
    void endSong() noexcept { current.reset(); }

private:
    dsp::ParameterStore store;
    std::shared_ptr<dsp::MixProcessor> current;
};
} // namespace snds
