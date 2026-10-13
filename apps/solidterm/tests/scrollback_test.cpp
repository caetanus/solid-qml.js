// libvterm scrollback behaviour of SolidTerm's TerminalView, without Qt (same callback shapes):
//
// 1. Round-trip: lines pushed while the terminal is NARROW come back (sb_popline) after it grew
//    wider and then taller — what moving the window to another workspace or resizing does. The
//    padding cells must be real blanks (width 1, default colours): libvterm's backfill loop advances
//    by each cell's width, so a width-0 padding cell loops forever (100% CPU, frozen window).
//    Registered with a short meson timeout: that bug shows up as a timeout.
// 2. Anchor: while the user is scrolled back, output that pushes lines into the scrollback keeps the
//    SAME line at the top of the view (a TUI redrawing a spinner must not drag the view around).
//
// Both run twice: against libvterm's VTermScreen (the callback shapes TerminalView used to have) and
// against TermScreen, SolidTerm's own screen buffer that replaced it (termscreen.h).
#include "../native/scrollanchor.h"
#include "../native/scrollbackcells.h"
#include "../native/termscreen.h"

#include <vterm.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Scrollback {
    VTerm *vt = nullptr;
    std::vector<std::vector<VTermScreenCell>> lines;
    int offset = 0; // lines scrolled back from live, like TerminalView::m_scrollOffset
};

// Same shape as TerminalView::cbSbPushLine (no size limit here).
int pushLine(int cols, const VTermScreenCell *cells, void *user)
{
    auto *sb = static_cast<Scrollback *>(user);
    sb->lines.emplace_back(cells, cells + cols);
    sb->offset = offsetAfterPush(sb->offset, int(sb->lines.size()));
    return 1;
}

// Same shape as TerminalView::cbSbPopLine.
int popLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *sb = static_cast<Scrollback *>(user);
    if (sb->lines.empty())
        return 0;
    const std::vector<VTermScreenCell> line = std::move(sb->lines.back());
    sb->lines.pop_back();
    sb->offset = clampOffset(sb->offset, int(sb->lines.size()));
    const int n = std::min(cols, int(line.size()));
    std::copy(line.begin(), line.begin() + n, cells);
    VTermColor fg, bg;
    vterm_state_get_default_colors(vterm_obtain_state(sb->vt), &fg, &bg);
    fillBlankCells(cells, n, cols, fg, bg);
    return 1;
}

VTerm *newTerm(Scrollback &sb, VTermScreenCallbacks &callbacks, int rows, int cols)
{
    VTerm *vt = vterm_new(rows, cols);
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    sb.vt = vt;
    callbacks = {};
    callbacks.sb_pushline = pushLine;
    callbacks.sb_popline = popLine;
    vterm_screen_set_callbacks(screen, &callbacks, &sb);
    vterm_screen_reset(screen, 1);
    return vt;
}

void writeLines(VTerm *vt, int from, int count)
{
    for (int i = from; i < from + count; ++i) {
        char buf[32];
        const int len = std::snprintf(buf, sizeof buf, "line %d\r\n", i);
        vterm_input_write(vt, buf, size_t(len));
    }
}

std::string lineText(const std::vector<VTermScreenCell> &cells)
{
    std::string out;
    for (const VTermScreenCell &c : cells)
        out += (c.chars[0] && c.chars[0] < 0x80) ? char(c.chars[0]) : ' ';
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// The line at the top of a scrolled-back view (offset > 0 rows into the scrollback).
std::string viewTop(const Scrollback &sb)
{
    // offset 0 = the live screen (no scrollback line on top); out of range = a broken anchor.
    if (sb.offset <= 0)
        return "<live screen>";
    if (size_t(sb.offset) > sb.lines.size())
        return "<offset past the oldest line>";
    return lineText(sb.lines[sb.lines.size() - size_t(sb.offset)]);
}

bool roundTrip()
{
    Scrollback sb;
    VTermScreenCallbacks callbacks;
    VTerm *vt = newTerm(sb, callbacks, 5, 10);
    writeLines(vt, 0, 20); // the first lines scroll into the scrollback at 10 columns
    if (sb.lines.empty()) {
        std::fprintf(stderr, "roundTrip setup: nothing reached the scrollback\n");
        return false;
    }
    // Wider first (rows unchanged: nothing comes back yet), THEN taller: libvterm now pulls the
    // 10-column lines back as 30-column rows — 20 padding cells each. A window manager moving the
    // window to another workspace delivers exactly such a sequence of configures.
    vterm_set_size(vt, 5, 30);
    vterm_set_size(vt, 20, 30);
    // A backfilled row's padding is a default blank, not a zeroed (black, width-0) cell.
    VTermScreenCell cell;
    vterm_screen_get_cell(vterm_obtain_screen(vt), VTermPos { 0, 25 }, &cell);
    vterm_free(vt);
    if (cell.width != 1 || !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)) {
        std::fprintf(stderr, "roundTrip: padding cell is not a default blank (width %d)\n", int(cell.width));
        return false;
    }
    return true;
}

