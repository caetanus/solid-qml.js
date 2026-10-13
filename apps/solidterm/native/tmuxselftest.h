#pragma once

#include <QString>

class QQuickWindow;

// Self-checking runs of the tmux mode through the REAL UI path, offscreen (SOLIDTERM_TMUXTEST,
// docs/solidterm-tmux-control-mode.md phase 1). The app runs `--tmux <session>` against
// SOLIDTERM_TMUX_BIN:
//   "fake" — tests/tmux/fake-tmux.py playing scenario-basic.py (FAKE_TMUX_LOG names its command log):
//            tabs and splits built from the layouts, cell-exact geometry, seeded TermScreen contents,
//            every pane's TermScreen equal to an oracle fed the same bytes, tricky %output, a
//            re-split, a window added / renamed / closed, %pause → reseed, read-only input, and the
//            window close detaching (the fake logs detach-client);
//   "real" — a real tmux on a private socket prepared by tests/tmux/run-real-ui.sh: two windows, a
//            split, known text; tabs, geometry, seeded text and live output, then detach on close.
// Exit code 0 = PASS. Call right after the first window is created (before the event loop runs).
namespace TmuxSelfTest {
void start(QQuickWindow *window, const QString &mode);
}
