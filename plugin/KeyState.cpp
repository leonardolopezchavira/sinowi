#include "KeyState.h"

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>

bool isMacKeyDown(int macKeyCode) {
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, static_cast<CGKeyCode>(macKeyCode));
}
#else
bool isMacKeyDown(int) { return false; }
#endif
