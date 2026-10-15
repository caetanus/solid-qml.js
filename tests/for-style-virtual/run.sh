#!/bin/sh
# A <For> of 2000 rows coloured by an inline style object (tests/for-style-virtual/for-style-virtual.tsx),
# app level: the row must be certified (`virtualize: true`), the repeater must materialize only the
# window, rows recycled by scrolling must show their own colour, and a component signal read in the
# style must update the visible rows live (and the parked ones, seen after scrolling back).
# Runs the generated app twice offscreen — virtualized, and eager (the `virtualize` line deleted) — with a
# probe (probe.qml.in) that scrolls, calls the signal setter and grabs a frame per step; check.py samples
# the frames and requires the two runs to be pixel-identical.
# Usage: tests/for-style-virtual/run.sh <solid-qml-loader> <outdir>   (exit 77 = skipped: no python3 PIL)
set -e
LOADER=$1
OUT=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
case "$LOADER" in /*) ;; *) LOADER="$PWD/$LOADER" ;; esac
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
python3 -c 'import PIL' 2>/dev/null || { echo "for-style-virtual: python3 PIL missing, skipped"; exit 77; }
rm -rf "$OUT"
mkdir -p "$OUT/gen"
cd "$ROOT"
node --import tsx scripts/gen.mjs tests/for-style-virtual/for-style-virtual.qml.tsx "$OUT/gen/App.generated.qml" >/dev/null
grep -q 'virtualize: true' "$OUT/gen/ForStyleVirtual.qml" || { echo "for-style-virtual: the row is not virtualized" >&2; exit 1; }

run() { # <mode>: virtual | eager
    d="$OUT/$1"
    cp -r "$OUT/gen" "$d"
    [ "$1" = eager ] && sed -i '/^ *virtualize: true$/d' "$d/ForStyleVirtual.qml"
    sed -i 's/^\( *\)ForStyleVirtual {$/&\n\1    id: __probe/' "$d/App.generated.qml"
    sed -i '$d' "$d/App.generated.qml" # the Window's closing brace: the probe closes it
    sed "s|@OUT@|$d|" tests/for-style-virtual/probe.qml.in >> "$d/App.generated.qml"
    SQ_PHASE_STATS=1 QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 timeout 90 "$LOADER" \
        --qml "$d/App.generated.qml" --css "$d/App.generated.css" --width 400 --height 400 > "$d/run.log" 2>&1 || true
    if ! grep -q "FSV done" "$d/run.log" || grep -qE 'TypeError|ReferenceError' "$d/run.log"; then
        echo "for-style-virtual: the $1 run did not finish cleanly" >&2
        grep -iE 'error|warning' "$d/run.log" | head -5 >&2
        exit 1
    fi
}
run virtual
run eager
exec python3 tests/for-style-virtual/check.py "$OUT/virtual" "$OUT/eager"
