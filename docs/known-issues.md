# Known issues

Open defects and limits of solid-qml 0.3.0-preview.1, each checked against the code on 2026-10-11.
A workaround is given where there is one. What is supported at all is in the
[support matrix](support-matrix.md).

## Constructs that compile but are wrong at runtime

The transpiler rejects most constructs it cannot translate (the attribute contract and the
statement translator both fail loudly). A few still get through without an error and produce an
app that misbehaves at runtime. The [support matrix](support-matrix.md#solid-apis-jsx-and-statements)
marks them ⚠️ and a test keeps that list up to date. In short:

| Construct | What happens | Workaround |
|---|---|---|
| A helper imported from a relative non-component module (`import { f } from "./util"`) | only components are followed through relative imports; `f` is undefined at runtime | keep helpers in the component's own file, or move them into an npm package |
| A module-level constant initialised with `new …` or a RegExp literal (`const RE = /a/`) | the constant is not emitted; it is undefined at runtime | use string methods, a literal/array/object constant, or an npm package |

These belong to release-plan M2 ("no silent miscompiles"). Until they fail loudly, check new code
against the support matrix.

## Reactivity that differs from Solid

- **`batch` defers computations, not bindings.** Inside `batch(fn)`, effects
  (`createEffect`/`createComputed`/`createRenderEffect`) are queued and run once after `fn`, as in
  Solid. QML bindings (the render and `createMemo`) still update on every write. Their final value
  is the same, except for a memo that reads a signal through `untrack`: it may keep the value it
  saw at the write that re-evaluated it, where Solid re-evaluates it once after the batch.
- **Effects triggered by the same write run in creation order.** Solid orders them by when each
  last subscribed, which can differ after re-runs. No program should depend on either order.
- **`<Dynamic>` switching components may mount the new one before the old one's cleanups run.**
  Each candidate is its own lazily mounted branch; Solid disposes the old component first.
- **Dependencies are static.** An effect re-runs when anything its body (or the helpers it calls)
  can read changes, not only what the last run read: a branch not taken still subscribes. The
  values are right; an effect may run more often than in Solid.

## Performance and memory

- **Eager lists and per-row memory are not yet ahead of Chromium.** Only scrolling lists that get
  virtualized are, and methodology and raw numbers have not been published yet (release-plan M10).
  An eager list creates and lays out every row up front.
  *Workaround:* make long lists virtualizable. Keep the `<For>` rows passive (plain elements,
  classes and text, with no handlers or refs on the row) and put them in a vertical scroll
  container (`overflow-y: auto`). `transpiler/src/emit/forCertify.ts` decides which rows qualify.
- **Virtualization is switched off while an assistive technology is active**
  (`QAccessible::isActive()`, `vendor/qml-css-engine/src/qmlcss/cssrepeater.cpp`). With a screen
  reader running, every list is eager so that every row is reachable. There is no workaround yet;
  virtualization with a screen reader active is outside the preview.
- **`<DocumentEditor>` is slower to edit in the paginated (print) view than in the continuous
  view**, especially mid-document in a long book. Qt's own pagination in `QTextDocumentLayout` is
  the cause. The opt-in gate `RICHTEXT_PERF_BUDGET=1` in `tests/richtext_test.cpp` exposes it, and
  it is not fixed. The project's last report was about 26 ms per keystroke on a 300-page book; that
  number was not re-measured for this document.
  *Workaround:* set `pageView: false` on the editor (from QML or through the handle) for
  continuous view.

## Widgets

- **`<Menu>` positioning.** On Wayland a popup anchors to its parent item, so the transpiler moves
  a 1×1 host item to the click point and opens the menu there
  (`transpiler/src/emit/native/menus.ts`). Mapping that point through the CSS layout's item
  nesting can put the menu in the wrong place. Diagnosed, not fixed; it needs a live Wayland
  session to verify (`docs/RESUME-HERE.md`, A1).
- **`<Menu>` sizing and theming.** Labels can be clipped and CSS sizing on the menu is not fully
  honoured (release-plan M4). The opt-in theme `src/solid-qml/themes/desktop.css` styles menu
  rows, separators, the `<select>` list and the calendar through custom properties. It cannot
  give the highlighted row an accent fill with a contrasting label, because that needs ancestor
  `:hover` matching, which the engine does not have yet.
- **Menu mnemonics are only visual.** `&File` underlines the letter but binds no Alt+letter
  accelerator, and F10/Alt does not open a menu bar (`src/widgets/menuwidgets.cpp`
  `__mnemonicMarkup`). *Workaround:* `<Shortcut keys="Alt+F" onActivated={…}>`.
- **`<WebView>` can go black after being hidden and shown again.** Reported, not fixed, and not
  re-verified for this release, since offscreen rendering cannot show it. A possible workaround
  (not verified): mount the view under `<Show>`, so that hiding destroys it and showing creates it
  again.
- **Form controls have no default look.** An unstyled `<input type="checkbox">` (or radio) has
  zero size and is invisible. *Workaround:* give it a `width`/`height` and style `.indicator`,
  `:checked .indicator` and `.indicator-glyph`; the CSS snippet is in
  [support matrix → elements](support-matrix.md#elements-and-their-attributes), and
  `examples/widgets.css` styles every control.
- **Scrolling:** wheel and scrollbar only. There is no keyboard scrolling (PageUp/PageDown/
  Home/End), no horizontal scrolling (`overflow-x`) and no kinetic fling
  (`vendor/qml-css-engine/src/qmlcss/cssscroll.h`).
- **`title` tooltips** work on generic elements (`<div>`, `<span>`…). They are not available on
  `<button>` (the attribute is rejected) or in the AOT back-end.
- **Data views** (`<TableView>`, `<TreeView>`) are built from plain items rather than Qt's item
  views (`docs/native-behavior-audit.md`, P5).
- **`<DocumentEditor>` ODF coverage:** page setup, headers and footers, footnotes (their text is
  kept inline), bookmarks and links (their text is kept) are not preserved
  ([document-editor.md](document-editor.md), Notes).

## CSS

- Unknown properties and selectors are **ignored without a warning**.
- `>`, `+` and `~` all match as the descendant combinator. Attribute selectors and structural
  pseudo-classes (`:nth-child`, `:not`…) are not implemented.
- `position: relative/fixed/sticky`, `display: inline/inline-block`, `margin: auto`, `hsl()`,
  `currentColor`, `filter` and `font-style: italic` are not implemented.

The full list is in [support matrix → CSS](support-matrix.md#css).

## Platform, build and distribution

- **Qt 6.11 only; Qt 6.12 is not supported yet.** The text layout (`csstextlayout` in the engine)
  is a port of Qt 6.11's `QQuickText`, and the library uses private Qt headers (list below). When
  Arch's rolling mirror moved to Qt 6.12.0, the engine's text-metric oracle tests started failing,
  which is why CI and the clean-room check pin an Arch archive snapshot with Qt 6.11.2
  (release-plan M7). *Workaround:* build against Qt 6.11.x.
- **Debian, Ubuntu and Fedora are untested.** [Getting started](getting-started.md) gives
  best-effort package names only.
- **No npm package.** A new app imports the runtime by path from a clone of this repository and
  runs the clone's transpiler ([getting started](getting-started.md#your-first-app-outside-the-checkout)).
  No AppImage or binary SDK either; this preview is source only.
- **`qml-css-engine:text` is sensitive to the exact font and HarfBuzz version.** See
  [getting started → troubleshooting](getting-started.md#troubleshooting).
- **Headless interaction testing is mouse-only.** `solid-qml-loader --click x,y,ms` sends real
  clicks before `--grab`; there is no way to inject keystrokes, so typing into an `<input>` cannot
  be tested offscreen with the loader.
- **Offscreen tests cannot check popups, GPU materials or 3D.** Those were checked by hand on a
  real display, and only partly (release-plan M4).
- **The embed library has no automated test.** `examples/embed-host` (the `embed-host-demo`
  executable, built when the `examples` option is enabled, which is the default when its
  dependencies are found) is a demo; the clean-room smoke test covers the installed loader, not
  embedding.
- **The AOT (C++) back-end is experimental** and is not part of the preview.

### Private Qt API in use

Private headers carry no compatibility promise from Qt, so `libsolidqml` has to be rebuilt for any
Qt upgrade ([release policy](release-policy.md)):

| File | Private API | Why |
|---|---|---|
| `src/widgets/a11y.cpp` | `QQuickItemPrivate`, `QAccessibleQuickItem`, `QQuickAccessibleAttached` | accessibility on demand |
| `src/widgets/menuwidgets.h`, `src/widgets/tabbutton.h` | `QQuickMenu`, `QQuickMenuItem`, `QQuickMenuSeparator`, `QQuickTabButton` (QtQuickTemplates2) | the widgets subclass them |
| `src/widgets/swipe.cpp` | `QQuickTranslate` | swipe rows |
| `src/shims/richtext.cpp` | `QTextDocumentPrivate`, `QQuickTextEdit(Private)`, `QQuickTextControl(Private)` | margins without an undo step, size refresh (see [document-editor.md](document-editor.md)) |
| `src/shims/richtext_odf.cpp` | `QZipReader`, `QZipWriter` | reading and writing `.odt` |
| `src/shims/sharedbuffers.cpp` | V4 internals (`qv4engine_p.h`, `qv4arraybuffer_p.h`, `qv4object_p.h`…) | `SharedArrayBuffer` |
| `vendor/qml-css-engine/src/qmlcss/cssbox.cpp`, `csstext.cpp` | `QQuickItemPrivate`, `QQuickItemChangeListener` | geometry listeners, layout mirroring |
| `apps/solidterm/native/windowblur.cpp` | `QPlatformWindow` (QPA) | SolidTerm's blur only; not part of the library |
