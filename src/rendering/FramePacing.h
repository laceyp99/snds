#pragma once

#include <algorithm>
#include <cmath>

namespace snds::rendering::pacing
{
/** Frame pacing for the artwork. The target is 60 frames per second and no more: the picture
    follows music, not a benchmark, and a render loop that runs flat out would burn a core and
    compete with the UI and the audio thread for it.

    GL tier: the swap interval is chosen from the display's refresh rate so vsync delivers about 60
    fps (a 60 Hz panel swaps every frame, 120 Hz every second one, 144 Hz every second one for 72),
    and a software floor on the frame time keeps the render thread from spinning when a driver
    ignores the swap interval.
    CPU tier: painting happens on the message thread, so it is paced by a timer at 60 Hz and, if
    paints are slow, at 30 Hz before it gives up. Static tier: no animation, no per-frame work. */
inline constexpr double targetFps = 60.0;

// Timer rates (Hz) of the host's health tick per tier. Only the CPU tier needs a frame timer.
inline constexpr double glHealthHz = 10.0;
inline constexpr double staticHealthHz = 2.0;
inline constexpr double cpuFullHz = 60.0;
inline constexpr double cpuReducedHz = 30.0;

// The CPU tier's budget: the average cost of a paint over the last `cpuBudgetWindow` paints.
inline constexpr double cpuBudgetMs = 8.0;         // at 60 Hz (half a 16.7 ms frame, leaving the UI its share)
inline constexpr double cpuReducedBudgetMs = 12.0; // at 30 Hz
inline constexpr int cpuBudgetWindow = 30;

/** How many display refreshes to wait between swaps to get close to `target` fps.
    60 -> 1, 75 -> 1, 90 -> 1, 120 -> 2, 144 -> 2 (72 fps), 240 -> 4. Unknown refresh -> 1. */
inline int swapIntervalFor (double refreshHz, double target = targetFps) noexcept
{
    if (! std::isfinite (refreshHz) || refreshHz <= 0.0 || target <= 0.0)
        return 1;

    return std::max (1, static_cast<int> (std::floor (refreshHz / target + 0.2)));
}

/** The shortest time a frame may take, in milliseconds: a little under the target interval, so a
    correctly vsynced loop never sleeps and a free-running one is held near the target. */
inline double minFrameIntervalMs (double target = targetFps) noexcept
{
    return 1000.0 / (std::max (1.0, target) * 1.25);
}
} // namespace snds::rendering::pacing
