#pragma once

#include <algorithm>
#include <cmath>

// Pure (Qt-free) geometry/paging contract for TerminalView's composed scrollbar (see
// TerminalView::composeScrollBar in terminalview.cpp) — mirrors the SAME formulas the QML bar's JS
// uses for the thumb's size/position and for track-click paging, so this header both documents and
// pins that contract (tested in scrollbar_test.cpp; the QML can't #include it, hence "mirrors").
//
// Model: the whole buffer is `sbSize + rows` lines (oldest at the top, live at the bottom); the
// viewport is `rows` lines tall, currently showing lines [sbSize-offset, sbSize-offset+rows). offset
// 0 = live (viewport at the very bottom); offset sbSize = fully scrolled back (viewport at the top).

// Thumb height as a fraction of the track — "viewport / (scrollback + rows)" (owner), clamped to
// [small-positive, 1] so an empty buffer (sbSize=0, rows=0 — degenerate) never divides by zero.
inline double scrollbarThumbHeightFrac(int rows, int sbSize)
{
    const int total = rows + sbSize;
    if (total <= 0 || rows <= 0)
        return 1.0;
    return std::clamp(double(rows) / double(total), 0.0, 1.0);
}

// Where the TOP of the thumb sits, as a fraction of the track's free travel (0 = track top = oldest
// line in view; 1 = track bottom = live) — the position side of "position follows m_scrollOffset".
inline double scrollbarThumbTopFrac(int offset, int sbSize)
{
    if (sbSize <= 0)
        return 1.0; // nothing to scroll: pin at the bottom (live)
    return std::clamp(double(sbSize - offset) / double(sbSize), 0.0, 1.0);
}

// Inverse of scrollbarThumbTopFrac: the offset a drag to thumb-top-fraction `fracTop` corresponds
// to, clamped to [0, sbSize].
inline int scrollbarOffsetFromThumbFrac(double fracTop, int sbSize)
{
    if (sbSize <= 0)
        return 0;
    fracTop = std::clamp(fracTop, 0.0, 1.0);
    return std::clamp(int(std::lround((1.0 - fracTop) * sbSize)), 0, sbSize);
}

// Track-click paging distance, in lines — 90% of the viewport, like a classic scrollbar "page"
// (never 0, even for a 1-row view).
inline int scrollbarPageLines(int rows)
{
    return std::max(1, int(std::lround(rows * 0.9)));
}
