#pragma once

// Whether a key of the computer keyboard is down right now, read from the
// system's keyboard state rather than from key events (a plug-in window inside
// a host often gets no key events: the host keeps them). macOS virtual key code;
// on other systems this returns false and the caller falls back to JUCE.
bool isMacKeyDown(int macKeyCode);
