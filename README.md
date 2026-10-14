<p align="center">
  <img src="assets/logo.png" width="132" alt="solid-qml logo">
</p>

<h1 align="center">solid-qml</h1>

<p align="center">
  <strong>Write it like the web. Ship it native.</strong><br>
  Solid.js components (TSX + CSS) rendered by Qt Quick: no browser, no webview, no DOM.
</p>

<p align="center">
  <a href="docs/sdk.md">SDK</a> ·
  <a href="docs/getting-started.md">Getting started</a> ·
  <a href="docs/architecture.md">Architecture</a> ·
  <a href="docs/support-matrix.md">Support matrix</a> ·
  <a href="docs/performance.md">Performance</a> ·
  <a href="ROADMAP.md">Roadmap</a>
</p>

<p align="center">
  <em>0.3.0-preview.1 — developer preview. APIs, generated output and commands may still change.</em>
</p>

---

## Why

Every Electron app ships its own Chromium. React Native gives you native components, but it was
built for phones, and on the desktop each platform is a separate project (Linux has no official
support). You end up choosing between *feels like the web* and *feels native*.

solid-qml refuses that trade-off:

- **The web's developer experience:** TSX, CSS, Vite, hot reload, npm packages.
- **Real native UI:** Qt Quick's GPU scene graph, system menus, dialogs that are real windows,
  tray, notifications, keyboard focus and accessibility from the platform.
- **No browser inside:** one app, one process, one runtime.
- **One codebase, two targets:** the same `src/main.tsx` runs in the browser (Vite + Solid) and as
  a native app.

## Hello, native

```tsx
import { createSignal } from "solid-js";
import "./app.css";

export default function Counter() {
  const [count, setCount] = createSignal(0);
  return (
    <div class="card">
      <h1>Clicked {count()} times</h1>
      <button onClick={() => setCount(count() + 1)}>Click me</button>
    </div>
  );
}
```

That is ordinary Solid. solid-qml compiles it to Qt Quick: `count` becomes a reactive QML
property, the CSS is laid out and painted by a C++ engine, and `<button>` is a native control
styled by your stylesheet.

## Quick start

Only **Node.js** is needed: no Qt, no C++ toolchain. The native runtime arrives as an npm package.
Linux x86_64 (glibc ≥ 2.34) in this preview:

```sh
npm create solid-qml@latest my-app
cd my-app && npm install
npm run dev                            # browser preview (Vite + Solid)
npx solid-qml dev                      # native Qt Quick window, hot reload
npx solid-qml build --target linux     # dist/linux/<App>-x86_64.AppImage + .tar.gz
```

