#include "app/WindowAttention.h"

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace snds
{
void requestWindowAttention (juce::Component& component)
{
#if JUCE_WINDOWS
    auto* top = component.getTopLevelComponent();
    auto* peer = top != nullptr ? top->getPeer() : nullptr;

    if (peer == nullptr || top->hasKeyboardFocus (true))
        return;

    FLASHWINFO info {
        sizeof (FLASHWINFO), static_cast<HWND> (peer->getNativeHandle()), FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0
    };
    FlashWindowEx (&info);
#else
    juce::ignoreUnused (component);
#endif
}
} // namespace snds
