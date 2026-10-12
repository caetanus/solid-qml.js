#pragma once

#include <QString>

// SOLIDTERM_XDROPTEST=1 — a self-checking run of the pane drag & drop paths, driven through the same
// code a real drag takes (window drag events → TerminalTabs routing → TerminalPanes::finishDrag):
//   1. split window A, mark the new pane's shell PID into $SOLIDTERM_PIDLOG;
//   2. a drag nobody accepts (Esc / dropped outside) → the pane detaches into a NEW window B;
//   3. drag B's pane onto A's first pane (right zone) → it moves into A's split tree, B closes,
//      and the SAME shell (same PID, scrollback intact) answers again;
//   4. drag it onto A's tab strip → it becomes a new tab of A;
//   5. drop it onto its own pane → nothing changes (no detach).
// Exits 0 when every check passes, 1 otherwise. SOLIDTERM_XDROPSHOT=<prefix> also grabs each stage.
namespace PaneDropSelfTest {
void start(const QString &pidLog, const QString &shotPrefix);
}
