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
- Soak after the fix: see below.
