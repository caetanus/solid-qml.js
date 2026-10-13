#!/usr/bin/env python3
"""SolidTerm search bar evidence: capture the Ctrl+Shift+F bar offscreen in light and dark, and
assert every icon (the search glyph + the ↑/↓/✕ buttons) is vertically centred on the field.

  scripts/solidterm-searchbar-evidence.py <solidterm-binary> <app-dir> <outdir>

Launches <solidterm-binary> offscreen with SOLIDTERM_DIR=<app-dir>, SOLIDTERM_FINDTEST="hello" (opens
the bar) and SOLIDTERM_PLAINSHOT=<outdir>/searchbar-<scheme> (grabs every top-level window and quits)
— the same debug hooks apps/solidterm/main.cpp already exposes for this. QT_QUICK_BACKEND=rhi forces
a real GPU context: the default offscreen Quick backend ("software") silently drops MultiEffect-based
rendering (the icons' colourisation shader included), which would make this pass for the wrong reason.

Runs once per colour scheme (light, dark): dark is forced via `gsettings set
org.gnome.desktop.interface color-scheme prefer-dark` around the dark capture, after reading and
restoring the ORIGINAL value — never left changed, even if a capture or an assertion raises.

The alignment check is a pixel measurement, not a CSS re-derivation: each icon's own glyph bounding
box (any pixel differing from its local background by a real amount — not anti-aliasing noise) gives
its vertical centre; the field's own box gives the reference. Column ranges below are this bar's
current fixed layout (search icon inside the field's left edge; three 34px-square buttons on the
right, in order) — a rebuilt bar with different button positions would need those column constants
re-measured. That's the point: this is a regression test for the KNOWN current layout, not a
general-purpose interpreter of an arbitrary one.

Exit code 0 and "PASS" per scheme when every icon's centre is within ALIGN_TOLERANCE_PX of the
field's; exit 1 and "FAIL" (with the measured deltas) otherwise.
"""
import os
import subprocess
import sys

ALIGN_TOLERANCE_PX = 1.0
# (x0, x1) windows in the RAW (1x) screenshot, y always 0..46 (the bar's own height: 6px padding +
# 34px row + 6px padding). Tuned against this bar's current CSS (apps/solidterm/src/term.css
# .searchbar/.search-in-wrap/.search-btn) — see the module docstring.
FIELD_COL = (495, 520)        # inside .search-in-wrap, away from any glyph
SEARCH_ICON_COL = (8, 32)     # the <icon name="edit-find-symbolic"> inside the field
UP_COL = (905, 939)           # .search-btn #1 (go-up-symbolic)
DOWN_COL = (939, 973)         # .search-btn #2 (go-down-symbolic)
CLOSE_COL = (973, 1000)       # .search-btn #3 (window-close-symbolic)
BAR_HEIGHT = 46


def glyph_bbox_center_y(arr, x0, x1, y0, y1, bg):
    """Vertical centre (in pixels) of the glyph in arr[y0:y1, x0:x1] — the bounding box of pixels
    that differ from `bg` by more than antialiasing noise. None if the window is empty/uniform."""
    import numpy as np
    region = arr[y0:y1, x0:x1].astype(int)
    diff = np.abs(region - np.array(bg)).sum(axis=2)
    mask = diff > 40
    ys, _xs = np.where(mask)
    if len(ys) == 0:
        return None
    return y0 + (int(ys.min()) + int(ys.max())) / 2.0


def measure(png_path):
    """Returns {name: (center_y, delta_from_field)} for every icon in the screenshot, plus
    "field_center" itself. Raises if the field or any icon cannot be located at all (a layout
    change big enough to need this script's column constants updated, not a centring regression)."""
    from PIL import Image
    import numpy as np

    im = Image.open(png_path).convert("RGB")
    arr = np.array(im)
    field_bg = tuple(int(c) for c in arr[23, (FIELD_COL[0] + FIELD_COL[1]) // 2])
    bar_bg = tuple(int(c) for c in arr[2, 2])

    field_center = glyph_bbox_center_y(arr, *FIELD_COL, 0, BAR_HEIGHT, field_bg)
    if field_center is None:
        raise RuntimeError(f"{png_path}: could not locate the field's own box at x={FIELD_COL} — "
                            "layout changed? update FIELD_COL")

    icons = {
        "search-icon": glyph_bbox_center_y(arr, *SEARCH_ICON_COL, 0, BAR_HEIGHT, field_bg),
        "up": glyph_bbox_center_y(arr, *UP_COL, 0, BAR_HEIGHT, bar_bg),
        "down": glyph_bbox_center_y(arr, *DOWN_COL, 0, BAR_HEIGHT, bar_bg),
        "close": glyph_bbox_center_y(arr, *CLOSE_COL, 0, BAR_HEIGHT, bar_bg),
    }
    missing = [name for name, c in icons.items() if c is None]
    if missing:
        raise RuntimeError(f"{png_path}: could not locate icon(s) {missing} — "
                            "layout changed? update their column constants")

    return field_center, {name: (c, c - field_center) for name, c in icons.items()}


def gsetting(schema, key):
    out = subprocess.run(["gsettings", "get", schema, key], capture_output=True, text=True, timeout=5)
    return out.stdout.strip().strip("'")


def capture(binary, app_dir, outdir, scheme_tag, extra_env=None):
    os.makedirs(outdir, exist_ok=True)
    prefix = os.path.join(outdir, f"searchbar-{scheme_tag}")
    env = dict(os.environ)
    env.update({
        "QT_QPA_PLATFORM": "offscreen",
        "QT_QUICK_BACKEND": "rhi",         # NOT the default offscreen "software" backend — see docstring
        "QSG_RHI_BACKEND": "opengl",
        "QT_FORCE_STDERR_LOGGING": "1",
        "SOLIDTERM_DIR": app_dir,
        "SOLIDTERM_FINDTEST": "hello",
        "SOLIDTERM_PLAINSHOT": prefix,
    })
    if extra_env:
        env.update(extra_env)
    subprocess.run([binary], env=env, timeout=20, check=False)
    png = f"{prefix}-0.png"
    if not os.path.exists(png):
        raise RuntimeError(f"no screenshot produced at {png} (binary: {binary})")
    return png


def run_scheme(binary, app_dir, outdir, scheme_tag, label):
    png = capture(binary, app_dir, outdir, scheme_tag)
    field_center, icons = measure(png)
    ok = True
    print(f"[{label}] field_center={field_center:.1f}")
    for name, (center, delta) in icons.items():
        status = "OK" if abs(delta) <= ALIGN_TOLERANCE_PX else "MISALIGNED"
        if status != "OK":
            ok = False
        print(f"[{label}] {name}: centre={center:.1f} delta={delta:+.1f}px {status}")
    print(f"[{label}] {'PASS' if ok else 'FAIL'} (tolerance ±{ALIGN_TOLERANCE_PX}px) — {png}")
    return ok


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    binary, app_dir, outdir = sys.argv[1], sys.argv[2], sys.argv[3]

    ok = run_scheme(binary, app_dir, outdir, "light", "light")

    schema, key = "org.gnome.desktop.interface", "color-scheme"
    original = gsetting(schema, key)
    try:
        subprocess.run(["gsettings", "set", schema, key, "prefer-dark"], check=True, timeout=5)
        ok = run_scheme(binary, app_dir, outdir, "dark", "dark") and ok
    finally:
        subprocess.run(["gsettings", "set", schema, key, original], check=True, timeout=5)
        restored = gsetting(schema, key)
        print(f"[cleanup] color-scheme restored to {restored!r} (was {original!r})")

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
