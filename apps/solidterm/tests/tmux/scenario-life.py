# Scenario for the tmux LIFECYCLE tests (phase 4; tmuxselftest.cpp modes "life", "oldversion",
# "nobinary"; run-forward.sh): whatever session name SolidTerm asks for, one window "shell" with one
# pane that shows a marker, and a step that makes the server go away.
import sys


def setup(srv):
    name = "life"
    args = sys.argv[1:]
    for flag in ("-s", "-t"):
        if flag in args:
            name = args[args.index(flag) + 1].lstrip("=")
    srv.session, srv.session_name = 1, name
    srv.window(1, "shell", 1)
    srv.pane(1, screen=[b"life-marker " + name.encode()], cursor=(0, 1), title="shell")

    # The tmux server dies under us (kill-server, a crash): %exit with that reason, then EOF.
    def server_exit(srv, line):
        srv.notify("%exit server exited unexpectedly")
        sys.stdout.flush()
        sys.exit(0)
    srv.on(r"'step serverexit'", server_exit)
