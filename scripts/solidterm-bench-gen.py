#!/usr/bin/env python3
"""Deterministic inputs for scripts/solidterm-bench.sh.

  mixed <out> [MB]          ~MB of mixed text: ASCII words, some UTF-8 (accents, dashes, box
                            drawing, a few CJK wide chars), SGR colours (16/256/truecolor, bold,
                            underline). Same bytes every run (fixed seed).
  dense <out> <rows> <cols> [MB]
                            vtebench "dense_cells"-like: repeatedly home the cursor and repaint the
                            whole rows x cols screen, every cell with its own 256-colour fg+bg and
                            a different character.
  region <out> <rows> <cols> [MB]
                            vtebench "scrolling_in_region"-like: DECSTBM margins (1 row kept at top
                            and bottom), then full-width lines written at the bottom margin, so every
                            line scrolls only the region.
"""
import random
import sys

def mixed(out, mb=100):
    rnd = random.Random(1234)
    words = ("lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor "
             "incididunt ut labore et dolore magna aliqua terminal render glyph atlas cursor").split()
    utf8 = ["ação", "coração", "é", "über", "naïve", "—", "…", "→", "│", "─", "┼", "█", "λ", "π", "日本", "語"]
    target = mb * 1024 * 1024
    n = 0
    with open(out, "wb") as f:
        buf = []
        size = 0
        while n < target:
            parts = []
            for _ in range(rnd.randint(4, 16)):
                r = rnd.random()
                if r < 0.10:
                    w = rnd.choice(utf8)
                elif r < 0.18:
                    w = "\x1b[%dm%s\x1b[0m" % (rnd.choice([31, 32, 33, 34, 35, 36, 91, 92, 93, 94]), rnd.choice(words))
                elif r < 0.22:
                    w = "\x1b[38;5;%dm%s\x1b[0m" % (rnd.randrange(256), rnd.choice(words))
                elif r < 0.24:
                    w = "\x1b[38;2;%d;%d;%dm%s\x1b[0m" % (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.choice(words))
                elif r < 0.26:
                    w = "\x1b[1m%s\x1b[0m" % rnd.choice(words)
                elif r < 0.27:
                    w = "\x1b[4m%s\x1b[0m" % rnd.choice(words)
                else:
                    w = rnd.choice(words)
                parts.append(w)
            line = (" ".join(parts) + "\n").encode()
            buf.append(line)
            size += len(line)
            n += len(line)
            if size > 1 << 20:
                f.write(b"".join(buf))
                buf, size = [], 0
        f.write(b"".join(buf))

def dense(out, rows, cols, mb=50):
    rnd = random.Random(42)
    chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    target = mb * 1024 * 1024
    n = 0
    frame = 0
    with open(out, "wb") as f:
        while n < target:
            cells = ["\x1b[H"]
            for r in range(rows):
                for c in range(cols):
                    k = (r * cols + c + frame) & 255
                    cells.append("\x1b[38;5;%d;48;5;%dm%s" % (k, 255 - k, chars[(r + c + frame) % len(chars)]))
            cells.append("\x1b[0m")
            data = "".join(cells).encode()
            f.write(data)
            n += len(data)
            frame += 1

def region(out, rows, cols, mb=50):
    target = mb * 1024 * 1024
    n = 0
    i = 0
    with open(out, "wb") as f:
        head = ("\x1b[2;%dr\x1b[%d;1H" % (rows - 1, rows - 1)).encode()
        f.write(head)
        while n < target:
            line = ("%08d " % i + "y" * (cols - 9))[:cols]
            data = ("\x1b[%d;1H\n" % (rows - 1) + line).encode()
            f.write(data)
            n += len(data)
            i += 1
        f.write(b"\x1b[r\x1b[%d;1H\n" % rows)

if __name__ == "__main__":
    kind = sys.argv[1]
    if kind == "mixed":
        mixed(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 100)
    elif kind == "dense":
        dense(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else 50)
    elif kind == "region":
        region(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else 50)
    else:
        sys.exit(__doc__)
