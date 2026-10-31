#include "app/MixService.h"

#include <array>
#include <utility>

namespace snds
{
namespace
{
/** Presents a MixProcessor as the engine's StemMixStage. */
class MixStage final : public audio::StemMixStage
{
public:
    explicit MixStage (std::shared_ptr<dsp::MixProcessor> processor)
        : mix (std::move (processor))
    {
    }

    void prepare (double sourceRate, int maxBlockFrames) override { mix->prepare (sourceRate, maxBlockFrames); }
    void reset() noexcept override { mix->reset(); }

    void process (const std::array<const float* const*, audio::stemCount>& stems,
                  float* const* output,
                  int numFrames) noexcept override
    {
        mix->process (stems, output, numFrames);
    }

private:
    std::shared_ptr<dsp::MixProcessor> mix;
};
} // namespace

std::function<std::unique_ptr<audio::StemMixStage>()> MixService::stageFactory()
{
    return [this]
    {
        current = std::make_shared<dsp::MixProcessor> (store);
        return std::make_unique<MixStage> (current);
    };
}

bool MixService::readMeters (dsp::MeterSnapshot& out) const noexcept
{
    return current != nullptr && current->meters().read (out);
}

void MixService::resetMix (double bpm)
{
    std::array<std::pair<int, float>, dsp::numParameters> changes;

    for (int i = 0; i < dsp::numParameters; ++i)
        changes[static_cast<size_t> (i)] = { i, dsp::parameterSpec (i).defaultValue };

    changes[static_cast<size_t> (dsp::parameterIndex (dsp::GlobalControl::tempoBpm))].second = static_cast<float> (bpm);
    store.setMany (changes);
}

void MixService::beginSong (double bpm)
{
    resetMix (bpm);
}
} // namespace snds
