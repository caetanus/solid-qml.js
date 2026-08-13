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
