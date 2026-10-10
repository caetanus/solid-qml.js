// libvterm scrollback round-trip: lines pushed while the terminal is NARROW come back (sb_popline)
// after it grew wider and then taller — what moving the window to another workspace or resizing does.
// The padding cells must be real blanks (width 1, default colours): libvterm's backfill loop
// advances by each cell's width, so a width-0 padding cell loops forever (100% CPU, frozen window).
// Registered with a short meson timeout: the bug shows up as a timeout.
#include "../native/scrollbackcells.h"

#include <vterm.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

struct Scrollback {
    VTerm *vt = nullptr;
    std::vector<std::vector<VTermScreenCell>> lines;
};

int pushLine(int cols, const VTermScreenCell *cells, void *user)
{
    static_cast<Scrollback *>(user)->lines.emplace_back(cells, cells + cols);
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
    const int n = std::min(cols, int(line.size()));
    std::copy(line.begin(), line.begin() + n, cells);
    VTermColor fg, bg;
    vterm_state_get_default_colors(vterm_obtain_state(sb->vt), &fg, &bg);
    fillBlankCells(cells, n, cols, fg, bg);
    return 1;
}

} // namespace

int main()
{
    VTerm *vt = vterm_new(5, 10);
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    Scrollback sb;
    sb.vt = vt;
    VTermScreenCallbacks callbacks {};
    callbacks.sb_pushline = pushLine;
    callbacks.sb_popline = popLine;
    vterm_screen_set_callbacks(screen, &callbacks, &sb);
    vterm_screen_reset(screen, 1);

    // 20 lines into a 5x10 screen: the first ones scroll into the scrollback at 10 columns.
    for (int i = 0; i < 20; ++i) {
        char buf[32];
        const int len = std::snprintf(buf, sizeof buf, "line %d\r\n", i);
        vterm_input_write(vt, buf, size_t(len));
    }
    if (sb.lines.empty()) {
        std::fprintf(stderr, "setup failed: nothing reached the scrollback\n");
        return 1;
    }

    // Wider first (rows unchanged: nothing comes back yet), THEN taller: libvterm now pulls the
    // 10-column lines back as 30-column rows — 20 padding cells each. A window manager moving the
    // window to another workspace delivers exactly such a sequence of configures.
    vterm_set_size(vt, 5, 30);
    vterm_set_size(vt, 20, 30);

    // A backfilled row's padding is a default blank, not a zeroed (black, width-0) cell.
    VTermScreenCell cell;
    vterm_screen_get_cell(screen, VTermPos { 0, 25 }, &cell);
    if (cell.width != 1 || !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)) {
        std::fprintf(stderr, "padding cell is not a default blank (width %d)\n", int(cell.width));
        return 1;
    }
    std::puts("scrollback round-trip ok");
    vterm_free(vt);
    return 0;
}
