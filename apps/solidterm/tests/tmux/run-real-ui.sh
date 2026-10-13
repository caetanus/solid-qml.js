#!/bin/bash
# SolidTerm's tmux mode through the real UI (offscreen) against a REAL tmux, on a PRIVATE socket
# (-S under <scratch>, -f /dev/null): never the user's server, which is not touched; ours is killed
# at the end. Skipped (77) without tmux. After the app detached (window closed), the session must
# still be alive in tmux (docs/solidterm-tmux-control-mode.md 5.1).
# Usage: run-real-ui.sh <solidterm binary> <tmux binary or ""> <scratch dir> [real|real-input|real-struct]
# real-input (phase 2): keys typed in SolidTerm run in the tmux shell; mouse-echo.py (alternate
# screen, SGR mouse, bracketed paste — like vim/less) receives clicks, wheel and a paste.
set -u
app=${1:?solidterm} tmuxbin=${2:-} dir=${3:?scratch} mode=${4:-real}
here=$(cd "$(dirname "$0")" && pwd)
[[ -n $tmuxbin && -x $tmuxbin ]] || { echo "SKIP: no tmux"; exit 77; }
mkdir -p "$dir"
sock="$dir/real-ui-$mode.sock"
wrapper="$dir/tmux-real-ui-$mode.sh"
cat > "$wrapper" <<EOF
#!/bin/sh
exec env -i PATH=/usr/bin:/bin HOME=/ TERM=xterm-256color "$tmuxbin" -S "$sock" -f /dev/null "\$@"
EOF
chmod 700 "$wrapper"
T() { "$wrapper" "$@"; }
trap 'T kill-server >/dev/null 2>&1; rm -f "$sock" "$wrapper"' EXIT
T kill-server >/dev/null 2>&1
T start-server \; set -g default-shell /bin/sh \; set -g default-command "env -i PS1='$ ' TERM=tmux-256color /bin/sh" \
  \; new-session -d -s real -n main -x 100 -y 30 || exit 1
T split-window -h -t real:0
T new-window -d -t real -n second
T send-keys -t real:0.0 "printf 'left-%s\\n' marker" Enter
T send-keys -t real:0.1 "printf 'right-%s\\n' marker" Enter
T send-keys -t real:1.0 "printf 'second-%s\\n' marker" Enter
sleep 0.5

QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 SOLIDTERM_TMUX_BIN="$wrapper" SOLIDTERM_TMUXTEST=$mode \
  SOLIDTERM_TMUXTEST_MOUSEECHO="$here/mouse-echo.py" \
  timeout 90 "$app" --standalone --tmux real
rc=$?
if T has-session -t real 2>/dev/null; then
  echo "solidterm: tmuxtest ok: the session is still alive in tmux after the window closed"
else
  echo "solidterm: tmuxtest FAIL: the session did not survive the window closing"
  rc=1
fi
exit $rc
