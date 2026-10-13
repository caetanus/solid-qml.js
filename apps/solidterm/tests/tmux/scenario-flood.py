# Scenario for the flood + selection regression test (tmuxselftest.cpp mode "flood"): one pane that
# prints like `hexdump /dev/urandom` while the test makes four selections in a row. With a small
# hold cap the pane's held output overflows, SolidTerm asks tmux to pause it, and must continue and
# reseed it after the selection — the owner's "o tmux travou" was that never happening.

def setup(srv):
    srv.session, srv.session_name = 1, "flood"
    srv.window(1, "flood", 1)
    srv.pane(1, screen=[b"$ "], cursor=(2, 0), title="flood")
    srv.on(r"'step flood'", lambda srv, line: srv.flood(1, True))
    srv.on(r"'step stopflood'", lambda srv, line: srv.flood(1, False))
