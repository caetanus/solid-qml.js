#!/usr/bin/env python3
# A vim/less-style program for SolidTerm's real-tmux input test: alternate screen, SGR mouse
# reporting (1000 + 1006) and bracketed paste (2004) on; prints every read as repr() on its own line
# until it reads a 'q' alone.
import os, sys, tty
tty.setraw(0)
os.write(1, b"\x1b[?1049h\x1b[H\x1b[?1000h\x1b[?1006h\x1b[?2004hREADY\r\n")
try:
    while True:
        data = os.read(0, 4096)
        os.write(1, (repr(data) + "\r\n").encode())
        if data == b"q":
            break
finally:
    os.write(1, b"\x1b[?2004l\x1b[?1006l\x1b[?1000l\x1b[?1049l")
