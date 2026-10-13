# SolidTerm parser thread: stall soak, design, measurements (2026-10-13)

Branch `st-thread` (from `widgets` 34032ec). Design: `docs/solidterm-parser-thread.md`. Same setup as
`perf-2026-10-13.md`: headless sway on the Intel UHD 630 (`WLR_BACKENDS=headless`,
`WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128`, 1896x2020@60 Hz scale 2, xwayland off), the owner's
SolidTerm config (copied into a private `XDG_CONFIG_HOME`), grid 105x56, release build
(`qml_debug=false`), bench inputs from `scripts/solidterm-bench-gen.py`.

## Step 0: the open stall risk

The P1 final bench had one `mixed` run (100 MB `cat`) that timed out at 300 s: the terminal stopped
consuming the pty. 90 targeted repeats had not reproduced it.

### Harness

- `apps/solidterm/native/stallwatch.h` (env `SOLIDTERM_STALLWATCH=<file>`): a watchdog thread in the
  app that every 500 ms checks each pty session and the GUI thread. It reports (once per episode) when
  the GUI heartbeat (a 100 ms QTimer) is older than 5 s, when nothing was read for 5 s while the
  kernel holds unread bytes (`FIONREAD` on the master), or when the master held unread bytes at every
  tick of a 5 s window and less than 2 MB was drained in it ("slow drain"). The report has the pending
  bytes, the pause flag, the notifier state, bytes and reads so far, the age of the last read and of
  the heartbeat, and every thread's state and wchan. It also sets `PR_SET_PTRACER_ANY` so the soak
  can attach gdb under yama `ptrace_scope=1`.
- `scripts/solidterm-soak.py`: runs the bench runner many times (cases interleaved), and on a report
  or a timeout captures the context switches of every thread over 2 s, `/proc` state and wchan of the
  terminal's threads and of its descendants (the shell, `cat`), and `gdb -p` backtraces of all
  threads; then kills only the process it spawned.

### Result: reproduced on the first runs, about 1 mixed run in 2

| binary | mixed | seq | region |
|---|---|---|---|
| 34032ec + watchdog (pre-fix) | **18 of 37 stalled** (first sessions: 1/1, 4/10, 3/6, 10/20) | 0 of 16 | 0 of 16 |

The earlier session had hit it 1 time in 5. Here it was immediate, probably because the private
runtime directory changes the timing a little; the mechanism below explains why it is phase-dependent.

What every captured stall shows:

- `cat` is blocked writing (`wait_woken`), the master holds 4095 unread bytes;
- the terminal is **not** deadlocked: the GUI thread and the render thread each switch context ~61
  times per second, the heartbeat is fresh, the notifier is enabled, the session is not paused;
- the watchdog's drain window: **1 265 355 bytes in 309 reads in 5 s**, every read exactly 4095 bytes,
  i.e. **one read per vsync** (≈ 62 per second, 250 KB/s). At that rate the 100 MB file needs ~7 min.
  The stall begins at the same point of the file in every run (~0.9 MB in, where the first burst of
  new glyphs makes one long frame) but only in some runs;
- gdb: the GUI thread sits in `QQuickWindow::event(UpdateRequest)` → the threaded render loop's
  `polishAndSync`, waiting for the render thread; the render thread sits in
  `QWaylandWindow::waitForFrameSync` (from `QWaylandGLContext::swapBuffers`) or in our
  `updatePaintNode` during the sync.

With `QT_LOGGING_RULES=qt.qpa.wayland.backingstore=true` (timestamped), healthy and stalled phases
differ only in order: healthy frames go `handleUpdate` (render thread commits) → `deliverUpdateRequest`
→ `requestUpdate`; stalled frames go `handleUpdate` → `requestUpdate` → `deliverUpdateRequest` at once.

### Root cause

A phase lock between Qt Quick's threaded render loop and QtWayland's frame-callback handling
(QtWayland 6.11, `QWaylandWindow`), made fatal by SolidTerm reading one pty chunk per event-loop turn:

