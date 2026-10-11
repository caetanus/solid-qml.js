#!/bin/sh
# C23 <For> row-identity contract on a VIRTUALIZED list, app level: generate tests/c23/for-virtual.qml.tsx,
# then for each mode (keyed: the generated `key: "id"`; ref: that line deleted = reference identity, the
# default) and each operation run the loader once (offscreen, SQ_PHASE_STATS=1) and count the rows that
# entered the window (repeater.beginCreate + repeater.rebind). Relative to the mount-only run (W rows):
#   replace row 3 by a new object with the same id: ref +1, keyed +0
#   refetch (every element a new object):            ref +W, keyed +0
#   swap 1<->998 (row 1 leaves the window):          +1 both;   remove 500 / mutate in place / clear: +0 both
# Usage: tests/c23/run-virtual.sh <solid-qml-loader> <outdir>
set -e
LOADER=$1
OUT=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
case "$LOADER" in /*) ;; *) LOADER="$PWD/$LOADER" ;; esac
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
rm -rf "$OUT"
mkdir -p "$OUT/gen"
cd "$ROOT"
node --import tsx scripts/gen.mjs tests/c23/for-virtual.qml.tsx "$OUT/gen/App.generated.qml" >/dev/null
cat tests/c23/for-virtual.css >> "$OUT/gen/App.generated.css"
grep -q 'virtualize: true' "$OUT/gen/ForVirtual.qml" || { echo "C23V: the row is not virtualized" >&2; exit 1; }
grep -q 'key: "id"' "$OUT/gen/ForVirtual.qml" || { echo "C23V: no key line" >&2; exit 1; }

created() { # <mode> <op> -> rows that entered the window
    d="$OUT/$1-$2"
    cp -r "$OUT/gen" "$d"
    sed -i "s/var op = \"none\"/var op = \"$2\"/" "$d/ForVirtual.qml"
    [ "$1" = ref ] && sed -i '/^ *key: "id"$/d' "$d/ForVirtual.qml"
    SQ_PHASE_STATS=1 QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 timeout 60 "$LOADER" \
        --qml "$d/App.generated.qml" --css "$d/App.generated.css" --width 400 --height 400 > "$d/run.log" 2>&1 || true
    if ! grep -q "C23V done $2" "$d/run.log" || grep -qE 'TypeError|ReferenceError' "$d/run.log"; then
        echo "C23V: $1 $2 did not run cleanly" >&2
        grep -iE 'error|warning' "$d/run.log" | head -5 >&2
        echo -1
        return
    fi
    awk '/^\[phase\] repeater\.(beginCreate|rebind) /{for(i=1;i<=NF;i++) if ($i ~ /^x[0-9]+$/) s += substr($i, 2)} END{print s+0}' "$d/run.log"
}

status=0
for mode in ref keyed; do
    w=$(created $mode none)
    for op in replace refetch swap remove mutate clear; do
        c=$(created $mode $op)
        case "$mode:$op" in
            ref:replace|ref:swap|keyed:swap) want=1 ;;
            ref:refetch) want=$w ;;
            *) want=0 ;;
        esac
        got=$((c - w))
        if [ "$c" -ge 0 ] && [ "$w" -gt 0 ] && [ "$got" -eq "$want" ]; then r=PASS; else r=FAIL; status=1; fi
        echo "C23V $r $mode $op window=$w entered=+$got want=+$want"
    done
done
exit $status
