"""Pixel checks for the inline-style showcase grab (tests/inline-style/run.sh).

Geometry from examples/inline-style.css: 16px padding; 44px swatches 8px apart; bars 14px tall,
6px apart, widths 40/120/80/200 from the inline style; the class-only box is #585b70."""
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
fails = []


def expect(x, y, rgb, what):
    got = im.getpixel((x, y))
    if any(abs(a - b) > 2 for a, b in zip(got, rgb)):
        fails.append(f"{what} at ({x},{y}): got {got}, expected {rgb}")


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


BG = hexrgb("#1e1e2e")
BAR = hexrgb("#89b4fa")
colors = ["#e53935", "#fb8c00", "#fdd835", "#43a047", "#00acc1", "#1e88e5", "#5e35b1", "#d81b60"]
for i, c in enumerate(colors):
    expect(16 + i * 52 + 22, 38, hexrgb(c), f"swatch {i}")

top = 16 + 44 + 12
for i, w in enumerate([40, 120, 80, 200]):
    y = top + i * 20 + 7
    expect(16 + w - 2, y, BAR, f"bar {i} inside its inline width {w}")
    expect(16 + w + 2, y, BG, f"bar {i} ends at its inline width {w}")

row = top + 4 * 14 + 3 * 6 + 12
expect(20, row + 32, hexrgb("#ff9800"), "inline background beats the class")
expect(140, row + 32, hexrgb("#585b70"), "class-only box")

if fails:
    print("inline-style: FAIL\n  " + "\n  ".join(fails))
    sys.exit(1)
print("inline-style: PASS (8 swatches, 4 bars, inline beats class)")
