# Scenario for SolidTerm's tmux tests (fake-tmux.py): session "work" with two windows —
#   @1 "editor": {%1, [%2, %3]}   (%3 is on the alternate screen, like vim)
#   @2 "logs":   %4
# The test drives it with `display-message -p 'step N'`; see tmux_client_test.cpp / tmuxselftest.cpp.

def setup(srv):
    srv.session, srv.session_name = 1, "work"
    srv.window(1, "editor", ("{", [1, ("[", [2, 3])]))
    srv.window(2, "logs", 4)
    srv.pane(1, history=[b"hist-%02d" % i for i in range(30)],
             screen=[b"screen-1-top", b"\x1b[1;31mred bold\x1b[0m plain", b"$ "], cursor=(2, 2), title="one")
    srv.pane(2, screen=[b"two: caf\xc3\xa9 back\\slash"], cursor=(0, 1), title="two")
    srv.pane(3, alt=True, history=[b"h3-old"], saved=[b"normal-under-alt"],
             screen=[b"ALT-TOP", b"", b"ALT-3"], cursor=(5, 2), title="vim")
    srv.pane(4, screen=[b"logs pane"], cursor=(0, 1), title="")  # no title: the tab shows the window name

    def grid_line(pid, text):
        # tmux parses output into its grid before queueing it for us: the next capture holds it.
        p = srv.panes[pid]
        rows = [r for r in p.screen]
        rows.insert(p.cursor[1], text)
        p.screen = rows
        p.cursor = (p.cursor[0], p.cursor[1] + 1)

    # Output racing the seed of %1: between the probe and the capture group, and right before the
    # group's first %begin. Both are already in the capture: SolidTerm must drop them (4.3).
    def gap(srv, line):
        grid_line(1, b"probe-gap")
        srv.output(1, b"probe-gap\r\n")
    srv.on(r"^display-message -p -t '%1' '[^']*'$", gap)

    def raced(srv, line):
        grid_line(1, b"raced")
        srv.output(1, b"raced\r\n")
    srv.on(r"capture-pane -p -t '%1' -e -C -J", raced, before=True)

    # Output after the group's last block is new: kept.
    def after_seed(srv, line):
        srv.output(1, b"after-seed\r\n")
    srv.on(r"capture-pane -p -t '%1' -e -C -J", after_seed)

    # step 1: tricky bytes on %2, cut inside UTF-8 and inside an escape sequence.
    def step1(srv, line):
        data = b"\r\nC:\\Users\\x \x01\x07 caf\xc3\xa9 \xf0\x9f\x9a\x80 \x1b[38;5;196mred\x1b[0m end\r\n"
        cut = [data.index(b"\xc3") + 1, data.index(b"\x9a"), data.index(b"38;5")]
        srv.output(2, data, cut=cut)
    srv.on(r"'step 1'", step1)

    # step 2: @1 re-split — %3 gone, a new %5 under %1 — then %5 prints.
    def step2(srv, line):
        w = srv.find_window(1)
        w.tree = ("{", [("[", [1, 5]), 2])
        srv.pane(5, screen=[], cursor=(0, 0), title="five")
        srv.panes.pop(3, None)
        srv.layout_change(w)
        srv.output(5, b"new pane five\r\n")
    srv.on(r"'step 2'", step2)

    # step 3: rename @2, add @3 (3.7c: %window-add carries no layout), then @2 is killed (3.7c
    # reports our own killed window as %unlinked-window-close).
    def step3(srv, line):
        srv.notify("%window-renamed @2 renamed logs")
        srv.window(3, "third", 6)
        srv.pane(6, screen=[], cursor=(0, 0), title="six")
        srv.notify("%session-window-changed $1 @3")
        srv.notify("%window-add @3")
        srv.output(6, b"third window\r\n")
        srv.windows = [w for w in srv.windows if w.id != 2]
        srv.panes.pop(4, None)
        srv.notify("%session-window-changed $1 @1")
        srv.active_window = 1
        srv.notify("%unlinked-window-close @2")
    srv.on(r"'step 3'", step3)

    # step 4: tmux pauses %1 (pause-after); its screen moved on meanwhile.
    def step4(srv, line):
        p = srv.panes[1]
        p.paused = True
        p.screen = [b"after-pause-1", b"after-pause-2"]
        p.cursor = (0, 2)
        srv.notify("%pause %1")
    srv.on(r"'step 4'", step4)

    # A client resize: every window gets a new layout, then %1 prints for its NEW size.
    def resized(srv, line):
        srv.output(1, b"after-resize\r\n")
    srv.on(r"^refresh-client -C 100x30$", resized)
