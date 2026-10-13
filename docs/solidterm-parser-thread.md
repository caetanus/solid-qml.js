# SolidTerm: pty reading and VT parsing off the GUI thread

Status: owner-approved (proposal P2 of `docs/evidence/solidterm/perf-2026-10-13.md`), implemented on
branch `st-thread`. Measurements: `docs/evidence/solidterm/parser-thread-2026-10-13.md`.

## Why

During an output flood the GUI thread spends ~95% of a core reading the pty and running libvterm
(`vterm_input_write` → `TermScreen` callbacks). Everything else that runs on that thread (key and
mouse events, QML bindings, timers, the scene-graph sync) waits behind whole reads. The goal is
responsiveness: input and UI stay on time during a flood. Throughput must not regress.

The stall soak that preceded this work (same evidence file, "Step 0") also showed how fragile the
single-threaded design is: on Wayland the GUI loop can phase-lock into one turn per vsync, and with
one pty read per turn a 100 MB `cat` drained at 250 KB/s. That is fixed on the single-threaded path
by draining with a time budget (`PtySession::onReadable`), but with a parser thread the drain rate no
longer depends on the GUI loop at all.

## Thread model

**One worker thread per terminal session** (per pane), a plain `std::thread` running a `poll(2)`
loop. Not a pool: a session's parsing is strictly sequential, a pool would add hand-off for nothing,
and an idle worker costs one blocked thread and two file descriptors. Not a `QThread` with an event
loop: the loop is two fds and a timeout, and owning it keeps the hot path free of Qt event objects.

```
              GUI thread                       worker thread (per pane)            render thread
  keys/mouse ──► vterm_keyboard_* ─┐      poll(master, wakeFd)                      (Qt Quick)
  selection/search/links ──┐       │       │ read(master, 64 KiB)  (no lock)
  resize (vterm_set_size) ─┤       │       ▼
                           ▼       ▼   ┌──────────── ScreenMutex ────────────┐
                      [lock] VTerm + VTermState + TermScreen + out-queue ◄──┤ [lock] slice ≤ 16 KiB:
                           ▲                                                │ vterm_input_write
           applyNews() ◄───┼── guiWakeFd (eventfd, coalesced, ≤ 1 / 4 ms) ◄─┘ hooks record "news"
                           │
  updatePaintNode (GUI blocked) ──[lock]── copy visible rows → render snapshot, clear dirty ──► build geometry (no lock)
```

### Ownership

| state | owner | others |
|---|---|---|
| pty master **read** side, the poll loop, the 64 KiB read buffer, the carry of a paused slice | worker | — |
| `VTerm` (parser, `VTermState`, keyboard/mouse encoders, output buffer), `TermScreen` (rows, alt screen, scrollback ring, dirty bits), the to-pty out-queue, the "news" record | shared, **only under `ScreenMutex`** | — |
| scroll offset, selection, search matches, hover link, GUI copies of cursor / title / sbCount / altscreen / mouse mode, everything QML reads | GUI | the render thread reads them during the sync (GUI blocked), as before |
| render snapshot (visible rows copied out of `TermScreen`), glyph atlas, scene-graph nodes | render thread, inside `updatePaintNode` | — |

`PtySession` still forks the shell, owns the master fd, resizes it (`TIOCSWINSZ`), reaps the child and
hangs it up on destruction. In threaded mode it does not create its `QSocketNotifier`; the worker
reads the fd instead.

### The lock: `ScreenMutex`

One `std::mutex` per session plus an atomic count of **waiting readers** (GUI/render):

- The worker parses in **slices** (at most 16 KiB, about 0.2 to 1 ms of libvterm work depending on
  the content): lock, `beginBatch` / `vterm_input_write(slice)` / `endBatch`, unlock. A 64 KiB read is
  up to four slices, so a reader never waits for more than one slice.
- After every slice the worker checks the waiting count. If a reader is waiting it yields until that
  reader has *acquired* the mutex (microseconds) before locking again. Without this the worker would
  re-take the mutex in a tight loop (futex barging) and starve the renderer.