1. When a frame callback arrives, QtWayland queues `doHandleFrameCallback` to the GUI thread, which
   delivers a pending update request.
2. If the callback arrives while the GUI thread is blocked in `polishAndSync` (waiting for the render
   thread, which is itself waiting in `waitForFrameSync` for that callback), the queued call runs only
   after the sync. By then the render thread has already committed the next frame and asked for the
   next callback.
3. Back on the GUI thread, the next loop turn reads ONE pty chunk (≈ 4 KiB), whose handler calls
   `update()` (an update request is now pending), then runs the stale `doHandleFrameCallback`, which
   delivers it: `polishAndSync` again, which blocks until the render thread finishes the frame it is
   in, i.e. until the next vsync. That vsync's callback again lands while the GUI thread is blocked.

Each turn of the GUI loop costs one vsync and drains one chunk. A long frame (a glyph burst) is what
shifts the phase into this state; nothing shifts it back while output keeps coming. This is not a
deadlock and not specific to the headless compositor: any Wayland compositor delivers callbacks
this way.

### Fix (single-threaded path): drain with a time budget

`PtySession::onReadable` now keeps reading while `FIONREAD` reports queued bytes, up to 8 ms per
activation (and stops at once when a handler pauses reads; `EINTR`/`EAGAIN` are no longer taken for
EOF). Inside the phase lock a turn now drains ~8 ms of output instead of 4 KiB.

- Regression test `PtyPauseTest::drainKeepsUpWithBlockedLoopTurns` (in `solidterm-ptypause`): with the
  notifier disabled and a writer keeping the pty full, it fires the notifier's activation by hand
  (no event loop involved) and measures the bytes drained per activation, with a realistic 150 µs per
  4 KiB parse cost in the handler. Pre-fix: 4095 bytes (fails); fixed: ~110 KB.
- Soak after the fix (single-threaded binary, `aeb8bc8`-equivalent): **160 runs, 0 stalls** (100
  mixed, 30 seq, 30 region). Mixed median 3.76 s; 27 of the 100 mixed runs took 5.5 to 7.2 s instead
  of ~3.5 s: the phase lock still happens, and each locked turn still loses a vsync, but it now
  drains ~8 ms of output per turn instead of 4 KiB. The parser thread (below) removes the dependency
  on the GUI loop entirely.
- A captured stall (watchdog report, per-thread state, gdb backtraces):
  `parser-thread-stall-capture.txt`.

## Step 1: design

`docs/solidterm-parser-thread.md`: one worker thread per pane owns the pty read side and runs
libvterm in 4 KiB slices under a per-session `ScreenMutex` (hand-off to waiting readers); the renderer
copies the visible rows into a snapshot under the lock and builds geometry without it; the TermScreen
hooks only record news, applied on the GUI thread on a coalesced, throttled eventfd wake; input goes
through a non-blocking out-queue; resize is a synchronous hand-off; pause stops the worker under the
lock; the worker is joined before anything it uses is freed. `SOLIDTERM_PARSER_THREAD=0` keeps the
GUI-thread reader.

## Step 2: correctness

- All 10 solidterm meson tests pass in the release build (both modes run the pane-drop and the
  pause/autoscroll/scrollbar/link self-test hooks; `linktest`'s synthesized double-click is flaky in
  BOTH modes and in the pre-thread binary: 1 of 3 passes there, it depends on Qt's double-click timing,
  not on this change). New: `solidterm-termworker` (real pty, no Qt).
- TermScreen oracle: passes (TermScreen only gained a push counter).
- GPU pixel parity (`scripts/solidterm-parity.sh`, headless sway on the Intel GPU): **byte-identical
  to 34032ec** (sha256 `98af04da…`) in both modes, 5 threaded + 2 single launches. A first threaded
  version was not: the worker parsed the shell's first output while the grid was still the pre-layout
  2x2 and wrapped it at 2 columns for good; the worker now starts paused and reads from the next GUI
  loop turn, as the notifier did.
