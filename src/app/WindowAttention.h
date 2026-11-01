#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace snds
{
/** Flash the taskbar button of the window holding `component` until it is brought forward, if it
    is not already the active window. Used when a long preparation finishes while the user is
    elsewhere. Does nothing when the window is active or on platforms without a taskbar flash. */
void requestWindowAttention (juce::Component& component);
} // namespace snds
