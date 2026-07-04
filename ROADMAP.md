# Roadmap — solid-qml-native

**An Electron *and* React Native competitor** ("React Native for QML"): web authoring DX
(Solid.js + JSX + CSS) rendering to **real native components** (QtQuick scene-graph items, not a
webview), cross-platform via Qt (desktop and mobile) — with maximum performance and minimum
memory footprint, no Chromium/Node per app and no React-Native-style JS bridge. A native app
must be *faster and lighter* than web, never slower. Every design choice serves that: lightweight
QtQuick primitives (no heavyweight QtQuick.Controls), paint **and** layout in a C++ CSS engine,
and a release path that AOT-compiles to C++ to drop the interpreter (and its bridge) entirely.

A Solid.js-style framework targeting QML/C++. Today: TSX → generated QML, loaded at runtime
by the V4 engine (handler/expression JS interpreted); paint+layout by the CSS engine in C++;
browser APIs (localStorage/fetch/timers/…) via C++ shims.

## Release builds: TSX → QtQuick C++ 1:1 (template, no qmltc)  ⭐
In release, **don't** load QML text at runtime or interpret in V4. Since **we** generate the
QML (a controlled, simple subset), we don't need **qmltc** (which exists to compile
*arbitrary* `.qml`): the transpiler itself emits **C++ directly, 1:1, via a template**. QML is
sugar over `QObject`/`QQuickItem` construction — the mapping is direct:

- **Structure**: each element → imperative C++ construction (`new` of the Css/QQuickItem type,
  `setParent`, `setProperty` for static props). `Component.onCompleted` → a call after
  construction. `MouseArea.onClicked`/signals → `connect` to a C++ lambda.
- **Reactive bindings / handler expressions** (`text: "…" + window.events`,
  `onClicked: window.events = window.events + 1`): the transpiler already translates the
  expression (`toQml`); in release, emit the equivalent expression in **C++** and connect it to
  the corresponding change signal — **AOT by generation**, eliminating V4 for the supported
  subset. Boundary: genuinely dynamic JS (async/fetch/complex closures) stays on V4 or needs an
  embedded JS runtime; the rest becomes C++.
- The layout engine, the shims, and the CSS engine are already C++ and stay.
- **Pipeline**: dev = transpiler → interpreted `.qml` (fast iteration, with source map/debug);
  release = transpiler → C++ (template), behind a build flag. The same AST/Block model we
  already have feeds both generators.
- **Implementation**: no extra toolchain — Node is already required, so a single **template**
  pass (Mustache/EJS/template-literal) over the same AST/Block, emitted from the **Vite plugin
  output** (`src/solid-qml/vite-plugin.ts` gains a `cpp` target alongside `qml`/`web`). The
  transpiler becomes the "front-end" (AST→Block) and the back-ends (QML today, C++ in release)
  are just template renderers over the same model.

## Debug / DX
- **VS Code debug proxy** (`tools/qml-tsx-dap/`): validate the live link (VS Code ↔ qml-debug ↔
  engine), calibrate path mapping (qrc vs filesystem) and DAP message types.
- **Source map granularity**: currently per-line/element (a handler resolves to the `<button>`
  line); refine to column/token for the JS bits (instrument `toQml`).

## Engine / CSS fidelity (minor pending)
- `grid`: `fr`/`minmax`/`repeat` tracks ok; missing cases (dense auto-rows, `grid-auto-flow`,
  `grid-column`/`grid-row` spans).
- `radial` gradient on the window background (combined with linear) — ok via CssFillLayer;
  revisit fine size/position.
- Polish: `text-shadow`, `backdrop-filter`/blur (the card's glassmorphism), `@supports`/
  `@container` (currently dropped).
- `@media`: supported (min/max-width/height); other features missing (orientation, prefers-*).

## V4→V8 shims (remaining)
- Done: layout/anim in C++, localStorage, fetch (+Headers/Request/Response/Abort),
  timers+queueMicrotask, btoa/atob, TextEncoder/Decoder, crypto, performance, structuredClone,
  async/await (Babel, web+transpiler).
- Not feasible: `BigInt` (and `BigInt64Array`/`BigUint64Array`) — V4 lacks it; arbitrary
  precision is out of scope.
- `crypto.subtle` (SubtleCrypto) if ever needed.

## Web target
- Today `vite-plugin-solid` renders in the browser for preview; keep parity with the QML target
  as the engine evolves.

## Native-only track (post-milestone-1)

Web parity is the floor, not the ceiling. Native-only capabilities, in intended order:

- **All QtQuick widgets** — complete the native widget surface beyond the HTML-mappable
  set (tree views, tables, dock/split layouts, menus/menu bars, system tray, dialogs).
- **Native C++ integration** — import and call the user's own C++ code from app sources
  (typed bindings, zero-overhead calls), the same way npm modules import today.
- **Component generation for legacy projects** — emit consumable QML/C++ components from
  app sources so existing Qt codebases can adopt pieces incrementally.
- **Native app examples** — real applications proving the model end to end: a terminal,
  a media player, a photo album, a simple browser.
- **Threading** — worker threads with a web-worker-like API surface over Qt's threading.
- **Background services (mobile)** — long-running work on Android/iOS lifecycles.
- **Push notifications** — native push wiring on desktop and mobile.
