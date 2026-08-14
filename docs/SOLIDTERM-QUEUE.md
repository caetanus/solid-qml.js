# SolidTerm — owner's UX queue (2026-08-13)

Reported in one batch while validating the gallery. Two are done; the rest are open, in the order
they were raised. Nothing here is a regression from the native-behavior pass unless noted.

## Done

- **[x] Maximize icon was wrong.** It drew one continuous diagonal with corner brackets (reads as a
  resize cursor). Tilix's zoom glyph is TWO SEPARATE arrows pointing out to opposite corners with a
  gap between them — redrawn that way in `apps/solidterm/native/paneheader.cpp` (stem + arrowhead per
  arrow). *Needs a look on a real desktop; the offscreen shot path did not trigger.*
- **[x] Opacity should be a slider that updates live.** The fixed `<select>` of seven percentages is
  now `<input type="range" min=40 max=100>` with the value beside it, emitting `onMoved` so the
  window updates WHILE dragging (`term.tsx` + `.cfg-slide` styles in `term.css`).

## Open

- **[ ] Emboss makes the text look badly kerned.** Confirmed by reading the draw path
  (`terminalview.cpp:428-434`): emboss pushes a dark glyph copy at (x+1, y+1) and a light one at
  (x-1, y-1). Those copies are FULL-CELL quads, so each one bleeds a pixel into the NEIGHBOURING
  cell — with adjacent characters the shadow of one glyph lands on the next, which reads as broken
  spacing. Fixes to weigh: clip the emboss quads to their own cell (adjust rect AND uv together),
  bake the engrave into the atlas glyph instead of drawing copies, or drop to a single +1 shadow at
  lower alpha (halves the bleed). MUST be verified on a real GPU — the custom QSGMaterial does not
  render under the offscreen/software backend.

- **[ ] Config panel: black font on a dark theme.** Unreadable. `term.css` deliberately sets no
  colors for `.cfg-*` — they come from the system-theme layer
  (`sysTheme->styleSheet()`, loaded in `apps/solidterm/main.cpp:95`). So either that layer has no
  rule for the config elements, or a light-theme default wins. Start by dumping the generated
  stylesheet for a dark scheme and checking which selector sets the label/input colour.
- **[ ] Config panel should be two columns.** It is one long `.cfg-scroll` column (620 px wide,
  520 px tall) — a two-column grid would halve the scrolling. `.cfg-body`/`.cfg-scroll` in
  `term.css`; the groups are already discrete `.cfg-card` blocks, so they can flow into a
  `display: grid; grid-template-columns: 1fr 1fr`.
- **[ ] Maximize needs an animation.** Toggling zoom snaps. It should tween the pane geometry
  (the CSS engine supports `transition`, so a class swap may be enough — otherwise animate in
  `paneheader.cpp` / the pane container).
- **[ ] Detach does not blur / alpha.** Dragging a pane out gives no visual feedback; the detached
  preview should carry the window's blur/translucency like the rest of the chrome.
- **[ ] Cannot re-attach a pane between two SolidTerm windows.** Drag-and-drop of a pane from one
  window into another window's split tree does not work — today the tree only accepts moves within
  its own window.
- **[ ] New option: focus-on-hover.** A config toggle that focuses the pane under the pointer
  (X11 "focus follows mouse", per pane). Store it with the other prefs through `termConfig`.

## Pre-existing bugs found during the gallery sweep (not SolidTerm, but recorded here)

- **Tray snippet fails**: `solidwidgets-tray` assigns `onAvailableChanged`, but the
  `SystemTrayIcon` in this Qt has no `available` property → "Cannot assign to non-existent
  property". The tray never composes. (`src/widgets/tray.cpp:18`; untouched by the native pass.)
- **TextArea anchor loop**: the widgets view logs "QML TextArea: Possible anchor loop detected on
  fill" ×4–5. (`src/widgets/textinputs.cpp`; also untouched.)

## Backdrop effects (emboss/blur behind the window) — SPEC, agreed 2026-08-13

The goal that started this: with a translucent window, make what shows THROUGH it carved/blurred.
The effect must act on the real backdrop, not on a background image the user has to pick.

**Design (owner's):**
1. **An overlay window behind everything** — a `wlr-layer-shell` surface on the BACKGROUND layer
   (what swaybg uses), so it sits above the wallpaper and below every window. Qt can drive this
   directly: `LayerShellQt::Window::get(qwindow)->setLayer(LayerBackground)` —
   `libLayerShellQtInterface.so.6` is installed, no raw protocol needed for this half.
2. **Source = what is visible in the overlay window's region** (owner: not the wallpaper file):
   a region capture via `wlr-screencopy-unstable-v1`. Verified available on this machine — a `grim`
   capture returned 3840×2160 under Sway, and both Sway and Hyprland are wlroots-based.
3. **The effect runs in the GL pipeline** (owner), not on the CPU: the captured texture goes through
   a fragment shader (emboss/blur) as a custom `QSGMaterial` — the same path the glyph atlas already
   uses (`shaders.qrc`, `glyph.{vert,frag}.qsb`). Note the project's known trap: a custom material's
   vertex shader must be compiled with `qsb --batchable`, and the software/offscreen backend does not
   render custom materials, so this needs a real-GPU check.
4. **Refresh policy** (owner): when the overlay window changes, re-enter the GL pipeline — i.e. drive
   it off the overlay's geometry/output changes, not a timer.

**Open detail:** the region capture composites everything above that region, so a naive refresh while
our own windows are on top would feed them back. The background layer keeps the OVERLAY itself out of
the shot, but the terminal window is above it — so the capture step needs the terminal excluded
(capture before showing / hide for the capture frame / capture on the compositor's pre-window layer).
Settle this while building; it decides the refresh cadence.

**Slices:** (a) layer-shell background window that renders a solid colour — proves the window type;
(b) region capture into it — proves the source; (c) the effect as a shader material — proves the
pipeline; (d) refresh wiring.
