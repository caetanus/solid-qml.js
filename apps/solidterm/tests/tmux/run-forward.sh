#!/bin/bash
# `--tmux` through the single-instance socket, and reattach-on-start (docs/solidterm-tmux-control-mode.md
# 5.2, 6.7, decision 3), with the FAKE tmux (tests/tmux/fake-tmux.py) and two real processes:
#  1. an instance starts (no arguments): it reattaches the sessions SolidTerm created that nothing is
#     attached to — "mine" (@solidterm 1, detached), not "notmine" nor "busy" (attached);
#  2. `solidterm --tmux fwd` forwards to it and exits at once; the instance opens a tmux window;
#  3. the same again, and `--tmux-attach mine`: the existing windows are raised, nothing re-attaches.
# Private XDG_RUNTIME_DIR (socket) and XDG_CONFIG_HOME (tmuxBinary = the fake); no SOLIDTERM_*
# variable (it would make the run standalone); offscreen. Usage: run-forward.sh <solidterm> <dir>
set -u
app=${1:?solidterm} dir=${2:?scratch}
here=$(cd "$(dirname "$0")" && pwd)
rm -rf "$dir"; mkdir -p "$dir/rt" "$dir/cfg/solidterm"; chmod 700 "$dir/rt"
printf '{ "tmuxBinary": "%s", "tmuxReattach": 1 }\n' "$here/fake-tmux.py" > "$dir/cfg/solidterm/config.json"
export XDG_RUNTIME_DIR="$dir/rt" XDG_CONFIG_HOME="$dir/cfg" QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1
export FAKE_TMUX_SCENARIO="$here/scenario-life.py" FAKE_TMUX_LOG="$dir/fake.log" FAKE_TMUX_SESSIONS="mine:1:0;notmine::0;busy:1:1"
unset WAYLAND_DISPLAY DISPLAY TMUX
for v in $(env | sed -n 's/^\(SOLIDTERM_[A-Z_]*\)=.*/\1/p'); do unset "$v"; done
: > "$FAKE_TMUX_LOG"
fail=0
ok() { echo "solidterm: forwardtest ok: $*"; }
bad() { echo "solidterm: forwardtest FAIL: $*"; fail=1; }
waitfor() { # <seconds> <grep pattern>
  for _ in $(seq $(( $1 * 10 ))); do grep -q -- "$2" "$FAKE_TMUX_LOG" && return 0; sleep 0.1; done; return 1; }
count() { grep -c -- "$1" "$FAKE_TMUX_LOG"; }

"$app" > "$dir/instance.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null' EXIT
waitfor 15 '# argv: -C attach-session -t =mine' && ok "on start, the instance reattached its own detached session (mine)" \
  || bad "no reattach of mine"
sleep 0.5
[[ $(count '=notmine') == 0 && $(count '=busy') == 0 ]] && ok "…and not a session it did not create, nor an attached one" \
  || bad "attached notmine/busy"

start=$(date +%s%N)
timeout 10 "$app" --tmux fwd > "$dir/fwd1.log" 2>&1; rc=$?
ms=$(( ($(date +%s%N) - start) / 1000000 ))
[[ $rc == 0 && $ms -lt 3000 ]] && ok "\`solidterm --tmux fwd\` forwarded and exited (rc $rc, ${ms} ms)" || bad "forwarding: rc $rc in ${ms} ms"
waitfor 10 '# argv: -C new-session -A -s fwd' && ok "the instance opened a tmux window on fwd" || bad "no window on fwd"
timeout 10 "$app" --tmux fwd > /dev/null 2>&1
timeout 10 "$app" --tmux-attach mine > /dev/null 2>&1
sleep 1.5
[[ $(count '# argv: -C new-session -A -s fwd') == 1 && $(count '# argv: -C attach-session -t =mine') == 1 ]] \
  && ok "asking again for a session that has a window raises it (no second attach)" || bad "a second attach happened"
kill $pid; wait $pid 2>/dev/null; trap - EXIT
if (( fail )); then cat "$dir/instance.log" | tail -20; echo "solidterm: forwardtest FAIL"; exit 1; fi
echo "solidterm: forwardtest PASS"
