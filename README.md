<p align="center">
  <img src="assets/logo.png" width="132" alt="solid-qml.js logo">
</p>

<h1 align="center">solid-qml.js</h1>

<p align="center">
  <strong>Solid.js, rendered natively via QML/QtQuick — no browser.</strong>
</p>

<p align="center">
  <em>⚠️ Experimental / alpha — APIs, generated formats and commands still change without notice.</em>
</p>

---

## What it is

**solid-qml.js** transpiles [Solid.js](https://www.solidjs.com/) components (JSX + CSS) into
**native QtQuick**: a real scene graph, GPU-rendered through Qt — **not** a webview,
**not** an emulated DOM.

You write UI in TypeScript/JSX with Solid's reactive model (signals, effects, memos, control
flow) and style it with plain CSS. The transpiler follows your imports and emits **one `.qml`
file per component**, plus a stylesheet, running on top of a custom C++ CSS engine.

- **vs Electron** — no Chromium + Node bundled into every app. The runtime is Qt, the UI is a
  native scene graph, the footprint is a fraction.
- **vs React Native** — real native components, no asynchronous JS bridge between worlds. And
  the release target is compiling everything to **C++ (AOT)**, eliminating the interpreter.

## Highlights (real, working today)

- ⭐ **npm/node module imports running on Qt's V4 engine.** QML's biggest historical defect has
  always been that you can't import npm packages — we solved that. The **package's real code**
  is mirrored and genuinely executed on the V4 engine (not a reimplementation, not a stub);
  every package that breaks reveals a V4 hole that we then plug.
- **Custom C++ CSS engine** doing **layout AND paint**: box model, **flexbox**, **grid**,
  `calc()`, `@media`, `vw`/`vh` units. No `QtQuick.Controls` — only lightweight primitives
  (Item / Text / TextInput / MouseArea / Repeater). The hot path (relayout) is C++ with zero
  abstraction overhead.
- **Web fonts via remote `@font-face`** — downloads the font, caches it and registers it with
  `QFontDatabase` at runtime. Installs nothing on the system.
- **Solid reactivity → QML**: `createSignal`, `createEffect`, `createMemo`, `createResource` +
  `<Suspense>`, `<Show>`, `<Switch>`/`<Match>` (lazy), `<For>`, `<Index>`.
- **Browser shims over V4**: `fetch` (real HTTPS via `QNetworkAccessManager`, with
  `Headers`/`Request`/`Response`/`AbortController`), `localStorage` (persistent),
  `XMLHttpRequest`, timers.
- **Responsive** — layout reflows on window resize; `@media`, `vw`, `vh` re-evaluate live.

## Quickstart

Requirements: Node.js + npm, **Qt 6** (Core, Gui, Qml, Quick, Test), **Meson** and **Ninja**.

```sh
# 1. JS dependencies
npm install

# 2. build the native loader (the C++ CSS engine is vendored under subprojects/)
meson setup build
ninja -C build

# 3. native dev: transpiles src/mainqml.tsx → QML and opens the native window with hot reload
node --import tsx scripts/dev-native.mjs
```

The dev loop watches `src/` and `examples/`, regenerates the QML on every change, and the loader
reloads the scene live (edit the CSS and watch the restyle happen instantly).

Other useful commands (see `package.json`):

```sh
npm run test:transpiler   # transpiler suite (node --test)
npm run dev               # web preview via Vite (Solid running in the browser)
```

## Roadmap

- **A single codebase for every platform** — apps for **Android, iOS, Windows, macOS and
  Linux** (mobile **and** desktop) from the same Solid code.
- **AOT → C++ for release** — our generator emits 1:1 QtQuick C++, eliminating interpreted V4;
  Qt's V4-AOT covers only the residual dynamic JS (async/fetch/closures).
- **Real native C++ interop** alongside Solid components.
- **Importing real QML components** (interop / legacy code).
- **Decent packaging** for deployment, and a path to **exporting into legacy projects**.

## Gaps / Current limitations

We are in alpha and insist on being honest about what **does not exist yet** or is only partial:

- **Charts & 3D need optional Qt modules.** `<Chart>`/`<Surface>` (QtGraphs) and `<Scene3D>`
  (Qt Quick 3D) are **opt-in per-widget modules** — the core engine never depends on them. You must
  install `qt6-graphs` and `qt6-quick3d` yourself; an app that doesn't use those tags pulls nothing.
  QtCharts is **not** used (it crashes offscreen — we use QtGraphs). Complex charts only: bar charts
  are drawn by the CSS engine, not QtGraphs.
- **3D renders only on a real GPU surface.** Qt Quick 3D can't render under the headless/`offscreen`
  platform, so 3D views can't be verified in CI screenshots — only that they load.
- **Missing desktop integration**: D-Bus, Avahi/zeroconf, deeper system-tray, not yet.
- **Threads** and **background services (mobile)**, not yet. **Gestures** (touch), not yet.
- `<For>` is still fragile with **nested sub-JSX** and complex cases.
- **Advanced CSS** mappings depend on open design decisions.
- **Menu keyboard activation** (Alt-mnemonics / F10) relies on Qt's built-ins and needs live checking
  on the target desktop.
- **AOT → C++ not implemented yet** — today dev runs on interpreted V4.
- **Packaging with Qt is painful** — the deployment story is still open.

Recently landed (no longer gaps): desktop **overflow scrolling** (mouse wheel, draggable scrollbar,
focus-follows-scroll) on both `div`/`CssRect` and `CssFill` boxes; the **escape hatch** (importing a
hand-written `.qml` as a component); the **keyboard tab-focus** model (ring, dialogs, default button).

## License

TBD (experimental).
