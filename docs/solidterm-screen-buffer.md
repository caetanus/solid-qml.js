# SolidTerm screen buffer: our own cells on top of libvterm's state layer

Status: design, branch `st-buffer` (owner-approved proposal P1 of
`docs/evidence/solidterm/perf-2026-10-13.md`). The implementation follows in the next commits.

## Why

With `VTermScreen` in use, 86.8% of `seq 1 5000000` was inside libvterm's `vterm_scroll_rect`:

- `moverect_internal` memmoves the whole cell buffer (105x56 cells x 36 B, about 208 KB) for
  **every** line that scrolls;
- the row that leaves the top is copied cell by cell (`vterm_screen_get_cell`) into a scratch
  `VTermScreenCell` array and handed to `sb_pushline`, which copied it again into our scrollback.

A terminal scrolls one line per linefeed at the bottom, so the cost of a line was O(screen), not
O(line).

## What changes

`VTermScreen` is dropped. SolidTerm drives `VTermState` (parser, escape handling, modes, pen,
charsets, tab stops, line info, mouse/keyboard encoding: all unchanged) with its own
`VTermStateCallbacks`, implemented by `TermScreen` (`apps/solidterm/native/termscreen.{h,cpp}`,
Qt-free so it is tested without a GUI).

### Storage

```
TermCell  = { uint32 chars[6]; TermPen pen }          36 B, POD, same layout as libvterm's ScreenCell
TermPen   = { VTermColor fg, bg; bitfields (bold, underline:2, italic, blink, reverse, conceal,
              strike, font:4, small, baseline:2, protected_cell, dwl, dhl:2) }
TermRow   = { TermCell *cells; uint32 cap; uint32 cols }  16 B handle to one malloc'ed row
```

- A screen buffer is a **table of row handles** (`std::vector<TermRow>`, one per visible row).
  Each row is one contiguous allocation; the cells of a row are never copied to scroll.
- The primary and the alternate screen are two such tables; the active one is a pointer.
- The scrollback is a **ring of row handles** (oldest at `head`), capacity = the scrollback limit.
  It grows by `push_back` until it reaches the limit and then rotates.

### Scrolling

`scrollrect(rect, downward, rightward)` is answered entirely by `TermScreen` (it returns 1, so
libvterm never falls back to `moverect`/`erase`):

- **Full-width vertical scroll** (`start_col == 0`, `end_col == cols`, `rightward == 0`; this is
  every linefeed at the bottom, `IL`/`DL`, `SU`/`SD`, `RI`, DECSTBM regions): the row handles inside
  `[start_row, end_row)` are moved (`memmove` of at most `rows` 16-byte handles, under 1 KB). The
  handles that fall out of the region are reused for the rows that come in, which are then
  erased. No cell is copied.
- **Scrollback push**: when the region starts at row 0 on the primary screen (libvterm's own
  condition in `moverect_internal`: `dest.start_row == 0`, full width, not altscreen), each row that
  leaves the top is **moved** into the scrollback ring (its handle, not its cells). When the ring is
  full, the evicted oldest row's buffer is handed back and becomes the new bottom row, so a scrolling
  terminal at the limit does no allocation at all. A row whose capacity is smaller than the current
  width (pushed while the terminal was narrower) is reallocated.
- A scroll by at least the height of the region erases it without moving or pushing anything, as
  in `vterm_scroll_rect`.
- **Column scrolls** (`ICH`/`DCH`, DECSLRM margins) move cells inside each affected row with one
  `memmove` per row, exactly like `moverect_internal`.

Data-driven refinement of the brief: the brief proposed rotating a ring head for the full-screen
case. A `memmove` of at most `rows` row handles costs less than erasing the incoming row
(`cols` x 36 B), so the head index (and a modulo on every row access) buys nothing. Every row
access stays `rows[r].cells[c]`.

### Faithfulness to VTermScreen

The callbacks are a port of libvterm 0.3.3's `screen.c` (MIT) onto the row table, so the screen
contents are identical. That includes the quirks:

- the wide-character continuation cell holds `chars[0] == (uint32_t)-1`, and a cell's width is
  derived from its right neighbour, as in `vterm_screen_get_cell`;
- `erase` writes only `chars[0] = 0` plus the pen's fg/bg and the line's DWL/DHL;
  `clearcell` copies the whole pen;
- DECSCNM (global reverse) is applied when a cell is read on the live screen and baked into a row
  when it enters the scrollback;
