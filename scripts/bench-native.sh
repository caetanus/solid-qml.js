#!/bin/sh
# Native rows benchmark — the SAME component the web runs (examples/bench-rows.tsx). Prints, per N and
# with the list's scroll ON (overflow-y: auto) and OFF (visible), the first round of each operation as
# "<op>=<sync ms>/<to next tick ms>". swap/remove1 are clamped and valid for every N; for N >= 1000 they
# are the standard js-framework-benchmark 1<->998 swap and row-500 removal.
# Usage: scripts/bench-native.sh [outdir]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-${TMPDIR:-/tmp}/bench-native}
case "$OUT" in
    /*) ;;
    *) OUT="$PWD/$OUT" ;;
esac
LOADER="$ROOT/build-local/solid-qml-loader"
cd "$ROOT"
if ! ninja -C build-local -n | grep -q "no work to do"; then
    echo "build-local is stale — run: meson compile -C build-local" >&2
    exit 1
fi
echo "libsolidqml sha256: $(sha256sum build-local/libsolidqml.so.0.2.0 | cut -c1-16)"
rm -rf "$OUT"
mkdir -p "$OUT/gen"
node --import tsx scripts/gen.mjs examples/bench-rows.qml.tsx "$OUT/gen/App.generated.qml" >/dev/null
# gen.mjs OVERWRITES App.generated.css: the bench CSS must be appended after it.
cat examples/bench-rows.css >> "$OUT/gen/App.generated.css"
for scroll in auto visible; do
    for n in 250 500 1000 2000; do
        d="$OUT/$scroll-$n"
        cp -r "$OUT/gen" "$d"
        sed -i "s/overflow-y: auto/overflow-y: $scroll/" "$d/App.generated.css"
        sed -i "s/build(1000)/build($n)/g" "$d/BenchRows.qml"
        QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 timeout 600 "$LOADER" \
            --qml "$d/App.generated.qml" --css "$d/App.generated.css" --width 800 --height 900 \
            > "$d/run.log" 2>&1 &
        pid=$!
        truncated=0
        if ! timeout 590 sh -c "until grep -q 'BENCH done' '$d/run.log'; do sleep 0.3; done"; then
            truncated=1
        fi
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        printf '%-8s N=%-5s ' "$scroll" "$n"
        firstround=$(grep -oE 'BENCH (create1k|update10th|swap|remove1|append1k|clear) [0-9.]+ \+tick [0-9.]+' "$d/run.log" \
            | head -6)
        opcount=0
        if [ -n "$firstround" ]; then
            printf '%s\n' "$firstround" | awk '{printf "%s=%s/%s ", $2, $3, $5}'
            opcount=$(printf '%s\n' "$firstround" | grep -c 'BENCH ' || true)
        fi
        if [ "$opcount" -lt 6 ] || [ "$truncated" -eq 1 ]; then
            printf 'TRUNCATED'
        fi
        echo
    done
done
