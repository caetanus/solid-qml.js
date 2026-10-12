// Selection-autoscroll policy of SolidTerm's TerminalView (xterm/VTE/Tilix behaviour), without Qt —
// same style as scrollback_test.cpp/mousereport_test.cpp: drives the pure helpers TerminalView's
// mouseMoveEvent/autoscrollTick use (autoscroll.h) directly, plus a tiny local re-implementation of
// TerminalView::cellAt's absRow formula (sbSize - offset + row) to prove the SELECTION keeps tracking
// the right absolute row as the view scrolls under a stationary pointer.
#include "../native/autoscroll.h"

#include <cstdio>

namespace {

bool expectEq(const char *what, int got, int want)
{
    if (got == want)
        return true;
    std::fprintf(stderr, "%s: got %d, want %d\n", what, got, want);
    return false;
}

// ── 1. edgeOvershoot: inside the view → 0 either way; above/below → signed distance past the edge.
bool edgeOvershootShape()
{
    bool ok = true;
    ok &= expectEq("inside top", int(edgeOvershoot(0, 400)), 0);
    ok &= expectEq("inside middle", int(edgeOvershoot(200, 400)), 0);
    ok &= expectEq("inside bottom edge (exclusive-safe)", int(edgeOvershoot(400, 400)), 0);
    ok &= expectEq("10px above", int(edgeOvershoot(-10, 400)), -10);
    ok &= expectEq("25px below", int(edgeOvershoot(425, 400)), 25);
    return ok;
}

// ── 2. autoscrollLinesPerTick: grows with distance, 1 line just past the edge, capped at 8 no
//    matter how far off the pointer gets flung (never a page-jump).
bool linesPerTickGrowsAndCaps()
{
    const double cellH = 16;
    bool ok = true;
    ok &= expectEq("no overshoot", autoscrollLinesPerTick(0, cellH), 0);
    ok &= expectEq("1px past edge", autoscrollLinesPerTick(1, cellH), 1);
    ok &= expectEq("just under one cell", autoscrollLinesPerTick(15, cellH), 1);
    ok &= expectEq("exactly one cell", autoscrollLinesPerTick(16, cellH), 2);
    ok &= expectEq("three cells", autoscrollLinesPerTick(3 * cellH, cellH), 4);
    ok &= expectEq("far off screen caps at 8", autoscrollLinesPerTick(1000 * cellH, cellH), 8);
    // Degenerate geometry never faults or scrolls.
    ok &= expectEq("zero cell height", autoscrollLinesPerTick(50, 0), 0);
    return ok;
}

// ── 3. autoscrollOffsetAfterTick: scrolling "above" grows the offset (reveals older scrollback),
//    "below" shrinks it (toward live) — both clamped the same way the wheel already is.
bool offsetAfterTickClamps()
{
    bool ok = true;
    ok &= expectEq("above grows", autoscrollOffsetAfterTick(10, 500, 3, true), 13);
    ok &= expectEq("above clamps at sbSize", autoscrollOffsetAfterTick(498, 500, 5, true), 500);
    ok &= expectEq("below shrinks", autoscrollOffsetAfterTick(10, 500, 3, false), 7);
    ok &= expectEq("below clamps at 0 (live)", autoscrollOffsetAfterTick(2, 500, 5, false), 0);
    return ok;
}

// ── 4. Selection extent while scrolling: a stationary pointer above the view, ticking repeatedly,
//    must keep walking the selection's moving end BACK through the scrollback (lower absRow each
//    tick) without any mouse movement, and stop growing once the offset is pinned at the oldest
//    line — exactly the TerminalView::autoscrollTick contract (cellAt's absRow = sbSize - offset +
//    row, row = 0 while scrolling up).
int absRowAtTop(int sbSize, int offset) // mirrors TerminalView::cellAt(QPointF(x, 0))
{
    return sbSize - offset + 0;
}

bool selectionWalksBackAndStops()
{
    const int sbSize = 20;
    const double cellH = 16;
    int offset = 0;
    const int anchorAbsRow = sbSize; // pressed on the live screen's row 0 before scrolling started
    int selEndAbsRow = anchorAbsRow;
    bool ok = true;

    // Pointer sits 8px above the edge (→ 1 line/tick): five ticks should walk the end back five
    // rows, further from the anchor each time.
    for (int tick = 0; tick < 5; ++tick) {
        const int lines = autoscrollLinesPerTick(8, cellH);
        offset = autoscrollOffsetAfterTick(offset, sbSize, lines, /*above=*/true);
        const int newEnd = absRowAtTop(sbSize, offset);
        ok &= expectEq("tick walks strictly further back", newEnd < selEndAbsRow ? 1 : 0, 1);
        selEndAbsRow = newEnd;
    }
    if (offset != 5) {
        std::fprintf(stderr, "selectionWalksBackAndStops: offset after 5 ticks = %d, want 5\n", offset);
        ok = false;
    }

    // Keep ticking well past the oldest line: offset pins at sbSize, and the selection end pins with
    // it instead of walking past the start of the scrollback (absRow 0).
    for (int tick = 0; tick < 50; ++tick) {
        const int lines = autoscrollLinesPerTick(8, cellH);
        offset = autoscrollOffsetAfterTick(offset, sbSize, lines, /*above=*/true);
    }
    ok &= expectEq("offset pins at sbSize", offset, sbSize);
    ok &= expectEq("selection end pins at the oldest row", absRowAtTop(sbSize, offset), 0);
    return ok;
}

} // namespace

int main()
{
    const bool a = edgeOvershootShape();
    const bool b = linesPerTickGrowsAndCaps();
    const bool c = offsetAfterTickClamps();
    const bool d = selectionWalksBackAndStops();
    const bool ok = a && b && c && d;
    std::puts(ok ? "autoscroll ok" : "autoscroll FAILED");
    return ok ? 0 : 1;
}
