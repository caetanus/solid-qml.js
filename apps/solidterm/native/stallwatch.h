#pragma once

// Stall watchdog for the solidterm soak (scripts/solidterm-soak.py). OFF unless SOLIDTERM_STALLWATCH
// names a report file. When on, a background thread checks every 500 ms whether a pty session has
// stopped consuming its master fd: no read for `kStallSec` while the kernel still holds unread bytes
// (FIONREAD > 0), or no read at all for 2x that. It then appends a state report (per session: fd,
// pending bytes, pause flag, notifier enabled, bytes/reads so far, age of the last read, age of the
// last GUI-thread heartbeat; per thread: state + wchan) and lets any process ptrace this one
// (PR_SET_PTRACER_ANY, so the soak can attach gdb under yama ptrace_scope=1).
//
// The hot path cost when off is one predictable branch on a static bool per read. The watchdog reads
// a few session fields racily (diagnostics only, never in a TSan run).

#include <atomic>
#include <cstdint>

class PtySession;

namespace StallWatch {

inline bool on = false;

void init();                           // reads SOLIDTERM_STALLWATCH; idempotent
void add(PtySession *s);               // GUI thread
void remove(PtySession *s);            // GUI thread
void heartbeat();                      // GUI thread (driven by a QTimer while on)
int64_t nowNs();

} // namespace StallWatch
