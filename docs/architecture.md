# Architecture

solid-qml turns Solid.js components (TSX + CSS) into a Qt Quick scene. There are two halves:

- a **build-time** half in TypeScript: the transpiler, which turns TSX into `.qml` files and one
  stylesheet;
- a **run-time** half in C++: the CSS engine, the widgets, the JavaScript shims, and the two ways
  of hosting the result (the loader executable and the embed library).

```
            build time (Node)                              run time (Qt 6.11, C++)
 ┌──────────────────────────────────────┐   ┌───────────────────────────────────────────────────┐
 │ app.tsx + *.css                      │   │ solid-qml-loader            or  your Qt app        │
 │   │ Babel: JSX → h() calls,          │   │   (src/loader.cpp)              + libsolidqml      │
 │   │ TS stripped, async → Promises    │   │                                 (SolidQmlEmbed)    │
 │   ▼                                  │   │        │ loads                                     │
 │ analysis: signals, memos, effects,   │   │        ▼                                           │
 │ stores, props, context (per comp.)   │   │ QQmlEngine (V4 JavaScript)                         │
 │   │                                  │   │   ├─ shims: fetch, localStorage, timers, Workers,  │
 │   ▼                                  │   │   │  fs/process by capability profile (src/shims)  │
 │ QML emit — one .qml per component ───┼──►│   ├─ solidqml.Widgets: C++ widgets (src/widgets)   │
 │ CSS sidecar  App.generated.css ──────┼──►│   │  + opt-in .qml modules (qml/solidqml/Widgets)  │
 │ npm imports mirrored → modules/*.mjs ┼──►│   └─ qmlcss: CSS engine — cascade, layout, paint   │
 └──────────────────────────────────────┘   │       (vendor/qml-css-engine)                      │
                                            └───────────────────────────────────────────────────┘
```

## Transpiler (`transpiler/`)

TypeScript, run with Node. The entry point is `generate()` in `transpiler/src/index.ts`. From the
command line, use `scripts/gen.mjs` (one-shot) or `scripts/dev-native.mjs` (watch and reload).

1. **Normalize** (`transpiler/src/babel/transform.ts`). Babel turns JSX into
   `h(tag, props, …children)` calls, strips TypeScript, and lowers `async`/`await` to Promise
   chains.
2. **Analyze** (`transpiler/src/model/symbols.ts`). For each component it finds the reactive
   symbols: `createSignal`, `createMemo`, `createEffect` (and what each effect reads),
   `createResource`, `createStore`, props, and context providers and consumers.
3. **Follow imports.** A component used as a tag is followed through relative imports, and each
   component becomes its own QML type (`Counter.qml`, `TodoRow.qml`…). Bare (npm) imports are
   **mirrored**: the package's real files are rewritten to ES modules V4 can load and written next
   to the output under `modules/` (`transpiler/src/resolve/node.ts`, `v4-compat.ts`). They are not
   reimplemented. Only components are followed through *relative* imports: a helper function
   imported from a sibling `.ts` file is not (see [known issues](known-issues.md)).
4. **Emit QML** (`transpiler/src/emit/qml.ts`, `component.ts`, `expr.ts`). A signal becomes a QML
   property, its getter a property read, its setter an assignment. A memo becomes a
   `readonly property`. An effect becomes a function that runs once, plus a `Connections` block for
   each dependency. `<For>` becomes a `Css.CssRepeater`. `<Show>`/`<Switch>` become items that are
   created only while their condition holds. Lowercase HTML tags and capitalised native tags go
   through a **tag registry** (`transpiler/src/emit/native/`) to the matching widget type.
5. **Check the attribute contract** (`transpiler/src/emit/attrs.ts`). Every attribute on a built-in
   element must be one its emitter handles; anything else is a compile error. A short documented
   list (`data-*`, and `aria-*`/`role`/`tabIndex` where a widget already has native semantics) only
   produces a warning. A few constructs still get through without an error; the
   [support matrix](support-matrix.md) lists them.
6. **CSS sidecar.** Every `import "./x.css"` in the module graph is concatenated, in import order,
   into `App.generated.css`, after the runtime's `reset.css` (`box-sizing: border-box` and no
   user-agent chrome, the same reset the web preview uses).

There is a second back-end, `transpiler/src/emit/cpp/`, which emits C++ instead of QML (AOT). It is
**experimental**: it covers a counter-sized subset and fails loudly on anything else. It is not part
of this preview.

## C++ CSS engine (`vendor/qml-css-engine/`)

A separate repository, pinned as a git submodule. It is LGPL-3.0-only, because its text layout is a
port of Qt's `QQuickText` (see its `NOTICE`). It is built as a static library, linked into
`libsolidqml`, and generated QML imports it as `import qmlcss 1.0 as Css`.

- **Cascade** (`CssTheme`): selectors, specificity, `!important`, custom properties (`var()`),
  `@media`, `@import`, remote `@font-face` (downloaded, cached and registered with
  `QFontDatabase`) and `@keyframes`.
- **Layout** (`CssLayoutEngine`, `csslayout.cpp`): block flow, flexbox, grid (tracks, `fr`,
  `minmax`, `repeat`, named areas) and absolute positioning, all in C++. A tree that is being
  created is laid out once, when it is complete (a layout batch), rather than once per element.
- **Paint**: `CssRect`/`CssFill` draw backgrounds, gradients, borders, radii and shadows;
  `CssText` lays out and draws text; `overflow: auto|scroll` gives a box a real `Flickable` with a
  desktop scrollbar.
