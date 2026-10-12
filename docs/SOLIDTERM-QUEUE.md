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
- **[x] ~~Emboss makes the text look badly kerned~~ — MOOT, not fixed, REMOVED (2026-10-12).**
  This item described the old per-glyph emboss draw (`terminalview.cpp:428-434` as of 2026-08-13: a
  dark glyph copy at (x+1,y+1) + a light one at (x-1,y-1), FULL-CELL quads bleeding into the
  neighbouring cell). That code no longer exists: `e1908eb` moved emboss from the glyphs to the
  BACKGROUND image, and `8523850` (2026-10-03, see "Background blur" below) replaced background
  emboss with compositor blur entirely — "Emboss was removed" is already documented further down
  this file. There is nothing left to clip/bake/fix; this bullet was never updated when the feature
  was removed. Verified by reading the current `terminalview.cpp` (no `emboss` anywhere) and the git
  history (`3272e09`→`e1908eb`→`58f0f96`→`8523850`) before touching anything.
- **[x] Selection should autoscroll past the top/bottom edge (owner, 2026-10-12).** While dragging a
  LOCAL selection (no mouse reporting, or Shift) with the pointer held above/below the view, a
  `QTimer` (`apps/solidterm/native/autoscroll.h` + `TerminalView::autoscrollTick`) now scrolls into
  the scrollback/toward live at a speed proportional to how far past the edge the pointer sits —
  xterm/VTE/Tilix style, keeps going with NO further mouse movement, stops on release or re-entry.
  Selection anchors are already in absolute buffer coordinates (`CellPos::absRow`), so they track
  correctly as content scrolls under them. Unit tests: `apps/solidterm/tests/autoscroll_test.cpp`.
  Verified live via `SOLIDTERM_AUTOSCROLLTEST` on the real GPU (offset moved 0→100 while the pointer
  sat still above the view; selection grew to span real scrollback content).
  *Gotcha found during verification*: driving this test with a hand-built `QMouseEvent` via
  `QCoreApplication::sendEvent(window, …)` silently drops the MOVE — Qt Quick's mouse grab is keyed
  to the originating `QPointingDevice`/point id, which a from-scratch `QMouseEvent` doesn't carry
  across calls. Use `QWindowSystemInterface::handleMouseEvent` (same entry point `QTest::mouseMove`
  uses) for any hook that presses then moves outside the item's bounds.
- **[x] Optional scrollbar for the scrollback (owner, 2026-10-12).** `TerminalView` composes a small
  Controls-free QML bar (`TerminalView::composeScrollBar`, same visual language as the engine's
  `QmlCss::composeScrollBar` in `cssscroll.h` — 12px track, 6px rounded thumb, same alpha states —
  but bound to `scrollOffset`/`scrollbackCount`/`viewportRows` instead of a Flickable's `contentY`,
  since TerminalView paints its own grid rather than hosting content in one). Thumb size =
  viewport/(scrollback+rows); drag/track-click/wheel all funnel through `TerminalView::scrollTo()`.
  Config key `scrollbar` ("always"/"auto"/"never", default "auto" — shown while scrolling/hovering,
  fades after 1s idle), forwarded `TerminalPanes`→`TerminalTabs`→`term.tsx` the same way `blur` is.
  Hidden with no scrollback AND on the alternate screen (`TerminalView::altScreen`, vim/less — their
  own scrolling, ours would be meaningless) even if scrollback is left over from before. Unit tests:
  `apps/solidterm/tests/scrollbar_test.cpp` (thumb geometry/paging formulas). Verified live via
  `SOLIDTERM_SCROLLBARTEST`: idle (hidden) → wheel (visible, thumb moved) → drag (offset follows the
  thumb) → `less /etc/hostname` (hidden on the alt screen) → `scrollbarMode="never"` (hidden despite
  scrollback+offset>0) — screenshots in `docs/evidence/solidterm/scrollbar-*.png`.
- **[x] Copy/open links (owner, 2026-10-12: "não dá pra copiar links no terminal").** The existing
  auto-detected-link feature (`871c209`, Ctrl+click to open) had no way to COPY a link — that was the
  actual complaint. Added: a right-click on a link now shows "Copy link"/"Open link" at the top of
  the Solid `<ContextMenu>` (`term.tsx`, via `TerminalView::hoverLinkChanged` forwarded through the
  same focused-pane delegation chain as `titleChanged`); double-click on a URL selects the WHOLE
  thing (not word-bounded — `/:.?&=#` etc. no longer stop it); `mailto:` added to the scheme list;
  trailing-punctuation trimming is now bracket-aware (VTE rule: an unmatched closing
  `)`/`]`/`}` is trimmed, a balanced one — e.g. a Wikipedia "`…(disambiguation)`" link — is kept); a
  URL split across a soft-wrapped row is detected whole (LIVE rows only — `VTermLineInfo::continuation`
  has no scrollback equivalent, a documented limitation). With mouse reporting active (vim/htop/tmux),
  all of this only applies with Shift held, same rule as local selection (`decideMouseAction`). The
  scanner itself is a new pure (Qt-free) header, `apps/solidterm/native/linkscan.h`, replacing the
  old inline `QRegularExpression` — unit tests in `apps/solidterm/tests/linkscan_test.cpp` (scheme
  table, trailing-punctuation table, a row-joined soft-wrap scenario). OSC 8 explicit hyperlinks were
  investigated (`vterm_screen_set_unrecognised_fallbacks` is the right hook) but NOT implemented:
  libvterm's screen layer has no per-cell metadata and no `moverect` is currently wired, so tracking
  an OSC 8 span reliably needs more plumbing than fit this pass — left as a follow-up, not started.
  Verified live via `SOLIDTERM_LINKTEST` on the real GPU: hover found the right URL, moving off
  cleared it, double-click copied the whole URL (not a fragment), and the hover state was still
  correct immediately before a right-click (exactly what the context menu's reactive binding reads).

## Open


- **[ ] Maximize needs an animation.** Toggling zoom snaps. It should tween the pane geometry
  (the CSS engine supports `transition`, so a class swap may be enough — otherwise animate in
  `paneheader.cpp` / the pane container).
- **[x] Detach does not blur / alpha.** Done (st-detach): the dragged pane now shows a drag preview
  — a rounded, translucent snapshot of the pane (keeps the window's own alpha, then 82 % opacity,
  accent border) — and the drop indicator appears on every valid target (split zone of any pane in
  any window, or the chrome strip for "new tab"). The preview is the QDrag icon; its surface gets the
  same `ext-background-effect-v1` request as the windows (`WindowBlur`, config `blur`). Limit: the
  owner's sway blurs only views, so the icon is translucent without blur there.
- **[x] Cannot re-attach a pane between two SolidTerm windows.** Done (st-detach): the header drag is
  a real QDrag (`application/x-solidterm-pane`, "<pid>:<serial>"); every window's `TerminalTabs`
  filters its window's drag events, so the COMPOSITOR picks the window under the cursor (Wayland has
  no global coordinates — the old `mapFromGlobal` hit-test could not work there). Drop on a pane's
  side → spliced into that window's split tree (the live `TerminalView` + pty move; same shell PID,
  scrollback intact); on the header/tab strip → a new tab; the emptied source window closes; Esc or
  a drop outside every SolidTerm window → detach to a new window as before; another process's pane
  is refused. Verified with a real pointer drag in a nested headless sway and on the real display;
  meson test `solidterm-pane-drop`. Evidence: `docs/evidence/solidterm/xdrop-*`.
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