- A real program (vim with syntax colours, alt screen, a split, 120 lines) grabbed 3 times per mode:
  identical.

### ThreadSanitizer

Build: `-Db_sanitize=thread`, `debugoptimized`, linked against **libvterm 0.3.3 built with
`-fsanitize=thread`** (`pkg_config_path`), so libvterm's own memory accesses are checked too. Qt
itself is not instrumented. Two harness findings first:

- TSan does not intercept `pthread_clockjoin_np`, which Qt 6's `QThread::wait()` uses, so any Qt
  program that restarts a QThread aborts in TSan's thread registry (a 5-line QThread probe does). The
  LD_PRELOAD shim `scripts/tsan-clockjoin-shim.c` routes it to `pthread_join`/`pthread_timedjoin_np`.
- Reports are classified by whether SolidTerm or libvterm code is in the top frames of a racing
  access (not just at the bottom of a stack, like `main`).

Runs (final code):

| run | result |
|---|---|
| all 10 solidterm meson tests (offscreen) | 10 TSan reports, **0** in SolidTerm/libvterm code (all inside Qt/QtDBus/glib/libdbus) |
| `SOLIDTERM_THREADSTRESS=90000` offscreen, flooding shell | 9 770 steps (search, scroll, hover, selection drag with pause/resume, keys, scrollback limit, resize, wheel, split, close mid-flood): 11 reports, **0** ours |
| the same on the headless GPU sway (threaded render loop: renderer vs parser) | 5 687 steps: 18 reports, 2 touch our code: `KeyFilter::eventFilter` reading a `QEvent` that the Wayland event thread posted through Qt's (uninstrumented, futex-based) event queue — a false positive, unrelated to this change; **0** in TerminalView/TermWorker/TermScreen/libvterm |
| harness check (mutation: the snapshot copy without the lock) | flagged at once: races in `copySnapshot` vs `cbPutglyph`, `clearDirty`, … |

What TSan found along the way (both fixed, own commits, both predate the parser thread):

- **use-after-free when a focused pane closes**: `~QQuickItem` (after `~TerminalView` freed the
  VTerm) emits `activeFocusChanged`, whose handler called `vterm_state_focus_out` on the freed VTerm;
- **`rowText` asserted** (debug Qt) on the right half of every wide glyph (`0xFFFFFFFF` marker) and on
  non-BMP code points; release builds silently truncated them.

## Step 3: measurements

Same harness, 5 runs per case, median (p95); all four terminals in the same session (full reports from
`scripts/solidterm-bench-report.py`: `parser-thread-bench/{threaded,single,before,tilix}.md`). "before" = the
pre-thread single-threaded code with the drain fix (`aeb8bc8`; the unfixed 34032ec stalls in half the
mixed runs, so it has no usable timing); "single" / "threaded" = the final binary with
`SOLIDTERM_PARSER_THREAD=0` / default. Grid 105x56; Tilix came up at 105x46 (18% fewer cells, which
favours Tilix).

### Throughput

| case | before | single | **threaded** | Tilix 0.9 / VTE |
|---|---|---|---|---|
| `seq 1 5000000` | 2.82 s (2.91) | 2.95 s (3.08) | **2.74 s** (2.99) | 3.02 s (3.12) |
| `cat` 100 MB mixed | 3.66 s (5.72) | 3.43 s (4.59) | **3.42 s** (3.46) | 3.85 s (4.04) |
| dense cells, 50 MB | 0.54 s (0.55) | 0.55 s (0.78) | **0.49 s** (0.63) | 2.25 s (2.53) |
| scrolling region, 50 MB | 1.47 s (1.56) | 1.48 s (1.53) | **1.34 s** (1.38) | 2.06 s (2.28) |
| CPU: GUI thread / parser / render | 87–98% / – / 4–14% | 88–98% / – / 3–16% | **1–2%** / ~100% / 4–18% | 91–96% (1 thread) |
| idle CPU (10 s) | – | 0% | 0% | – |

