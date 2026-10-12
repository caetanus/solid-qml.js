// Pure geometry/paging contract for TerminalView's composed scrollbar (scrollbar.h) — same style as
// autoscroll_test.cpp: no Qt, no vterm, just the formulas the QML bar's JS mirrors.
#include "../native/scrollbar.h"

#include <cstdio>

namespace {

bool expectEq(const char *what, int got, int want)
{
    if (got == want)
        return true;
    std::fprintf(stderr, "%s: got %d, want %d\n", what, got, want);
    return false;
}

bool near(const char *what, double got, double want, double eps = 1e-9)
{
    if (std::abs(got - want) <= eps)
        return true;
    std::fprintf(stderr, "%s: got %f, want %f\n", what, got, want);
    return false;
}

// ── 1. Thumb height: "viewport / (scrollback + rows)" — owner's literal formula.
bool thumbHeightMatchesFormula()
{
    bool ok = true;
    ok &= near("rows=24 sbSize=0 -> whole track", scrollbarThumbHeightFrac(24, 0), 1.0);
    ok &= near("rows=24 sbSize=24 -> half", scrollbarThumbHeightFrac(24, 24), 0.5);
    ok &= near("rows=24 sbSize=216 -> 1/10", scrollbarThumbHeightFrac(24, 216), 0.1);
    ok &= near("rows=0 degenerate -> 1.0 (no divide by zero)", scrollbarThumbHeightFrac(0, 0), 1.0);
    return ok;
}

// ── 2. Thumb position tracks m_scrollOffset: live (offset 0) pins the thumb at the BOTTOM (frac 1),
//    fully scrolled back (offset == sbSize) pins it at the TOP (frac 0), and it's linear between.
bool thumbPositionTracksOffset()
{
    bool ok = true;
    ok &= near("no scrollback -> bottom", scrollbarThumbTopFrac(0, 0), 1.0);
    ok &= near("live (offset 0) -> bottom", scrollbarThumbTopFrac(0, 100), 1.0);
    ok &= near("fully back (offset == sbSize) -> top", scrollbarThumbTopFrac(100, 100), 0.0);
    ok &= near("halfway back -> middle", scrollbarThumbTopFrac(50, 100), 0.5);
    ok &= near("clamps above sbSize", scrollbarThumbTopFrac(999, 100), 0.0);
    return ok;
}

// ── 3. Dragging the thumb to a fraction round-trips back to the offset that produced it.
bool dragRoundTrips()
{
    bool ok = true;
    for (int offset : { 0, 1, 50, 99, 100 }) {
        const double frac = scrollbarThumbTopFrac(offset, 100);
        const int back = scrollbarOffsetFromThumbFrac(frac, 100);
        ok &= expectEq("round-trip", back, offset);
    }
    ok &= expectEq("no scrollback -> offset 0 regardless of frac", scrollbarOffsetFromThumbFrac(0.3, 0), 0);
    ok &= expectEq("clamps frac below 0", scrollbarOffsetFromThumbFrac(-5, 100), 100);
    ok &= expectEq("clamps frac above 1", scrollbarOffsetFromThumbFrac(5, 100), 0);
    return ok;
}

// ── 4. Track-click paging: ~90% of the viewport, never 0 even for a tiny view.
bool pagingIsNinetyPercentOfViewport()
{
    bool ok = true;
    ok &= expectEq("24 rows -> ~22", scrollbarPageLines(24), 22);
    ok &= expectEq("1 row -> at least 1", scrollbarPageLines(1), 1);
    ok &= expectEq("10 rows -> 9", scrollbarPageLines(10), 9);
    return ok;
}

} // namespace

int main()
{
    const bool a = thumbHeightMatchesFormula();
    const bool b = thumbPositionTracksOffset();
    const bool c = dragRoundTrips();
    const bool d = pagingIsNinetyPercentOfViewport();
    const bool ok = a && b && c && d;
    std::puts(ok ? "scrollbar ok" : "scrollbar FAILED");
    return ok ? 0 : 1;
}