> **Preview: not yet published to npm.** Until the packages are published, install them from
> tarballs built from this repo: see [docs/sdk.md](docs/sdk.md#installing-from-tarballs-before-the-npm-release).

## Built with solid-qml: SolidTerm

[`apps/solidterm`](apps/solidterm) is a GPU-accelerated terminal used daily as its author's main
terminal. Its whole UI (tabs, splits, drag-and-drop between windows, preferences, search, menus,
tmux integration) is Solid TSX and CSS. Only the terminal core, the pty and the GPU glyph renderer
are C++.

Throughput against Tilix (VTE) on the same machine, median of 5 runs
([method and raw data](docs/evidence/solidterm/parser-thread-2026-10-13.md)):

| Test | SolidTerm | Tilix 0.9 |
|---|---:|---:|
| `seq 1 5000000` | **2.74 s** | 3.02 s |
| `cat` 100 MB mixed text | **3.42 s** | 3.85 s |
| Dense cells, 50 MB | **0.49 s** | 2.25 s |
| Scrolling region, 50 MB | **1.34 s** | 2.06 s |

## How it works

```
 your app (TSX + CSS)
        │
        ▼
 ┌──────────────┐   one .qml per component; signals → reactive properties,
 │  transpiler  │   <For>/<Show>/<Switch> → native repeaters and lazy items,
 └──────┬───────┘   unsupported constructs → compile errors, not runtime surprises
        ▼
 ┌──────────────┐   cascade, layout (block · flex · grid · absolute) and paint, in C++
 │  CSS engine  │
 └──────┬───────┘
        ▼
 ┌──────────────┐   <button> <input> <select> <dialog> menus tabs lists tables trees …
 │   widgets    │   built on Qt's own controls, styled by your CSS
 └──────┬───────┘
        ▼
 ┌──────────────┐   fetch · localStorage · timers · Workers · pure-JS npm packages
 │   runtime    │   on Qt's V4 JavaScript engine
 └──────────────┘
```

- **Transpiler** (`transpiler/`, TypeScript): follows your imports and emits one `.qml` file per
  component. What it cannot translate fails at compile time with a message; the few semantic
  differences that remain are listed in the [support matrix](docs/support-matrix.md).
- **C++ CSS engine** (`vendor/qml-css-engine`): measured against the same views in a browser at
  milestone 1 (tag `v0.1.0-alpha.1`), all 16 gallery views reached ≥ 96% grid-cell match, 99.2%
  on average.
- **Widgets** (`src/widgets`, C++): 44 native types, mostly on `QtQuick.Templates`. Heavier
  widgets (`<RichText>`/`<DocumentEditor>`, `<CodeEditor>`, `<MediaPlayer>`, `<WebView>`, charts,
  3D) are opt-in modules, loaded only when an app uses the tag.
- **Desktop integration:** system tray, notifications with inline reply (D-Bus), context menus,
  accelerators, mnemonics, real `<dialog>` windows, theme icons.
- **Loader and embed library:** run a generated app on its own (`solid-qml-loader`), or put
  solid-qml UI inside an existing Qt Quick application (`libsolidqml`, with pkg-config and CMake
  support).

## Performance, honestly

[docs/performance.md](docs/performance.md) is the only source for performance claims, and it
lists the losses next to the wins.

- **Wins:** in a 1,000-row js-framework-benchmark-style list, the automatically virtualized native
  list completes all six operations **4–15× faster** than the same Solid component in headless
  Chromium (6.8–16× at 10,000 rows).
- **Still behind:** non-scrolling (eager) lists and per-row memory.
- **Not measured yet:** footprint and startup versus a packaged Electron app.

## Status and roadmap

| | |
|---|---|
| **Linux x86_64** | ✅ supported: npm SDK (runtime bundled) or built from source |
| **Windows** | 🟡 builds and passes its tests on CI; not in this release |
| **macOS** | 🟡 CI port in progress |
| **Android** | 🔜 next SDK target |
| **AOT → C++** | 🧪 the generator emits C++ from TSX; first component renders pixel-identical to the interpreted one |

The goal for release builds: your components compiled to C++, with the JS engine kept only for
the dynamic code that needs it. See the [roadmap](ROADMAP.md) and the
[release plan](docs/RELEASE-PLAN.md).

## Documentation

| Document | Contents |
|---|---|
| [SDK](docs/sdk.md) | `npm create solid-qml`, dev, build and packaging |
| [Getting started](docs/getting-started.md) | Dependencies, build, tests, install, your first app outside the checkout |
| [Architecture](docs/architecture.md) | How the transpiler, CSS engine, widgets, shims, loader and embed library fit together |
| [Support matrix](docs/support-matrix.md) | What is supported, partial or unsupported: Solid APIs, JSX, elements, CSS, shims, platforms |
| [Known issues](docs/known-issues.md) | Open defects and limits, each with a workaround where there is one |
| [Keyboard & accessibility](docs/keyboard-accessibility.md) | How `tabIndex`/`role`/`aria-*` map to Qt Quick |
| [Document editor](docs/document-editor.md) | The `<DocumentEditor>` / `<RichText>` API |
| [Solid parity](SOLID-PARITY.md) | Solid API coverage and the `<For>` row-identity contract |
| [Release notes](docs/release-notes/0.3.0-preview.1.md), [release policy](docs/release-policy.md) | What is in this preview; versioning and the SONAME/ABI policy |

## Working on the framework (from source)

Requirements: Node.js + npm, **Qt 6** (Core, Gui, Qml, Quick, Test, + private headers for Qml/Quick/
QuickTemplates2), **CMake**, **Meson** (≥1.1) and **Ninja**, plus a system font (e.g. `noto-fonts`
on Arch). See [docs/getting-started.md](docs/getting-started.md) for the full, verified dependency
list.

```sh
git clone --recurse-submodules https://github.com/caetanus/solid-qml.js.git
cd solid-qml.js
npm ci
meson setup build
ninja -C build
node --import tsx scripts/dev-native.mjs   # transpile src/mainqml.tsx, open it, reload on save
```

## Contributing

It's a preview, so the most valuable contribution is a real app and a report of where it broke.
Open an issue with the smallest TSX/CSS that reproduces it; if a construct isn't supported, the
compile error tells you which one.

## License

- The framework (transpiler, widgets, loader, embed library, shims) is **MIT**. See [LICENSE](LICENSE).
- The C++ CSS engine in `vendor/qml-css-engine` is **LGPL-3.0-only**: part of its text layout is
  derived from Qt's `QQuickText` (see its `NOTICE`).
- Everything links against Qt, which is available under the LGPL-3.0. An application built with
  solid-qml must meet Qt's license terms (and the engine's LGPL-3.0 terms) when it is distributed.