- resize is a port of `resize_buffer` (reflow stays **off**, as before: SolidTerm never enabled
  it). That includes the scrollback push when the height shrinks and the `sb_popline` round trip
  when it grows, with the same padding (real blanks, see `scrollbackcells.h`) and the same
  cell conversion;
- two libvterm behaviours are kept on purpose, because the oracle demands identical output:
  shrinking the height while the alternate screen is active pushes the **alternate** screen's top
  rows into the scrollback (libvterm reads `screen->buffer` there), and a hard reset (`RIS`) during
  the alternate screen keeps showing it. Both are candidates for a later fix;
- `ED 3` (`CSI 3 J`, clear scrollback) stays a no-op, as before (`sb_clear` was not wired).

### Damage

- Each visible row has a dirty byte, set by `putglyph`/`erase`/scroll/`setlineinfo`; resize,
  alt-screen switches and DECSCNM mark every row. A frame-level flag says whether any row changed.
- Inside a pty batch (`beginBatch()`/`endBatch()` around `vterm_input_write`) no callback is made;
  the view calls `update()` once at the end of the batch, as it already did. Outside a batch (resize,
  reset) the first damage calls the view's `damage` hook, which calls `update()`.
- `buildPaintNode` clears the flags and, for now, still rebuilds every row. Reusing per-row
  geometry is proposal P4 and is not part of this change.

### Readers

Every reader goes through `TermScreen`'s accessor API:

| reader | before | after |
|---|---|---|
| renderer (`buildPaintNode`) | `vterm_screen_get_cell` per cell (call + 40 B copy) / `m_scrollback[i].cells` | `TermScreen::line(absRow)` → contiguous `TermCell` row, read in place |
| selection text, search (`rowText`), links, word/line select | `vterm_screen_get_cell` / `m_scrollback` | `line(absRow)` + `TermScreen::cell()` (a `VTermScreenCell` view, same values as `vterm_screen_get_cell`) |
| scrollbar, autoscroll, anchor, pause, search scroll | `m_scrollback.size()` | `sbCount()` |
| line continuation (link join) | `vterm_state_get_lineinfo` | unchanged |
| colours | `vterm_screen_convert_color_to_rgb` | `vterm_state_convert_color_to_rgb` (what it called) |

`absRow` keeps its meaning: `[0, sbCount)` is the scrollback (oldest first), `[sbCount,
sbCount + rows)` is the live screen.

The behaviour of an out-of-range column on a scrollback row narrower than the screen is unchanged:
it reads as a value-initialised cell (as `VTermScreenCell cell {}` did).

### C++ rules

No `std::function`, no virtual calls, no allocation per cell or per line in steady state (a row is
allocated only while the scrollback ring is still growing, or when a recycled row is narrower than
the screen). Host notifications are one plain function-pointer table (`TermScreenHooks`) plus a
`void *`, the same shape as libvterm's callbacks. The VTerm is built with an explicit
`calloc`/`free` allocator, so `TermScreen` can reallocate libvterm's line-info arrays in `resize`
(the `VTermStateFields` contract) with the same allocator.

## Correctness gates

1. Every existing SolidTerm test (scrollback round trip incl. resize, anchor, mouse report,
   autoscroll, links, scrollbar, pause, pane drop) passes. The scrollback round trip and anchor
   scenarios also run against `TermScreen`.
2. **Oracle** (`apps/solidterm/tests/termscreen_oracle_test.cpp`): the previous path (VTermScreen
   plus the exact push/pop/limit logic `TerminalView` had) is kept compiled as a test-only reference
   model. A corpus of escape sequences (cursor movement, scroll regions, IL/DL/ICH/DCH/ECH, erase
   variants incl. selective erase, wide chars, combining marks, SGR attributes and colours, alt
   screen enter/exit, DECAWM, DECSCNM, DECDWL, DECSLRM, resize narrower/wider/shorter/taller incl.
   in the alt screen, a small scrollback limit to exercise eviction) and a seeded random stream are
   fed to both. After every step it asserts identical cells (chars, width, attributes, colours) for
   the whole screen, identical cursor, and an identical scrollback.
3. GPU pixel parity of the fixed screen (`scripts/solidterm-parity.sh`): byte-identical grabs old
   vs new.
4. Real programs (htop, lvim, less, a scroll-region script) in the real window through a
   self-quitting hook: grabs old vs new compared.

## Performance gate

`scripts/solidterm-bench.sh` (seq, mixed, dense, region, latency, wheel), before/after vs Tilix,
in the results doc. Targets: seq 33 s → ≤ 7 s, mixed 12 s → ≤ 5 s, no regression in dense, latency
or frame pacing.
