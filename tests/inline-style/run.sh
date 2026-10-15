#!/bin/sh
# Inline `style`, app level: transpile examples/inline-style.qml.tsx, grab one offscreen frame in the
# loader and sample it — eight swatches coloured only by their inline style (one shared class), bars
# sized by numeric inline widths (px, relaid out), an inline background beating a class.
# Usage: tests/inline-style/run.sh <solid-qml-loader> <outdir>   (exit 77 = skipped: no python3 PIL)
set -e
LOADER=$1
OUT=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
python3 -c 'import PIL' 2>/dev/null || { echo "inline-style: python3 PIL missing, skipped"; exit 77; }
rm -rf "$OUT"
mkdir -p "$OUT"
cd "$ROOT"
node --import tsx scripts/gen.mjs examples/inline-style.qml.tsx "$OUT/App.generated.qml" >/dev/null
QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 timeout 60 "$LOADER" \
    --qml "$OUT/App.generated.qml" --css "$OUT/App.generated.css" --grab "$OUT/grab.png" > "$OUT/run.log" 2>&1
exec python3 "$ROOT/tests/inline-style/check.py" "$OUT/grab.png"
