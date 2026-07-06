#!/usr/bin/env python3
"""Generate a grayscale heightmap PNG for QtGraphs Surface3D from a gridded CSV.

Canonical use: the Walker Lake exhaustive dataset (Isaaks & Srivastava) — columns X, Y, V
(and U, T) on a regular grid. We rasterize variable V onto a WxH image, min-max normalized to
0..255, so <Surface heightMap="…"> renders the topography. Usage:

    python3 scripts/csv-to-heightmap.py walkerlake.csv out.png --value V

Any gridded CSV with X/Y coordinate columns and a value column works.
"""
import argparse, csv, sys
from PIL import Image

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv"); ap.add_argument("out")
    ap.add_argument("--x", default="X"); ap.add_argument("--y", default="Y")
    ap.add_argument("--value", default="V")
    a = ap.parse_args()
    xs, ys, pts = set(), set(), {}
    with open(a.csv, newline="") as f:
        for row in csv.DictReader(f):
            try:
                x, y, v = float(row[a.x]), float(row[a.y]), float(row[a.value])
            except (KeyError, ValueError):
                continue
            xs.add(x); ys.add(y); pts[(x, y)] = v
    if not pts:
        sys.exit("no (X,Y,value) rows found — check --x/--y/--value column names")
    xs, ys = sorted(xs), sorted(ys)
    xi = {x: i for i, x in enumerate(xs)}; yi = {y: i for i, y in enumerate(ys)}
    vmin, vmax = min(pts.values()), max(pts.values())
    span = (vmax - vmin) or 1.0
    img = Image.new("L", (len(xs), len(ys)))
    px = img.load()
    for (x, y), v in pts.items():
        px[xi[x], yi[y]] = int((v - vmin) / span * 255)
    img.save(a.out)
    print(f"{a.out}: {len(xs)}x{len(ys)} heightmap from {a.value} ({vmin}..{vmax})")

if __name__ == "__main__":
    main()
