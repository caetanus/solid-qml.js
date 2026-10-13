#!/bin/bash
# SHELL for SOLIDTERM_THREADSTRESS (the parser-thread race harness, apps/solidterm/main.cpp): an
# endless flood of the bench's mixed text (SGR colours, UTF-8, wide glyphs) interleaved with the
# escapes whose callbacks cross threads as news: OSC title, BEL, alt-screen enter/exit, cursor
# show/hide, mouse-mode on/off, a DECSTBM region, DSR/DA queries (libvterm answers through the
# output queue). Needs $BENCH_DIR/mixed.txt (scripts/solidterm-bench.sh generates it).
src=${BENCH_DIR:?}/mixed.txt
i=0
while :; do
    head -c 2000000 "$src" | tail -c 1000000
    printf '\e]0;stress %d\a\a\e[?1049h\e[2J\e[5;10Halt\e[?1049l\e[?25l\e[?25h\e[?1000h\e[?1000l' "$i"
    printf '\e[3;20r\e[20H\n\n\n\e[r\e[6n\e[c'
    i=$((i + 1))
done
