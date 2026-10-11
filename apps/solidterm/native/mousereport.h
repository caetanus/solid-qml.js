#pragma once

#include <vterm.h>

// Pure (Qt-free) mouse-reporting policy shared between TerminalView and its test — the exact rules
// xterm/VTE follow:
//   - Shift held always wins: today's local behaviour (selection, scrollback wheel, context menu),
//     regardless of what the program asked for.
//   - Otherwise, a program that enabled mouse tracking (DECSET 1000/1002/1003 — libvterm surfaces
//     this as VTERM_PROP_MOUSE CLICK/DRAG/MOVE) gets presses/releases/motion/wheel forwarded
//     through libvterm's own vterm_mouse_move()/vterm_mouse_button(), which encode X10/UTF8/SGR/
//     urxvt per whatever the program actually requested (DECSET 1005/1006/1015) — TerminalView
//     never builds escape bytes itself.
//   - Otherwise, on the alternate screen (a pager: less, man, git log) the wheel becomes cursor
//     Up/Down keys instead of scrolling the (empty) scrollback.
enum class MouseAction {
    Local,     // today's behaviour: selection, scrollback wheel, context menu
    Report,    // forward to the program via libvterm's mouse API
    AltScroll, // wheel → cursor-up/down keys (alternate screen, no mouse mode)
};

inline MouseAction decideMouseAction(int vtermMouseMode, bool altScreen, bool shiftHeld)
{
    if (shiftHeld)
        return MouseAction::Local;
    if (vtermMouseMode != VTERM_PROP_MOUSE_NONE)
        return MouseAction::Report;
    if (altScreen)
        return MouseAction::AltScroll;
    return MouseAction::Local;
}

// Clamps a pixel coordinate to a 0-based cell index. A mouse report always refers to the LIVE
// screen (xterm never reports scrollback positions), so the caller must snap scrolled-back state
// to live BEFORE using this for a reported event — once live, "pixel / cellSize" already lands on
// the right row because the viewport row IS the live row at offset 0.
inline int pixelToCellIndex(double pixel, double cellSize, int cellCount)
{
    if (cellSize <= 0 || cellCount <= 0)
        return 0;
    int idx = int(pixel / cellSize);
    if (idx < 0)
        idx = 0;
    if (idx >= cellCount)
        idx = cellCount - 1;
    return idx;
}

// High-resolution wheel deltas (touchpads send many small updates) accumulate in `pending` until a
// full physical-wheel "notch" (Qt's convention: angleDelta of 120 units) is reached; returns how
// many notches to emit (positive = up/away, negative = down/towards) and leaves the remainder for
// next time, so slow/fine scrolling never fires a notch early or drops one.
inline int accumulateWheelNotches(int &pending, int angleDeltaY)
{
    pending += angleDeltaY;
    const int notches = pending / 120;
    pending -= notches * 120;
    return notches;
}
