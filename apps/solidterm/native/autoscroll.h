#pragma once

#include "scrollanchor.h" // clampOffset

#include <algorithm>

// Selection-autoscroll policy (xterm/VTE/Tilix behaviour): while the user drags a LOCAL selection
// (mousereport.h's MouseAction::Local only — never while the running program has mouse tracking on)
// with the pointer past the top/bottom edge of the view, the view keeps scrolling toward that edge
// for as long as the pointer stays there — driven by a fixed-interval QTimer in TerminalView, not by
// further mouse movement (xterm/VTE/Tilix: no re-move required to keep going). These are the pure,
// Qt-free pieces of that policy; TerminalView::autoscrollTick wires them to the real geometry
// (cellAt/update) the same way TerminalView::mousePressEvent etc. wire mousereport.h's decision table.

inline constexpr int kAutoscrollTimerMs = 40; // ~25 Hz — smooth without flooding update()

// Signed distance (item-local pixels) the pointer sits PAST the nearest edge of a [0, viewHeight)
// view: negative above the top, positive below the bottom, 0 anywhere inside (→ no autoscroll). Qt
// Quick keeps delivering mouseMoveEvent positions outside the item's own bounds for as long as the
// item holds the implicit mouse grab from the press, so `y` here is never pre-clamped by Qt itself.
// (double, not Qt's `qreal` alias, so this header stays includable from the Qt-free test — qreal IS
// double on every desktop build Qt supports, so TerminalView's call sites convert silently.)
inline double edgeOvershoot(double y, double viewHeight)
{
    if (y < 0)
        return y;
    if (y > viewHeight)
        return y - viewHeight;
    return 0;
}

// How many lines to scroll on ONE timer tick, given how far past the edge the pointer sits (always a
// non-negative magnitude — the caller reads direction from the SIGN of edgeOvershoot, not from here).
// Grows with distance — xterm/VTE/Tilix: the farther out, the faster — and is capped so a pointer
// flung to the far corner of a huge monitor still scrolls at a sane top speed, never a page-jump.
inline int autoscrollLinesPerTick(double overshootMagnitude, double cellHeight)
{
    if (overshootMagnitude <= 0 || cellHeight <= 0)
        return 0;
    const int lines = 1 + int(overshootMagnitude / cellHeight);
    return std::min(lines, 8);
}

// The scrollOffset after one tick scrolls `lines` toward the scrollback (above=true: offset grows,
// revealing older content) or toward live (above=false: offset shrinks) — clamped with the SAME
// clampOffset TerminalView already uses for the wheel and for scrollback shrink/grow, so autoscroll
// can never run the view past the oldest line or past live.
inline int autoscrollOffsetAfterTick(int offset, int sbSize, int lines, bool above)
{
    return clampOffset(offset + (above ? lines : -lines), sbSize);
}
