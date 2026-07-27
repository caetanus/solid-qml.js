# Architecture

solid-qml.js has three layers: the **transpiler**, the **C++ CSS engine**, and the **browser
shims** over Qt's V4 engine.

## Transpiler (`transpiler/`)

Written in TypeScript. Normalizes Solid's JSX into `h()` calls (via Babel), analyzes the
component's reactive symbols (signals, effects, memos, resources, props) and emits structural
QML — **one `.qml` file per component**. It follows imports to build the dependency graph and
reproduce the composition as QML imports.

It covers reactivity (`createSignal`/`createEffect`/`createMemo`/`createResource`) and control
flow (`<Show>`, lazy `<Switch>`/`<Match>`, `<For>`, `<Index>`, `<Suspense>`), mapping each
construct to its native QtQuick equivalent (bindings, `Repeater`, Loaders).

npm/node module imports are **mirrored**: the package's real code is rewritten into ESM loadable
by V4 and executed on the engine — it is not a reimplementation.

## C++ CSS engine (`vendor/qml-css-engine/`)

Vendored. Does **layout AND paint**: box model, flexbox, grid, `calc()`, `@media`, `vw`/`vh`,
remote `@font-face` (download + cache + registration with `QFontDatabase`). Exposes lightweight
primitives (`CssRect`, `CssText`, `CssFill`, …) over Item/Text/TextInput/MouseArea/Repeater —
no `QtQuick.Controls`.

The hot path is the `CssLayoutEngine` (runs per relayout) and is written in C++ with zero
abstraction overhead — no copies, `std::function` or superfluous virtuals on the hot path.

## Browser shims over V4 (`src/shims/`)

V4 is the ECMAScript interpreter inside Qt/QML, with no DOM and no browser globals. The shims
install the globals Solid code expects:

- `fetch` — real HTTPS via `QNetworkAccessManager`, with `Headers`/`Request`/`Response`/
  `AbortController`.
- `localStorage` — persisted to disk.
- `XMLHttpRequest` — over the same network transport.
- timers (`setTimeout`/`setInterval`) and polyfills for JS missing from V4.

The `loader` (`src/loader.cpp`) is the executable that loads the generated QML, installs the
shims and the CSS sheet, and watches files for hot reload.
