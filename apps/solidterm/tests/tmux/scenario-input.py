# Scenario for the tmux INPUT test (phase 2; tmuxselftest.cpp mode "input"): session "keys", one
# window {%1, %2}. %1 has bracketed paste and SGR mouse reporting on (as vim/less would set them), so
# the seed turns them on in SolidTerm's parser too; %2 is a plain shell.

def setup(srv):
    srv.session, srv.session_name = 1, "keys"
    srv.window(1, "input", ("{", [1, 2]))
    srv.pane(1, screen=[b"mouse+paste app"], cursor=(0, 1), title="app",
             modes={"bracket_paste_flag": "1", "mouse_standard_flag": "1", "mouse_sgr_flag": "1"})
    srv.pane(2, screen=[b"$ "], cursor=(2, 0), title="shell")

    # Queries in a pane's output: tmux answers them itself; SolidTerm must not answer too.
    def queries(srv, line):
        srv.output(2, b"\x1b[c\x1b[6n\x1b[>c\x1b[?2004$p\x1b]11;?\x07queries-done\r\n")
    srv.on(r"'step query'", queries)

    # The program in %1 turns focus reporting on (DECSET 1004).
    def focus(srv, line):
        srv.output(1, b"\x1b[?1004hfocus-on\r\n")
    srv.on(r"'step focus'", focus)
