#!/bin/bash
# Fixed screen for before/after render-parity grabs (used as SHELL with SOLIDTERM_PLAINSHOT):
# every attribute the renderer handles — 16/256/truecolour fg+bg, reverse, bold, italic,
# underline, box drawing + blocks (procedural glyphs), wide CJK, emoji (colour glyph), combining
# marks, Nerd-Font PUA — then hides the cursor (no blink phase in the grab) and waits.
printf '\e[?25l\e[2J\e[H'
for i in $(seq 0 15); do printf '\e[38;5;%dm fg%-3d\e[0m' $i $i; done; echo
for i in $(seq 0 15); do printf '\e[48;5;%dm bg%-3d\e[0m' $i $i; done; echo
for i in $(seq 16 6 255); do printf '\e[48;5;%dm \e[38;5;%dmx\e[0m' $i $((255 - i + 16)); done; echo
for i in $(seq 0 8 255); do printf '\e[38;2;%d;%d;%dm#\e[0m' $i $((255 - i)) $((i / 2)); done; echo
printf 'plain \e[1mbold\e[0m \e[3mitalic\e[0m \e[1;3mbolditalic\e[0m \e[4munderline\e[0m \e[7mreverse\e[0m \e[1;4;7mall\e[0m\n'
printf '┌──┬──┐ ╔══╦══╗ ┏━━┳━━┓ █▓▒░ ▀▄▌▐ ⠿⡇ \n│ab│cd│ ║ef║gh║ ┃ij┃kl┃ ▁▂▃▄▅▆▇█\n└──┴──┘ ╚══╩══╝ ┗━━┻━━┛ \n'
printf 'wide: 日本語 中文 한국어  emoji: 🚀🎉✅  combining: e\xcc\x81 a\xcc\x83 n\xcc\x83  pua: \xee\x82\xb0\xee\x82\xb1\n'
printf 'ação coração über naïve — … → λ π\n'
for r in $(seq 1 30); do
    for c in $(seq 1 100); do printf '%s' "$(( (r * 7 + c * 3) % 10 ))"; done; echo
done
exec sleep 30
