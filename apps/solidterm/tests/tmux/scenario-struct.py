# Scenario for the tmux STRUCTURE test (phase 3; tmuxselftest.cpp mode "struct"): session "struct",
#   @1 "main": {%1, %2}     @2 "other": %3
# Splits, moves, swaps, zoom, resize, new/kill windows, break/join (hidden panes) are all answered
# by fake-tmux.py's tree model; the steps below only stage what tmux alone would emit.

def setup(srv):
    srv.session, srv.session_name = 1, "struct"
    srv.window(1, "main", ("{", [1, 2]))
    srv.window(2, "other", 3)
    srv.pane(1, screen=[b"one-marker"], cursor=(0, 1), title="one")
    srv.pane(2, screen=[b"two-marker"], cursor=(0, 1), title="two")
    srv.pane(3, screen=[b"three-marker"], cursor=(0, 1), title="three")

    # A new pane prints something we can follow when its view moves between windows.
    def mark(srv, line):
        for pid in srv.panes:
            if pid >= 4 and srv.panes[pid].title.startswith("new"):
                srv.output(pid, b"pane-%d-marker\r\n" % pid)
                srv.panes[pid].title = "marked %d" % pid
    srv.on(r"'step mark'", mark, once=False)

    # %1 enters copy mode (from a key binding or another client): tmux draws it, not the output.
    def mode(srv, line):
        p = srv.panes[1]
        p.modes["pane_in_mode"], p.modes["pane_mode"] = "1", "copy-mode"
        srv.notify("%pane-mode-changed %1")
    srv.on(r"'step mode'", mode)

    # A program inside tmux wraps a title (OSC 2) in tmux passthrough, cut across %output lines.
    def dcs(srv, line):
        data = b"\x1bPtmux;\x1b\x1b]2;from-passthrough\x07\x1b\\after-dcs\r\n"
        # separate reads (gap): the inner OSC title reaches libvterm in fragments
        srv.output(1, data, cut=[3, 9, data.index(b"pass"), data.index(b"\x07") + 2], gap=0.08)
    srv.on(r"'step dcs'", dcs)
