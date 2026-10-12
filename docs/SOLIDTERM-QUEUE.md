# SolidTerm — owner's UX queue (2026-08-13)

Reported in one batch while validating the gallery, in the order they were raised. Nothing here is
a regression from the native-behavior pass unless noted. Updated 2026-10-12: the config-panel
colour and two-column items were fixed by an earlier session but left marked open in this doc;
closed now with fresh evidence, plus a new scrollbar-visibility option and one newly-found item
(button contrast on the accent colour).

## Done

- **[x] Maximize icon was wrong.** It drew one continuous diagonal with corner brackets (reads as a
  resize cursor). Tilix's zoom glyph is TWO SEPARATE arrows pointing out to opposite corners with a
  gap between them — redrawn that way in `apps/solidterm/native/paneheader.cpp` (stem + arrowhead per
  arrow). *Needs a look on a real desktop; the offscreen shot path did not trigger.*
- **[x] Opacity should be a slider that updates live.** The fixed `<select>` of seven percentages is
  now `<input type="range" min=40 max=100>` with the value beside it, emitting `onMoved` so the
  window updates WHILE dragging (`term.tsx` + `.cfg-slide` styles in `term.css`).
- **[x] Config panel: black font on a dark theme.** Fixed by an earlier session (commits
  `4804804`, `90ae8b2`, `fdc8d05` — labels got an inheritable `color`; a `TextField`'s value, a
  `Select`'s `.value`/`.chevron`, and a button's text NODE each needed their own scoped rule in
  `sysTheme->styleSheet()`, since colour on the container alone doesn't reach a separate text
  node). This item was left open in this doc even after the fix landed; closing it now with fresh
  evidence (2026-10-12): offscreen `SOLIDTERM_SHOT` grabs of the Preferences dialog forced to each
  GNOME `color-scheme`, pixel-sampled with WCAG relative-luminance contrast —
  `docs/evidence/solidterm/{dark,light}-1.png`. Every row label, input value, select value/chevron,
  slider value, heading and keyboard-shortcut field passes AA (≥4.5:1) in BOTH schemes:

  | element | dark bg→text | ratio | light bg→text | ratio |
  |---|---|---|---|---|
  | heading (`cfg-group`) | #242424→#9a9a9a | 5.52:1 | #fafafb→#5e5c64 | 6.31:1 |
  | row label (`cfg-l`) | #303030→#fff | 13.20:1 | #fff→#2e3436 | 12.65:1 |
  | input value | #1e1e1e→#fff | 16.67:1 | #fff→#2e3436 | 12.65:1 |
  | select value/chevron | #383838→#fff | 11.73:1 | #fff→#2e3436 | 12.65:1 |
  | button label (`Choose…`) | #383838→#fff | 11.73:1 | #fff→#2e3436 | 12.65:1 |
  | slider value | #303030→#e9e9e9 | 10.87:1 | #fff→#444b4b | 9.13:1 |
  | shortcut field value | #1e1e1e→#fff | 16.67:1 | #fff→#2e3436 | 12.65:1 |

  One control does NOT pass in either scheme and is logged below as a new item: the primary
  "Done" button (`.cfg-close`, white text on the raw GNOME accent colour) measures ~3.77:1 — that
  is an accent/branding contrast gap, not a dark-vs-light bug (same ratio both schemes), so it is
  out of scope for this pass and queued separately.
- **[x] Config panel should be two columns.** Fixed by an earlier session (commit `fdc8d05`):
  `.cfg-scroll` is `display:flex; flex-direction:row; flex-wrap:wrap`, with two explicit
  `.cfg-col` divs (980 px dialog → two ~452 px columns) rather than CSS `grid` — explicit columns
  were chosen over grid auto-placement because auto-placement would drop the short `Keyboard`
  group under the tall `Appearance` column instead of beside it. Tab order is therefore
  COLUMN-MAJOR (all of Appearance, then all of Behavior + Keyboard), which reads naturally since
  each column is a self-contained group of related settings — row-major would jump between
  unrelated Appearance/Behavior fields on every Tab press. Verified in both schemes, same
  screenshots as above (`docs/evidence/solidterm/{dark,light}-1.png`): two columns, no unexpected
  wrapping, less scrolling than the old 620 px single column.
- **[x] New option: terminal scrollbar visibility.** Preferences → Behavior → "Scrollbar"
  (Always / Auto / Never), `termConfig` key `scrollbar`, default `"auto"`, live like the other
  options (`term.tsx`). The terminal view itself reads the key and draws the bar; this only adds
  the control. Fits the two-column layout (same `.cfg-sel` styling, already proven readable above).

## Open

- **[ ] The "Done" button's contrast is marginal on the GNOME accent colour.** `.cfg-close`
  (and `.option-label:hover` / `.ov-active`) use `accentText`, hardcoded to white in
  `systemtheme.cpp`, against the raw accent colour. Measured for the owner's accent (`blue`,
  `#3584e4`): white text ≈ 3.77:1 (fails WCAG AA 4.5:1 for 14px/600 text; passes AA-large's 3:1).
  Swapping to a near-black (`#202020`) helps for most of the 9 GNOME accent names but still falls
  short for several (blue 4.32, teal 4.34, green 4.28, slate 4.16 — only yellow/orange/pink/purple/
  red clear 4.5 with one of the two fixed choices). A real fix needs either a per-accent
  computed text colour (pick whichever of white/near-black wins, still short for blue) or a
  dedicated button background shade — a branding call, not a CSS-layering bug, so left for the
  owner to weigh in on rather than guessed at here.

- **[ ] Emboss makes the text look badly kerned.** Confirmed by reading the draw path
  (`terminalview.cpp:428-434`): emboss pushes a dark glyph copy at (x+1, y+1) and a light one at
  (x-1, y-1). Those copies are FULL-CELL quads, so each one bleeds a pixel into the NEIGHBOURING
  cell — with adjacent characters the shadow of one glyph lands on the next, which reads as broken
  spacing. Fixes to weigh: clip the emboss quads to their own cell (adjust rect AND uv together),
  bake the engrave into the atlas glyph instead of drawing copies, or drop to a single +1 shadow at
  lower alpha (halves the bleed). MUST be verified on a real GPU — the custom QSGMaterial does not
  render under the offscreen/software backend.

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

## 2026-10-11 — owner report
- Mouse events are not forwarded to terminal apps (lvim, TUIs): no VTERM_PROP_MOUSE handling, wheel only scrolls
  SolidTerm's own scrollback. In progress (branch term-mouse): xterm-style reporting, Shift bypass, alternate scroll.
