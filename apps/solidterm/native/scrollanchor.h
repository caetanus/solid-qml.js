#pragma once

#include <algorithm>

// Scrolled-back view policy (VTE/Tilix behaviour): program output never moves a scrolled-back view —
// only the user does (typing, paste, the wheel). `offset` = lines scrolled back from the live screen
// (0 = live); the view's top line is scrollback[size - offset].

// A line entering the scrollback while scrolled back pushes everything one line further from live:
// +1 keeps the SAME content in view. At the limit (the oldest line just got dropped) it stays capped.
inline int offsetAfterPush(int offset, int sbSize)
{
    return offset > 0 ? std::min(offset + 1, sbSize) : 0;
}

// Lines leaving the scrollback (popped back when the terminal grows): never point past the oldest.
inline int clampOffset(int offset, int sbSize)
{
    return std::clamp(offset, 0, sbSize);
}
