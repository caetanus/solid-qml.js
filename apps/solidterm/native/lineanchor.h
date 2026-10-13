#pragma once

#include <cstdint>

// Text anchoring (VTE/Tilix behaviour): a selection, a search match or the hovered link sticks to
// its TEXT, not to a screen position — output that scrolls the text up carries the highlight with
// it, and once the text has left the scrollback for good the highlight goes too.
//
// Positions are kept as LINE NUMBERS: TermScreen::lineBase() + absRow, where absRow is TermScreen's
// row space ([0, sbCount) scrollback oldest first, then the live rows) and lineBase() counts the
// lines that have left that space for good (evicted at the scrollback limit, trimmed, cleared).
// A line's number therefore never changes while it is retained: a push moves it from live row r to
// r-1 while sbCount grows by one (below the limit) or lineBase grows by one (at the limit); a pop
// (terminal grown taller) moves it from the scrollback back to live row 0 without changing either
// sum. absRow alone (the previous scheme) shifts by one per push once the ring is full — the drift
// this replaces.
//
// Pure and Qt-free: TerminalView converts at its edges (cellAt, the renderer, selectedText, search)
// with a base/sbCount/offset triple read together under the screen lock.

using LineNo = int64_t;

struct LinePoint {
    LineNo line = 0;
    int col = 0;
};

inline bool operator==(const LinePoint &a, const LinePoint &b) { return a.line == b.line && a.col == b.col; }
inline bool operator!=(const LinePoint &a, const LinePoint &b) { return !(a == b); }
inline bool lineBefore(const LinePoint &a, const LinePoint &b) { return a.line < b.line || (a.line == b.line && a.col < b.col); }

// The line number of TermScreen absolute row `absRow`, and back (negative: evicted).
inline LineNo lineOfAbsRow(uint64_t base, int absRow) { return LineNo(base) + absRow; }
inline int64_t absRowOfLine(uint64_t base, LineNo line) { return line - LineNo(base); }

// The line shown on the top row of a view scrolled `offset` lines back from live.
inline LineNo viewTopLine(uint64_t base, int sbCount, int offset) { return LineNo(base) + sbCount - offset; }

// The view row (0 = top) a line sits on; outside [0, rows) when it is not in view.
inline int64_t viewRowOfLine(LineNo top, LineNo line) { return line - top; }

// The oldest line still retained (scrollback index 0, or live row 0 with no scrollback).
inline LineNo oldestLine(uint64_t base) { return LineNo(base); }

// An ordered span [from, to] against the retained lines: Kept as is, Clipped (its start was evicted:
// it now starts at the oldest retained line, column 0) or Gone (wholly evicted — clear it; VTE
// deselects when the selected text falls off the scrollback).
enum class Eviction { Kept, Clipped, Gone };
inline Eviction clipEvicted(LinePoint &from, const LinePoint &to, uint64_t base)
{
    if (to.line < oldestLine(base))
        return Eviction::Gone;
    if (from.line < oldestLine(base)) {
        from = { oldestLine(base), 0 };
        return Eviction::Clipped;
    }
    return Eviction::Kept;
}
