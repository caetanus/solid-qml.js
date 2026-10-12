#!/bin/sh
# C23 <For> row-identity contract, app level: generate tests/c23/for-identity.qml.tsx with the
# transpiler, run it in the loader (offscreen) and require every `C23 PASS` line and no `C23 FAIL`.
# Usage: tests/c23/run.sh <solid-qml-loader> <outdir>
set -e
LOADER=$1
OUT=$2
EXPECTED=48 # PASS lines the fixture prints when every check runs
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Absolute = a POSIX path or a Windows drive path (meson passes D:\... there).
case "$OUT" in /*|[A-Za-z]:*) ;; *) OUT="$PWD/$OUT" ;; esac
rm -rf "$OUT"
mkdir -p "$OUT"
cd "$ROOT"
node --import tsx scripts/gen.mjs tests/c23/for-identity.qml.tsx "$OUT/App.generated.qml" >/dev/null
QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 timeout 60 "$LOADER" \
    --qml "$OUT/App.generated.qml" --css "$OUT/App.generated.css" --width 400 --height 400 \
    > "$OUT/run.log" 2>&1 &
pid=$!
timeout 55 sh -c "until grep -q 'C23 done' '$OUT/run.log'; do sleep 0.2; done" || true
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
grep 'C23 ' "$OUT/run.log" || true
status=0
if ! grep -q 'C23 done' "$OUT/run.log"; then
    echo "C23: the fixture did not finish" >&2
    status=1
fi
if grep -q 'C23 FAIL' "$OUT/run.log"; then
    status=1
fi
passes=$(grep -c 'C23 PASS' "$OUT/run.log" || true)
if [ "$passes" -ne "$EXPECTED" ]; then
    echo "C23: $passes PASS lines, expected $EXPECTED" >&2
    status=1
fi
if [ "$status" -ne 0 ]; then
    grep -iE 'error|warning|TypeError|ReferenceError' "$OUT/run.log" | head -20 >&2 || true
fi
exit $status
