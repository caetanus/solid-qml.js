"""Checks for tests/for-style-virtual/run.sh: <virtual dir> <eager dir>.

Rows are 20px tall in a 400x400 window, row i coloured palette[i % 7] by its inline style, with a 20px
swatch at the left coloured by the `tone` signal (#ffffff, then #00ff00 after retone()). Frames: top
(contentY 0), mid (row 1000 at the top), mid-tone (after the signal), top-tone (scrolled back: rows
rebound from the pool or created again), end-tone (the last 20 rows)."""
import re
import sys
from PIL import Image, ImageChops

virtual, eager = sys.argv[1], sys.argv[2]
fails = []
PALETTE = ["#e53935", "#fb8c00", "#fdd835", "#43a047", "#00acc1", "#1e88e5", "#5e35b1"]
N, ROW_H, VIEW = 2000, 20, 400
WINDOW_MAX = 3 * (VIEW // ROW_H) + 4  # viewport ± one viewport of margin (+ partial rows)


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def log(d):
    text = open(f"{d}/run.log").read()
    grabs = {m[0]: (float(m[1]), float(m[2]), int(m[3]))
             for m in re.findall(r"FSV grab (\S+) contentY=(\S+) contentHeight=(\S+) live=(\d+)", text)}
    mounted = re.search(r"FSV mounted sync (\d+) ms \+tick (\d+) ms", text)
    entered = sum(int(x) for x in re.findall(r"^\[phase\] repeater\.(?:beginCreate|rebind) .*?\bx(\d+)\b", text, re.M))
    created = sum(int(x) for x in re.findall(r"^\[phase\] repeater\.beginCreate .*?\bx(\d+)\b", text, re.M))
    return grabs, mounted, entered, created


def frame(d, name, first_row, tone):
    im = Image.open(f"{d}/{name}.png").convert("RGB")
    for k in range(VIEW // ROW_H):
        y = k * ROW_H + ROW_H // 2
        for x, want, what in ((200, hexrgb(PALETTE[(first_row + k) % 7]), f"row {first_row + k} background"),
                              (10, hexrgb(tone), f"row {first_row + k} swatch (tone)")):
            got = im.getpixel((x, y))
            if any(abs(a - b) > 2 for a, b in zip(got, want)):
                fails.append(f"{d.rsplit('/', 1)[-1]} {name}: {what} at ({x},{y}): got {got}, expected {want}")
    return im


steps = [("top", 0, "#ffffff"), ("mid", 1000, "#ffffff"), ("mid-tone", 1000, "#00ff00"),
         ("top-tone", 0, "#00ff00"), ("end-tone", N - VIEW // ROW_H, "#00ff00")]
vg, vmount, ventered, vcreated = log(virtual)
eg, emount, _, ecreated = log(eager)
for name, first, tone in steps:
    for d, grabs in ((virtual, vg), (eager, eg)):
        if name not in grabs:
            fails.append(f"{d}: no {name} grab")
            continue
        y, h, live = grabs[name]
        if y != first * ROW_H:
            fails.append(f"{d} {name}: contentY {y}, expected {first * ROW_H}")
        if h != N * ROW_H:
            fails.append(f"{d} {name}: contentHeight {h}, expected {N * ROW_H} (exact logical extent)")
    vi, ei = frame(virtual, name, first, tone), frame(eager, name, first, tone)
    if ImageChops.difference(vi, ei).getbbox() is not None:
        fails.append(f"{name}: the virtualized frame differs from the eager one")
    if name in vg and vg[name][2] > WINDOW_MAX:
        fails.append(f"virtual {name}: {vg[name][2]} live rows, expected at most {WINDOW_MAX}")
    if name in eg and eg[name][2] != N:
        fails.append(f"eager {name}: {eg[name][2]} live rows, expected {N}")

# Rows that ever entered the window (created or rebound), mount + 4 scroll positions: a few windows,
# never the list.
if not 0 < ventered <= 5 * WINDOW_MAX:
    fails.append(f"virtual: {ventered} rows entered the window, expected 1..{5 * WINDOW_MAX}")
if ecreated != N:
    fails.append(f"eager: {ecreated} rows created, expected {N}")

timing = (f"mount 2000 rows: virtualized sync {vmount[1]} ms (+tick {vmount[2]} ms), "
          f"eager sync {emount[1]} ms (+tick {emount[2]} ms)") if vmount and emount else "mount timing missing"
if fails:
    print("for-style-virtual: FAIL\n  " + "\n  ".join(fails))
    print("  " + timing)
    sys.exit(1)
print(f"for-style-virtual: PASS (virtualized: {vcreated} created, {ventered} entered the window, "
      f"live <= {max(g[2] for g in vg.values())}; eager: {ecreated} created; 5 frames pixel-identical; "
      f"recycled rows and the live signal correct)")
print("  " + timing)
