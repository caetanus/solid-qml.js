#pragma once

#include <vterm.h>

// Fill cells [from, to) of a line handed back to libvterm by sb_popline with REAL blanks: width 1
// and the terminal's default colours (what libvterm's own clearcell gives an erased cell).
//
// A value-initialised VTermScreenCell is NOT blank: its width is 0 and its colours are explicit RGB
// black. libvterm's backfill loop (screen.c resize_buffer, 0.3.3) advances by each cell's width —
// `pos.col += sb_buffer[pos.col].width` — so a width-0 padding cell never advances: an infinite
// loop at 100% CPU, freezing every pane, the first time a line pushed while the terminal was
// narrower comes back after it grew wider and then taller (a workspace move, a resize).
// `fg`/`bg` come from vterm_state_get_default_colors (they carry the DEFAULT flags).
inline void fillBlankCells(VTermScreenCell *cells, int from, int to, const VTermColor &fg, const VTermColor &bg)
{
    VTermScreenCell blank {};
    blank.width = 1;
    blank.fg = fg;
    blank.bg = bg;
    for (int i = from; i < to; ++i)
        cells[i] = blank;
}