No regression: threaded is the fastest SolidTerm in every case, and its p95 in mixed (3.46 s) shows
the phase lock no longer costs anything (single/before p95 4.6–5.7 s come from it). The parse itself
did not get faster: the gain is that the GUI work, the render sync and the parse now overlap.

### Responsiveness during a flood (`flood` case: `seq 1 15000000`, ~8.7 s)

A 5 ms precise timer on the GUI thread, and 300 keys (60 per run) injected 100 ms apart from a helper
thread through `QWindowSystemInterface` (asynchronous delivery, like the Wayland event thread):

| | single | **threaded** |
|---|---|---|
| GUI timer lateness p50 / p95 / p99 / max | 12.03 / 13.08 / 13.75 / 22.3 ms | **0.05 / 0.70 / 0.90 / 16.6 ms** |
| timer firings more than 10 ms late | 2 601 of 2 613 (it could not fire at its rate at all) | **5 of 8 615** |
| key injected → `keyPressEvent`, median / p95 / max | 3.78 / 8.07 / 8.83 ms | **0.06 / 0.17 / 0.85 ms** |
| → written to the pty | 3.78 / 8.08 ms | **0.20 / 0.46 ms** |
| → its echo parsed (behind the flood in the pty) | 4.75 / 9.00 ms | **1.45 / 2.34 ms** |
| → first `updatePaintNode` after it | 16.27 / 24.84 ms | **9.81 / 16.99 ms** |
| → frame swapped | 28.14 / 37.17 ms | **25.79 / 33.01 ms** |
| frames: fps / p99 interval / max / % over 16.7 ms | 62.4 / 16.22 / 18.0 / 0.2% | 62.4 / 16.18 / 18.0 / 0.2% |

The GUI thread is now free during a flood: events are handled within a tenth of a millisecond
instead of waiting behind a whole read. What remains of key → screen is frame pacing: the echo is on
the screen data 1.5 ms after the key, then waits for the next sync (≤ one frame) and for the swap,
which with QtWayland's frame-callback throttling presents a frame one vsync after its sync. That part
is the compositor's pace and is the same in both modes.

Frames stay at 62 fps with p99 16.2 ms in every case and both modes. updatePaintNode (render thread,
GUI blocked) costs more with the parser thread: seq 0.17 → 0.43 ms average, mixed 0.62 → 0.84 ms; of
that, waiting for the screen lock is 0.19 / 0.10 ms on average (max 2.8 / 1.8 ms: the parser
descheduled while holding it) and the row copy 0.02 ms. Well inside the 16.7 ms budget, and the GUI
thread no longer spends the rest of the frame parsing.

### Idle input latency and wheel (no flood)

| | before | single | threaded |
|---|---|---|---|
| key → frameSwapped (300 keys, `bash --norc`), median (p95) | 3.68 (4.37) ms | 3.64 (4.19) ms | 3.67 (4.87) ms |
| key → echo parsed | 0.26 ms | 0.25 ms | 0.17 ms |
| wheel, 10k scrollback, 600+600 notches at 125 Hz: fps / p99 | 60.0 / 16.48 ms | 59.9 / 16.49 ms | 59.7 / 16.49 ms |

### Stall soak with the parser thread

160 runs (100 mixed, 30 seq, 30 region), **0 stalls**; mixed median 3.20 s, max 3.89 s (the
single-threaded fixed binary: max 7.18 s, 27 of 100 runs over 5.5 s).

### Profile (frame-pointer build, perf, threaded seq)

Samples by thread over the run: parser ("st-parser") 2 825 (≈ one core for the 2.85 s measured), the
`seq` process 2 111, GUI thread 279 (mostly start-up: fontconfig, the dynamic loader), render thread 211.
The parser's profile is P1's: `TermScreen::cbPutglyph` 9.4%, libvterm's parser, `eraseInternal` 4.1%,
`cbScrollrect`/`moveRect`/`pushRow` ~2% each; locking does not show up.