- Readers (`ScreenLock` RAII on the GUI or the render thread) increment the count, lock, decrement.

No per-cell locking, no reader/writer lock (readers never overlap: the renderer only runs while the
GUI thread is blocked), no lock-free cell copies.

### How the renderer reads the screen without tearing

`updatePaintNode` runs on the render thread while the GUI thread is blocked. It takes the
`ScreenMutex` once and **copies the visible rows into a render-side snapshot** (`rows × cols`
`TermCell`s plus, per row, its length and whether it is a live row), together with the cursor,
DECSCNM, `sbCount`, and the effective scroll offset. Then it clears `TermScreen`'s dirty bits,
unlocks, and builds the geometry from the snapshot with no lock held:

- live view (offset 0), same size, same DECSCNM: only rows marked dirty are copied (an idle frame,
  e.g. the cursor blink, copies nothing);
- otherwise (scrolled back, resized, alt-screen switch, DECSCNM flip, first frame after a window
  change) every visible row is copied: 105 × 56 × 36 B = 212 KB, a few tens of microseconds.

The snapshot is allocated once per grid size (no per-frame allocation). Colours come from a 256-entry
table filled from `vterm_state_get_palette_color` when the session starts (libvterm never changes the
palette afterwards: it handles no OSC 4), so the renderer no longer calls into `VTermState`.

**Scroll anchoring across threads.** A scrolled-back view must keep showing the same lines while
the worker pushes new ones. `TermScreen` counts every line ever pushed (`pushedTotal`, 64-bit). The
GUI remembers the count it last applied; the renderer computes the effective offset as
`offset > 0 ? min(offset + (pushedTotal - seen), sbCount) : 0` (the same result as applying
`offsetAfterPush` once per line), and the GUI applies the same formula when it processes the news.
Both sides agree without the renderer writing GUI state.

### How news reaches the GUI

The `TermScreen` hooks (`movecursor`, `settermprop`, `bell`, `sbPushed`, `sbPopped`, `damage`) run on
the worker under the lock. They no longer touch the view's GUI state or emit signals: they record
**news** (cursor and visibility, mouse mode, altscreen, title text + changed flag, bell count, "scroll
moved", "content damaged", "child exited").

The worker then wakes the GUI through an **eventfd watched by a `QSocketNotifier`** on the GUI
thread: no allocation, no `QEvent`, no `invokeMethod`. Coalescing: an atomic "posted" flag, so there
is at most one wake in flight; and a minimum interval of **4 ms** between wakes (when news arrives
sooner the worker polls with a timeout until the interval has passed). The first news after a quiet
period is posted at once, so a keystroke echo is not delayed; during a flood the GUI gets at most 250
wakes per second instead of one per slice.

`applyNews()` (GUI, under the lock, short) copies the news into the GUI-side copies, applies the
scroll anchoring, then (after unlocking) emits `scrollChanged` / `titleChanged` / `bellRang`, restarts
the blink phase, and calls `update()`. All signals are emitted on the GUI thread, as before.

### Input to the pty

Keys, paste, mouse reports, focus reports and the libvterm responses (DA, DSR, …) all go through
libvterm's output callback, which now only appends to the **out-queue** (under the lock, which every
caller already holds). Whoever holds the lock then tries a **non-blocking** `write(2)` of the queue
(the master is `O_NONBLOCK` in threaded mode). Bytes the kernel does not take stay queued; the worker
adds `POLLOUT` to its poll and writes them when the slave drains. So:

- a keystroke is written by the GUI thread at once, as before (no added latency, no thread hop);
- no thread ever blocks in `write(2)`. The single-threaded path blocks there, which deadlocks when a
  large paste goes to a program that is itself blocked writing output nobody reads (the terminal is
  stuck in the write). The queue removes that hazard.

`vterm_keyboard_*`, `vterm_mouse_*` and `vterm_state_focus_*` read libvterm's modes and write its
output buffer, so the GUI calls them under the lock. The wait is at most one slice.

