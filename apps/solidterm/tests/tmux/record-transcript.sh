#!/bin/bash
# Records real-3.7c-session.txt: a control-mode session against a REAL tmux on a private socket
# (never the user's server), with a scrubbed environment (env -i, /bin/sh, cwd /) so the transcript
# holds no personal data. After re-recording, regenerate and REVIEW the expected event log:
#   build/test-solidterm-tmux-protocol --print apps/solidterm/tests/tmux/real-3.7c-session.txt \
#       > apps/solidterm/tests/tmux/real-3.7c-session.expected
# Usage: record-transcript.sh <scratch-dir>
set -euo pipefail
dir=${1:?scratch dir}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$dir"
sock="$dir/record.sock"
out="$here/real-3.7c-session.txt"
T() { env -i PATH=/usr/bin:/bin HOME=/ TERM=xterm-256color tmux -S "$sock" -f /dev/null "$@"; }
trap 'T kill-server 2>/dev/null || true; rm -f "$sock"' EXIT

FMT='#{pane_width} #{pane_height} #{history_size} #{cursor_x} #{cursor_y} #{cursor_flag} #{alternate_on} #{alternate_saved_x} #{alternate_saved_y} #{scroll_region_upper} #{scroll_region_lower} #{keypad_cursor_flag} #{keypad_flag} #{insert_flag} #{wrap_flag} #{origin_flag} #{mouse_standard_flag} #{mouse_button_flag} #{mouse_any_flag} #{mouse_sgr_flag} #{mouse_utf8_flag} #{bracket_paste_flag} #{cursor_shape} #{cursor_blinking} #{pane_in_mode} #{pane_title}'

cd /
T start-server \; set -g default-shell /bin/sh \; set -g default-command "env -i PS1='$ ' TERM=tmux-256color /bin/sh" \
  \; new-session -d -s rec -n main -x 80 -y 24
T set -g automatic-rename off
T split-window -h -t rec:0
T split-window -v -t rec:0.1
T new-window -t rec -n logs
T select-window -t rec:0
T select-pane -t rec:0.0
T select-pane -t rec:0.0 -T pane0
T send-keys -t rec:0.0 "printf 'one\\ntwo\\n'" Enter
sleep 0.5

# Our commands, one per line; "#n <line>" = a line of n commands (for the ##send directives).
cmds=(
  "1 refresh-client -C 100x30"
  "1 refresh-client -f pause-after=5"
  "1 list-windows -F '#{window_id} #{window_index} #{window_active} #{window_raw_flags} #{window_layout} #{window_visible_layout} #{window_name}'"
  "1 list-panes -s -F '#{pane_id} #{window_id} #{pane_active} #{pane_width} #{pane_height} #{pane_current_path}'"
  "1 display-message -p -t '%0' '$FMT'"
  "2 capture-pane -p -t '%0' -e -C -J -S -50 -E '-' ; capture-pane -p -t '%0' -P -C"
  "3 display-message -p 'a' ; kill-pane -t '%99' ; display-message -p 'b'"
  "1 bogus-cmd"
  "1 display-message -p 'it'\"'\"'s \$HOME'"
)
{
  for c in "${cmds[@]}"; do printf '%s\n' "${c#* }"; done
  sleep 0.6
  T send-keys -t rec:0.0 "printf 'a\\\\b\\tc\\001 caf\\303\\251 \\033[31mred\\033[0m\\n'" Enter
  sleep 0.4
  T split-window -v -t rec:0.0
  sleep 0.3
  T new-window -t rec -n 'tmp win'
  sleep 0.3
  T rename-window -t rec:2 'renamed one'
  sleep 0.2
  T kill-window -t rec:2
  sleep 0.3
  T resize-pane -Z -t '%0'
  sleep 0.3
  printf 'detach-client\n'
  sleep 0.3
} | T -C attach -t rec > "$dir/raw.txt"

{
  echo "## RECORDED from a real tmux 3.7c (record-transcript.sh): private socket, env -i, /bin/sh."
  echo "## Shows: the flags-0 argv block, %extended-output after pause-after, a list/seed exchange,"
  echo "## a group whose 2nd command fails (the 3rd never answers), a parse error, a quoted ', a"
  echo "## printf with \\\\, TAB, \\001 and UTF-8, a split (%layout-change), a window added, renamed and"
  echo "## killed (3.7c reports OUR killed window as %unlinked-window-close), a zoom (-Z), %exit."
  sent=0
  while IFS= read -r line; do
    printf '%s\n' "$line"
    if [[ $sent == 0 && $line == %session-changed* ]]; then
      for c in "${cmds[@]}"; do printf '##send %s\n' "$c"; done
      echo "##send 1 detach-client"
      sent=1
    fi
  done < "$dir/raw.txt"
} > "$out"
echo "wrote $out"
