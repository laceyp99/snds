#include "rendering/VisualController.h"

namespace snds::rendering
{
void VisualController::setOriginalSelected (bool original)
{
    if (settings.staticMode == original)
        return;

    settings.staticMode = original;
    mapper.reset();                   // no easing tail when the Original goes on, and a clean start when it goes off
    host.setEffects (EffectParams()); // the host must not bring back stale effects when static mode ends
    host.setStaticMode (original);
}

void VisualController::update (const dsp::VisualSnapshot& snapshot, double nowMs)
{
    // The first frame assumes a 60 Hz tick; after that, the real elapsed time.
    const auto dt = lastMs < 0.0 ? 1.0 / 60.0 : (nowMs - lastMs) / 1000.0;
    lastMs = nowMs;
    latest = snapshot;
    host.setEffects (mapper.update (snapshot, settings, dt));
}

void VisualController::update (const dsp::VisualStore& store, double nowMs)
{
    dsp::VisualSnapshot snapshot = latest;

    if (! store.read (snapshot))
        snapshot = latest; // a write collided: keep the last values

    update (snapshot, nowMs);
}

void VisualController::reset()
{
    mapper.reset();
    lastMs = -1.0;
    host.setEffects (EffectParams());
}
} // namespace snds::rendering