- **Repeater** (`CssRepeater`): the target of `<For>`. It reconciles rows by object reference (or
  by a `key` field when the app asks for it), and it can **virtualize** (see below).

Which CSS is covered: [support matrix → CSS](support-matrix.md#css). The engine's own reference is
`vendor/qml-css-engine/docs/css-support.md`.

## Widgets (`src/widgets/`, module `solidqml.Widgets`)

Every built-in element is a **C++ type** registered in `solidqml.Widgets`
(`src/widgets/solidwidgets.cpp`): `Div`, `Text`, `Button`, `TextField`, `Select`, `Dialog`, `Menu`,
`TabBar`, `TreeView`, `TableView`… The transpiler **instantiates** them (`W.Button { … }`); it does
not hand-emit their internals. Most of them wrap a real `QtQuick.Templates` control (`T.Button`,
`T.TextField`, `T.Menu`…), so keyboard handling, focus, text input, selection and the clipboard
behave the way Qt's controls do. Everything visual comes from your CSS.

Heavier widgets are **opt-in `.qml` modules** under `qml/solidqml/Widgets/`: `Chart`, `Scene3D`,
`Surface`, `MediaPlayer`, `WebView`, `RichText`/`DocumentEditor`, `CodeEditor`. The transpiler adds
the import only to a component that uses the tag, so an app that never uses `<WebView>` never
loads QtWebEngine.

## Runtime shims (`src/shims/`)

Qt's V4 engine has no DOM, no browser globals and no Node. The shims install what Solid code and
npm packages expect, as real globals, visible both to QML JavaScript and to mirrored modules:

- `fetch` with `Headers`/`Request`/`Response`/`AbortController` (HTTPS through
  `QNetworkAccessManager`; `file://` and `qrc:` too); `localStorage` (persisted to disk); timers
  and `queueMicrotask`; `btoa`/`atob`, `TextEncoder`/`TextDecoder`, `crypto.getRandomValues`/
  `randomUUID`, `performance`, `structuredClone`; a fetch-backed `XMLHttpRequest` for npm code;
  additive ES2019–2023 library methods that V4 lacks (`jspolyfill.cpp`).
- `Worker`, `worker_threads`, `SharedArrayBuffer`/`Atomics`. Each worker runs its own QML engine
  on a real thread.
- `process`, `fs` and `child_process`, gated by a **capability profile**: `browser` (none of them),
  `desktop` (the default: `fs` and `process`, no command execution) or `trusted` (everything). The
  profile comes from the loader's `--capabilities`, from `$SOLIDQML_CAPABILITIES`, or from the
  third argument of `SolidQmlEmbed::init`.

The full list is in
[support matrix → runtime shims](support-matrix.md#runtime-shims-and-capability-profiles).

## Hosting: the loader and the embed library

- **`solid-qml-loader`** (`src/loader.cpp`) runs a generated app on its own. It loads the entry
  `.qml` and the CSS, installs the shims, and with `--watch` reloads when the files change (the dev
  loop behind `scripts/dev-native.mjs`). It can also render one frame offscreen to a PNG
  (`--grab`) and run a JavaScript file directly on V4 (`--mini-node`).
- **`libsolidqml`** (`src/embed/solidqmlembed.h`) puts solid-qml inside an **existing Qt Quick
  app**. `SolidQmlEmbed::init(engine)` prepares the host's engine, and a
  `SolidIsland { source: "Dashboard.qml" }` item mounts generated UI anywhere in the host scene.
  It installs with pkg-config (`solidqml`) and CMake (`find_package(SolidQml)`) files. See
  `examples/embed-host/`.
- **The other direction**: TSX can use hand-written QML. `import Badge from "./Badge.qml"` brings
  in one file; `import { Gauge } from "qml:Acme"` brings in a type from a QML module or plugin
  found on the import path (`--import-path`). See `examples/native/customqml.tsx` and
  `examples/legacy-plugin/`.

## Virtualization

A `<For>` whose rows are passive (plain elements, classes and text, with no handlers or refs) is
**certified** at compile time (`transpiler/src/emit/forCertify.ts`), and its repeater is emitted
with `virtualize: true`. At run time the repeater virtualizes only when its container is a
vertical, non-wrapping scroll column: it creates the rows near the viewport and recycles them as
the list scrolls. Otherwise it creates every row (an eager list). While an assistive technology is
active (`QAccessible::isActive()`), virtualization is switched off so that a screen reader sees
every row. Evidence: `tests/c23/run-virtual.sh`, `meson test for-identity-virtual`.

## Accessibility on demand

Native controls get their accessibility semantics from their Templates control; plain elements get
them from `role`/`aria-*`/`tabIndex` ([keyboard & accessibility](keyboard-accessibility.md)). A
plain element only pays for a flag when it is created (`QQuickItemPrivate::setAccessible`, a
private Qt API). Its accessible interface (role, name, state) is built by a factory in
`src/widgets/a11y.cpp` the first time an assistive technology asks for it, so an app with no
screen reader running pays almost nothing. Tests: `meson test widgets-a11y widgets-a11y-generic`.

## Where to read next

- [Getting started](getting-started.md): build, install, a first app.
- [Support matrix](support-matrix.md): what is supported, and what the transpiler accepts or
  rejects.
- [Known issues](known-issues.md).
