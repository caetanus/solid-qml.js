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
- **[x] New option: focus-on-hover.** Done (4b76be1): Preferences → Behavior → "Focus follows
  mouse" (off by default, `termConfig` key `focusOnHover`). Entering a pane focuses it only while the
  SolidTerm window is already active, with no mouse button held and no pane drag in progress.
- **[x] New option: gap between splits.** Done (4b76be1): Preferences → Appearance → "Gap between
  splits" (0-24 px, live, key `splitGap`). 0 keeps the 6 px handle-coloured divider; above 0 the
  divider is that wide and transparent (the window background shows between panes), still draggable.

## Pre-existing bugs found during the gallery sweep (not SolidTerm, but recorded here)

- **Tray snippet fails**: `solidwidgets-tray` assigns `onAvailableChanged`, but the
  `SystemTrayIcon` in this Qt has no `available` property → "Cannot assign to non-existent
  property". The tray never composes. (`src/widgets/tray.cpp:18`; untouched by the native pass.)
- **TextArea anchor loop**: the widgets view logs "QML TextArea: Possible anchor loop detected on
  fill" ×4–5. (`src/widgets/textinputs.cpp`; also untouched.)

## Background blur behind the window — DONE via ext-background-effect-v1 (2026-10-03)

Supersedes the 2026-08-13 "backdrop effects" spec (layer-shell overlay + screencopy + our own shader),
which was dropped: a client never sees the pixels behind its surface — translucency is blended by the
compositor after our frame leaves the process — so the only ways to filter the backdrop are (a) the
compositor does it, or (b) the app screen-captures the desktop and must then learn its own window
position, which Wayland does not expose (wlr-foreign-toplevel has no geometry; only compositor IPC
does — sway-only, the WallpaBlur approach). (b) is not compositor-agnostic, so it is out.

**Now:** SolidTerm asks the compositor to blur behind its translucent window through the standard
`ext-background-effect-v1` (`native/windowblur.{h,cpp}`). Implemented by Mutter 51, KWin 6.7, niri
26.04 and others; a compositor without it leaves the window plainly translucent. One "Blur background"
slider (config key `blur`, 0-100) drives it: 0 = off; above 0 the compositor blur is requested (on/off
only — the protocol carries no strength, the radius is compositor policy) and the app's own
background image is box-blurred by that strength. Emboss was removed (owner: "troque o slide do emboss
pra blur"). `SOLIDTERM_BLURINFO=1` logs whether the compositor supports it.

**Open:** the owner's sway (1.13-dev) does not implement the protocol — blur shows there only once the
compositor side exists.
