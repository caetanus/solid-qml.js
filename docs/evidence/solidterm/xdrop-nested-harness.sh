#!/bin/bash
# Real pointer drag of a SolidTerm pane between two windows, in a NESTED HEADLESS sway (isolated from
# the desktop): a virtual pointer (zwlr_virtual_pointer_v1, xdrop-vptr.c) presses on window B's pane
# header and moves onto window A, so the drag is a genuine wl_data_device drag handled by the
# compositor. MODE=split (default: A's pane, right zone) | tab (A's chrome strip) | outside (the
# wallpaper gap: detach). Writes xdrop-<mode>-*.png next to this script; prints shell PIDs before/after.
# Needs: a sway with ext-background-effect (the owner's build), grim, swaybg, wayland-scanner,
# a build dir with solidterm (BUILD=, default build-st).
set -u
MODE=${MODE:-split}
TAG=$([ $MODE = split ] && echo nested || echo nested-$MODE)
EV=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$EV/../../.." && pwd)
N=$W/.scratch/xdrop-nest
SWAY=${SWAY:-$HOME/.local/libexec/sway-dev/sway}
WLR_PROTO=${WLR_PROTO:-$HOME/lab/sway/subprojects/wlroots/protocol}
mkdir -p $N/cfg/solidterm
cat > $N/sway.conf <<'CONF'
output HEADLESS-1 resolution 1600x900 position 0 0 bg /usr/share/backgrounds/bentwood.jpg fill
default_border none
gaps inner 8
focus_follows_mouse no
CONF
echo '{ "blur": 60, "opacity": 78, "showHeader": 1, "fontSize": 13 }' > $N/cfg/solidterm/config.json
if [ ! -x $N/vptr ]; then
  X=$WLR_PROTO/wlr-virtual-pointer-unstable-v1.xml
  wayland-scanner client-header $X $N/vp.h && wayland-scanner private-code $X $N/vp.c
  cc -O1 -I$N -o $N/vptr $EV/xdrop-vptr.c $N/vp.c $(pkg-config --cflags --libs wayland-client) || exit 1
fi
export XDG_RUNTIME_DIR=/run/user/1000
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WAYLAND_DISPLAY= $SWAY -c $N/sway.conf > $N/sway.log 2>&1 &
SWAYPID=$!
for i in $(seq 50); do
  NEW=$(cd $XDG_RUNTIME_DIR && ls -t | grep -E '^wayland-[0-9]+$' | head -1)
  [ "$NEW" != "wayland-1" ] && [ -S $XDG_RUNTIME_DIR/sway-ipc.1000.$SWAYPID.sock ] && break; sleep 0.2
done
SOCK=$(ls $XDG_RUNTIME_DIR/sway-ipc.1000.$SWAYPID.sock)
echo "nested display=$NEW ipc=$SOCK"
export WAYLAND_DISPLAY=$NEW SWAYSOCK=$SOCK
rm -f $N/ptr.fifo; mkfifo $N/ptr.fifo
$N/vptr < $N/ptr.fifo 2>$N/vptr.log &
VP=$!
exec 7>$N/ptr.fifo
ptr() { echo "$@" >&7; }
cleanup() { exec 7>&-; kill $APP 2>/dev/null; sleep 0.5; kill $VP $SWAYPID 2>/dev/null; wait 2>/dev/null; rm -f $N/ptr.fifo; }
trap cleanup EXIT
sleep 1.5
cd $N
QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 SHELL=/bin/bash XDG_CONFIG_HOME=$N/cfg \
  SOLIDTERM_AUTOSPLIT=1 SOLIDTERM_DETACH=1 SOLIDTERM_DRAGLOG=1 SOLIDTERM_BLURINFO=1 \
  $W/${BUILD:-build-st}/solidterm --standalone > $N/app.log 2>&1 &
APP=$!
sleep 5
swaymsg -t get_tree | python3 -c '
import json,sys
def walk(n):
    if n.get("pid") and n.get("type")=="con": print(n["rect"]["x"],n["rect"]["y"],n["rect"]["width"],n["rect"]["height"])
    for k in n.get("nodes",[])+n.get("floating_nodes",[]): walk(k)
walk(json.load(sys.stdin))' > $N/wins.txt
cat $N/wins.txt
pids_before=$(ps -o pid= --ppid $APP | sort | tr '\n' ' ')
echo "shells before: $pids_before"
grim $EV/xdrop-$TAG-0-before.png
# Source: the right window (the detached pane), its pane header; target: right half of the left window.
read LX LY LW LH < <(sed -n 1p $N/wins.txt)
read RX RY RW RH < <(sed -n 2p $N/wins.txt)
SX=$((RX + 120)); SY=$((RY + 58))
TX=$((LX + LW * 3 / 4)); TY=$((LY + LH / 2))
[ $MODE = tab ] && { TX=$((LX + LW / 2)); TY=$((LY + 25)); }      # the window's chrome strip
[ $MODE = outside ] && { TX=$((LX + LW / 2)); TY=$((LY + LH + 4)); } # the wallpaper gap below

ptr abs $SX $SY; sleep 0.3
ptr down; sleep 0.2
for f in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  ptr abs $((SX + (TX - SX) * f / 20)) $((SY + (TY - SY) * f / 20)); sleep 0.08
done
sleep 0.8
grim $EV/xdrop-$TAG-1-dragging.png
ptr up
sleep 2
grim $EV/xdrop-$TAG-2-after.png
swaymsg -t get_tree | python3 -c '
import json,sys
c=[0]
def walk(n):
    if n.get("pid") and n.get("type")=="con": c[0]+=1
    for k in n.get("nodes",[])+n.get("floating_nodes",[]): walk(k)
walk(json.load(sys.stdin)); print("windows after:", c[0])'
pids_after=$(ps -o pid= --ppid $APP | sort | tr '\n' ' ')
echo "shells after:  $pids_after"
[ "$pids_before" = "$pids_after" ] && echo "SAME shell PIDs (none restarted)" || echo "PID SET CHANGED"
grep -v propagateSizeHints $N/app.log | tail -15
