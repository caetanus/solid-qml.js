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

- Supported environment: **Linux x86_64 with Qt 6.11**, built from source. Arch/Manjaro is
  verified. Other distros are untested. There is no npm package, binary SDK, Windows, macOS or
  mobile build yet.
- Only part of Solid is covered. Check the [support matrix](docs/support-matrix.md) before you
  port an app: a few APIs (`untrack`, `batch`, `on`…) compile today but are **wrong at runtime**.
- Performance: the methodology and numbers have not been published yet (release-plan M10). Until
  they are, this project makes no performance claims.

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

## Quickstart (inside the checkout)

The verified steps, including the package list and an app built outside the checkout, are in
[docs/getting-started.md](docs/getting-started.md).

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
