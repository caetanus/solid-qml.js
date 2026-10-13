#!/bin/bash
# SolidTerm performance bench — run on the REAL display (Wayland; the glyph material does not
# render on the offscreen/software backend). Every launched terminal quits by itself.
#
#   scripts/solidterm-bench.sh <solidterm-binary> <label> [cases...] [-- tilix|xterm]
#
# cases (default: mixed seq dense region latency wheel idle):
#   mixed    cat a deterministic 100 MB mixed file (ASCII + UTF-8 + SGR colours)
#   seq      seq 1 5000000
#   dense    vtebench-like dense cells: full-screen repaints, every cell its own fg/bg (50 MB)
#   region   vtebench-like scrolling in a DECSTBM region (50 MB)
#   latency  60 synthetic keys into bash (SOLIDTERM_LATENCYTEST) → per-stage latency
#   wheel    10k-line scrollback, 600 notches up + 600 down at 125 Hz (SOLIDTERM_WHEELTEST)
#   idle     10 s idle at a prompt-less sleep → CPU
# Throughput cases time themselves INSIDE the terminal (date before/after the command; the
# command returning = the terminal consumed its output). Results: JSON per case in
# $BENCH_RESULTS (default .scratch/bench/results/<label>/), summarised by solidterm-bench-report.py.
#
# Env: RUNS (default 5), MAX_LOAD (default 2.0 — waits for a quiet machine before each run),
#      COMPARE ("tilix" / "xterm" / "tilix xterm" — also run the throughput cases there),
#      GEOMETRY (COLSxROWS for the comparison terminals; default: what solidterm reported),
#      SKIP_SELF=1 (run only the COMPARE terminals, into the same results dir),
#      BENCH_SEQ (seq length, default 5000000).
# The output must be ON: with the panel powered off (DPMS) Wayland sends no frame callbacks and
# Qt renders nothing. Headless alternative on the same GPU (what perf-2026-10-13.md used):
#   WLR_BACKENDS=headless WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 sway -c <conf with
#   "output HEADLESS-1 resolution 1896x2020@60Hz scale 2"> → run this with WAYLAND_DISPLAY=<its socket>.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
bin=$(realpath "$1"); label=$2; shift 2
cases=("$@"); [ ${#cases[@]} -eq 0 ] && cases=(mixed seq dense region latency wheel idle)
runs=${RUNS:-5}
bdir=${BENCH_DIR:-$root/.scratch/bench}
res=${BENCH_RESULTS:-$bdir/results/$label}
mkdir -p "$bdir"; [ "$label" = "--runner-only" ] || mkdir -p "$res"
export TMPDIR=${TMPDIR:-$root/.scratch/tmp}; mkdir -p "$TMPDIR"

[ -f "$bdir/mixed.txt" ] || python3 "$here/solidterm-bench-gen.py" mixed "$bdir/mixed.txt" 100

# The shell every terminal runs. Throughput: time the command, report "t0/t1" (wall ns) + size.
cat > "$bdir/runner.sh" <<'EOF'
#!/bin/bash
set -u
sleep 1.5 # let the window map and size the grid (the pty starts 2x2 until the first layout)
read -r rows cols < <(stty size)
echo "size ${cols}x${rows}" > "$BENCH_OUT.tmp"
gen() { # $1 kind → cached file sized to this terminal
    local f="$BENCH_DIR/$1-${cols}x${rows}.txt"
    [ -f "$f" ] || python3 "$BENCH_GEN" "$1" "$f" "$rows" "$cols" 50
    echo "$f"
}
case "$BENCH_CASE" in
    mixed)  f="$BENCH_DIR/mixed.txt" ;;
    dense)  f=$(gen dense) ;;
    region) f=$(gen region) ;;
    seq)    f="" ;;
    idle)   sleep 1; echo "t0 $(date +%s%N)" >> "$BENCH_OUT.tmp"; sleep 10
            echo "t1 $(date +%s%N)" >> "$BENCH_OUT.tmp"; mv "$BENCH_OUT.tmp" "$BENCH_OUT"; exit 0 ;;
    latency) mv "$BENCH_OUT.tmp" "$BENCH_OUT"; PS1='$ ' exec bash --norc --noprofile -i ;;
    wheel)  seq 1 12000; echo "t0 $(date +%s%N)" >> "$BENCH_OUT.tmp"
            mv "$BENCH_OUT.tmp" "$BENCH_OUT"; exec sleep 120 ;;
esac
sleep 0.5 # settle (first frames, atlas warm-up) before timing
t0=$(date +%s%N)
if [ -n "$f" ]; then cat "$f"; else seq 1 "${BENCH_SEQ:-5000000}"; fi
t1=$(date +%s%N)
{ echo "t0 $t0"; echo "t1 $t1"; } >> "$BENCH_OUT.tmp"
mv "$BENCH_OUT.tmp" "$BENCH_OUT"
sleep 0.3
exit 0
EOF
chmod +x "$bdir/runner.sh"
export BENCH_GEN="$here/solidterm-bench-gen.py"
[ "$label" = "--runner-only" ] && exit 0 # just (re)write the runner (for manual/perf runs)

drive() { # term case [extra json]
    python3 "$here/solidterm-bench.py" "{\"term\":\"$1\",\"case\":\"$2\",\"runs\":$runs,\"dir\":\"$bdir\",\"out\":\"$res/$1-$2.json\",\"bin\":\"$bin\",\"max_load\":${MAX_LOAD:-2.0},\"geometry\":\"${GEOMETRY:-}\"${3:-}}"
}

echo "solidterm bench '$label' → $res (runs=$runs)" >&2
if [ -z "${SKIP_SELF:-}" ]; then # SKIP_SELF=1: only the COMPARE terminals (results dir reused)
    for c in "${cases[@]}"; do
        drive solidterm "$c"
    done
fi
if [ -n "${COMPARE:-}" ]; then
    if [ -z "${GEOMETRY:-}" ]; then
        GEOMETRY=$(python3 -c "import json,sys; r=json.load(open('$res/solidterm-${cases[0]}.json')); print(r[0]['timing'].get('size',''))")
    fi
    for t in $COMPARE; do
        for c in "${cases[@]}"; do
            case "$c" in mixed|seq|dense|region|idle) drive "$t" "$c" ;; esac
        done
    done
fi
python3 "$here/solidterm-bench-report.py" "$res"