### Resize: a synchronous hand-off

`applyGrid` (GUI) takes the lock, calls `vterm_set_size` (`TermScreen::cbResize` runs on the GUI
thread while the worker is held off), unlocks, sends `TIOCSWINSZ`, and applies the news at once. The
worker cannot observe a half-resized screen, and the renderer only ever sees the size the GUI has.

### Selection, search, links, scrollbar, autoscroll

Every GUI-thread reader takes the lock for the duration of its read: `cellAt` (it also applies
pending news first, so its absolute row agrees with what was rendered), `selectedText`, `rowText` (word
and line select, links, search), `linkAt` (also reads `vterm_state_get_lineinfo`, which the resize
reallocates), `clearScrollback`, `setScrollbackLimit`, `resetTerminal`. These are short except
`search`, which walks the whole scrollback (a few milliseconds at 8000 lines): the worker waits that
long, once, when the user searches. The scrollbar and every QML-visible property read the GUI-side
copies and never lock.

### Pause on selection

`pauseOutputForSelection()` sets the worker's paused flag **under the lock**: once it returns no
slice is being parsed and none will start. The worker stops polling `POLLIN` (the writer then blocks
on the full kernel buffer, as with the disabled notifier before) and keeps the unparsed rest of its
current read as a carry. `resume` clears the flag and wakes the worker, which parses the carry first.
Nothing is buffered beyond one read (≤ 64 KiB), nothing is lost.

### Shutdown and pane close mid-flood

`~TerminalView` first stops the worker: set the stop flag, wake it through its eventfd, `join()`.
The worker is either in `poll` (wakes at once) or in a slice (finishes it, under a millisecond), and
it never waits on the GUI thread, so the join cannot deadlock. Only then are the GUI wake notifier,
`VTerm`, `TermScreen` and the pty (SIGHUP + close) torn down. A wake that was already queued for the
GUI is a level on an eventfd owned by the view; its notifier is destroyed with the view, so nothing
fires into a dead object. The render thread cannot be inside `updatePaintNode` during the destructor
(Qt Quick deletes items on the GUI thread outside the sync).

### Detach and reattach across windows

The `TerminalView` object is re-parented into another window; the worker belongs to the view and
does not notice. `ItemSceneChange` already drops the cached nodes and the atlas; it now also marks the
render snapshot stale (a full copy on the next frame). The new window's render thread takes the same
`ScreenMutex`.

### The single-threaded path stays

`SOLIDTERM_PARSER_THREAD=0` (read once at startup) keeps the `QSocketNotifier` path: the GUI reads
(with the time-budgeted drain), parses, and applies the news right after each read. It uses the same
news record, the same snapshot renderer and the same lock (uncontended there), so the two modes differ
only in which thread runs the read and the parse. It is the A/B baseline and the fallback.

## C++ rules (owner)

- No `std::function`, no virtual call, no allocation per chunk or per slice. Hooks stay a plain
  function-pointer table; the worker's sink is one as well.
- The per-slice cost added by threading is one uncontended `std::mutex` lock/unlock pair and one
  relaxed atomic load.
- The worker's buffers (read buffer, carry, out-queue) are allocated once and only grow (the
  out-queue grows with a large paste).

## Risks and how they are checked

| risk | check |
|---|---|
| data races (any unlocked access to `TermScreen`/`VTerm`) | ThreadSanitizer build: all solidterm tests plus a flood scenario that exercises selection, search, links, keys, resize and scrollback limits during output |
| renderer starved by the worker | reader hand-off; measured as `updatePaintNode` duration and frame pacing during floods |
| GUI-thread latency | a 5 ms GUI timer's lateness (max / p95) during `seq`; key → frame latency during a flood |
| throughput regression | full bench single vs threaded vs Tilix |
| screen content changes | TermScreen oracle test, GPU pixel parity of the fixed screen |
| use-after-free on close mid-flood | an ASan/TSan run closing panes during a flood |