bool anchoredWhileScrolledBack()
{
    Scrollback sb;
    VTermScreenCallbacks callbacks;
    VTerm *vt = newTerm(sb, callbacks, 5, 20);
    writeLines(vt, 0, 30);
    sb.offset = 3; // the user scrolled back three lines
    const std::string before = viewTop(sb);
    writeLines(vt, 30, 10); // the program keeps printing: ten more lines enter the scrollback
    const std::string after = viewTop(sb);
    vterm_free(vt);
    if (before.empty() || before != after) {
        std::fprintf(stderr, "anchor: view top was '%s', became '%s'\n", before.c_str(), after.c_str());
        return false;
    }
    return true;
}

// ── the same two scenarios on TermScreen, with TerminalView's hook bookkeeping ──────────────────
struct TermSb {
    TermScreen screen;
    int offset = 0;
};

void tsPushed(int count, void *user)
{
    auto *t = static_cast<TermSb *>(user);
    t->offset = offsetAfterPush(t->offset, count);
}

void tsPopped(int count, void *user)
{
    auto *t = static_cast<TermSb *>(user);
    t->offset = clampOffset(t->offset, count);
}

const TermScreenHooks kTsHooks = { nullptr, nullptr, nullptr, nullptr, tsPushed, tsPopped };

VTerm *newTermScreen(TermSb &t, int rows, int cols)
{
    VTerm *vt = TermScreen::newVTerm(rows, cols);
    vterm_set_utf8(vt, 1);
    t.screen.attach(vt, &kTsHooks, &t);
    t.screen.reset(true);
    return vt;
}

std::string lineText(const TermScreen &s, int absRow)
{
    const TermScreen::Line l = s.line(absRow);
    std::string out;
    for (int c = 0; c < l.cols; ++c) {
        const uint32_t ch = l.cells[c].chars[0];
        out += (ch && ch < 0x80) ? char(ch) : ' ';
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

bool roundTripTermScreen()
{
    TermSb t;
    VTerm *vt = newTermScreen(t, 5, 10);
    writeLines(vt, 0, 20);
    if (t.screen.sbCount() == 0) {
        std::fprintf(stderr, "roundTrip(TermScreen) setup: nothing reached the scrollback\n");
        vterm_free(vt);
        return false;
    }
    vterm_set_size(vt, 5, 30);
    vterm_set_size(vt, 20, 30);
    VTermScreenCell cell;
    t.screen.cell(t.screen.liveLine(0), 25, &cell);
    const std::string top = lineText(t.screen, t.screen.sbCount()); // first live row: a popped line
    vterm_free(vt);
    if (cell.width != 1 || !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)) {
        std::fprintf(stderr, "roundTrip(TermScreen): padding cell is not a default blank (width %d)\n", int(cell.width));
        return false;
    }
    if (top.rfind("line ", 0) != 0) {
        std::fprintf(stderr, "roundTrip(TermScreen): the popped line reads '%s'\n", top.c_str());
        return false;
    }
    return true;
}

bool anchoredTermScreen()
{
    TermSb t;
    VTerm *vt = newTermScreen(t, 5, 20);
    writeLines(vt, 0, 30);
    t.offset = 3;
    const std::string before = lineText(t.screen, t.screen.sbCount() - t.offset);
    writeLines(vt, 30, 10);
    const std::string after = lineText(t.screen, t.screen.sbCount() - t.offset);
    vterm_free(vt);
    if (before.empty() || before != after) {
        std::fprintf(stderr, "anchor(TermScreen): view top was '%s', became '%s'\n", before.c_str(), after.c_str());
        return false;
    }
    return true;
}

} // namespace

int main()
{
    // Every case always runs, so one failure does not hide another.
    const bool roundTripOk = roundTrip();
    const bool anchorOk = anchoredWhileScrolledBack();
    const bool tsRoundTripOk = roundTripTermScreen();
    const bool tsAnchorOk = anchoredTermScreen();
    const bool ok = roundTripOk && anchorOk && tsRoundTripOk && tsAnchorOk;
    std::puts(ok ? "scrollback ok" : "scrollback FAILED");
    return ok ? 0 : 1;
}
