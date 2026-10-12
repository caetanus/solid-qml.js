<p align="center">
  <img src="assets/logo.png" width="132" alt="solid-qml logo">
</p>

<h1 align="center">solid-qml</h1>

<p align="center">
  <strong>Solid.js components rendered natively by Qt Quick, without a browser.</strong>
</p>

<p align="center">
  <em>0.3.0-preview.1: a developer preview. APIs, generated output and commands may still change.</em>
</p>

---

## What it is

solid-qml compiles [Solid.js](https://www.solidjs.com/) components (TSX + CSS) into Qt Quick. The
result is a real Qt Quick scene graph, not a webview and not an emulated DOM.

- **Transpiler** (`transpiler/`, TypeScript): follows your imports and emits one `.qml` file per
  component. Signals become QML properties, effects become change handlers, `<For>`/`<Show>`/
  `<Switch>` become repeaters and lazily created items. Most constructs it cannot translate are a
  compile error; the few that are not are listed in the [support matrix](docs/support-matrix.md).
- **C++ CSS engine** (`vendor/qml-css-engine`): resolves the cascade and does both **layout**
  (block, flex, grid, absolute positioning) and **paint**.
- **Widgets** (`src/widgets`, C++): `<button>`, `<input>`, `<select>`, `<dialog>`, menus, tabs,
  tree/list/table views and more, mostly built on `QtQuick.Templates` controls and styled by your
  CSS. Heavier widgets (`<RichText>`, `<CodeEditor>`, `<MediaPlayer>`, `<WebView>`, charts, 3D) are
  opt-in modules that an app only loads when it uses the tag.
- **Runtime shims** (`src/shims`): `fetch`, `localStorage`, timers, Workers and more, on top of
  Qt's V4 JavaScript engine. Pure-JS npm packages are mirrored and run as they are.
- **Loader and embed library**: run a generated app on its own (`solid-qml-loader`), or put
  solid-qml UI inside an existing Qt Quick application (`libsolidqml`).

## Status

This is a **developer preview**. See the [release notes](docs/release-notes/0.3.0-preview.1.md).

- Supported environment: **Linux x86_64 (glibc ≥ 2.34), Qt 6.11** — via the npm SDK (runtime
  bundled, nothing else to install) or built from source (Arch/Manjaro verified). Windows and macOS
  ports are in progress on CI branches (not in this release); Android is the next SDK target.
- Only part of Solid is covered. Check the [support matrix](docs/support-matrix.md) before you
  port an app: what is not supported fails at compile time with a message, and the few semantic
  differences that remain are listed in [known issues](docs/known-issues.md).
- Performance ([methodology and raw data](docs/performance.md)): in our 1,000-row scrolling-list
  benchmark the automatically virtualized native list beats the same Solid app in headless Chromium
  on all six js-framework-benchmark operations. Non-scrolling (eager) lists and per-row memory are
  still behind Chromium. Footprint and startup versus Electron are not measured yet.

## Documentation

| Document | Contents |
|---|---|
| [Getting started](docs/getting-started.md) | Dependencies, build, tests, install, your first app outside the checkout |
| [Architecture](docs/architecture.md) | How the transpiler, CSS engine, widgets, shims, loader and embed library fit together |
| [Support matrix](docs/support-matrix.md) | What is supported, partial or unsupported: Solid APIs, JSX, elements, CSS, shims, platforms |
| [Known issues](docs/known-issues.md) | Open defects and limits, each with a workaround where there is one |
| [Keyboard & accessibility](docs/keyboard-accessibility.md) | How `tabIndex`/`role`/`aria-*` map to Qt Quick |
| [Document editor](docs/document-editor.md) | The `<DocumentEditor>` / `<RichText>` API |
| [Solid parity](SOLID-PARITY.md) | Solid API coverage and the `<For>` row-identity contract |
| [Release policy](docs/release-policy.md) | Versioning and the SONAME/ABI policy |
| [Roadmap](ROADMAP.md), [release plan](docs/RELEASE-PLAN.md) | What comes next |

> **Preview — not yet published to npm.** Until the owner publishes the packages, install them
> from tarballs built from this repo: see [docs/sdk.md](docs/sdk.md#installing-from-tarballs-before-the-npm-release).

Only **Node.js** is needed — no Qt, no C++ toolchain (the native runtime arrives as an npm
package). Linux x86_64 (glibc ≥ 2.34) in this preview:

```sh
npm create solid-qml@latest my-app
cd my-app && npm install
npm run dev                            # browser preview (Vite + Solid)
npx solid-qml dev                      # native Qt Quick window, hot reload
npx solid-qml build --target linux     # dist/linux/<App>-x86_64.AppImage + .tar.gz
```

The same `src/main.tsx` and CSS drive both targets. Full developer flow: [docs/sdk.md](docs/sdk.md).

## Working on the framework (from source)

Requirements: Node.js + npm, **Qt 6** (Core, Gui, Qml, Quick, Test, + private headers for Qml/Quick/
QuickTemplates2), **CMake**, **Meson** (≥1.1) and **Ninja**, plus a system font (e.g. `noto-fonts`
on Arch) — see [docs/getting-started.md](docs/getting-started.md) for the full, verified dependency
list (clean-room tested on 2026-10-11).

```sh
git clone --recurse-submodules https://github.com/caetanus/solid-qml.js.git
cd solid-qml.js
npm ci
meson setup build
ninja -C build
node --import tsx scripts/dev-native.mjs   # transpile src/mainqml.tsx, open it, reload on save
```

## License

- The framework (transpiler, widgets, loader, embed library, shims) is **MIT**. See [LICENSE](LICENSE).
- The C++ CSS engine in `vendor/qml-css-engine` is **LGPL-3.0-only**: part of its text layout is
  derived from Qt's `QQuickText` (see its `NOTICE`).
- Everything links against Qt, which is available under the LGPL-3.0. An application built with
  solid-qml must meet Qt's license terms (and the engine's LGPL-3.0 terms) when it is distributed.
