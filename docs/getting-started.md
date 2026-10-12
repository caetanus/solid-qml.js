# Getting started

This is the preview (0.x) install story: build the framework from source, run its test suite, and
author a new app **outside** the checkout against the installed artifacts.

## CI / reference environment

The project's CI (release-plan M7, [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) and
the authoritative reference environment for the 0.x preview is:

- **Linux x86_64, glibc >= 2.34** — container image `quay.io/pypa/manylinux_2_34_x86_64`
  (AlmaLinux 9). This matches Qt 6.11/6.12's own RHEL-9-family reference platform (glibc 2.34,
  GCC 11+) more closely than a rolling-release distro can be pinned to, and ships GCC 14.2.1 on
  PATH with full C++20 support out of the box.
- **Qt 6.11.2**, installed from its *official* Linux x64 binaries via
  [aqtinstall](https://github.com/miurahr/aqtinstall) (`aqt install-qt linux desktop 6.11.2
  linux_gcc_64 -m qtshadertools`) — not distro packages. This is also the most reliable way to get
  the *private* Qt headers this project's engine and widgets need
  (`QtQuickTemplates2/private`, `QtQuick/private`, …): aqtinstall's binaries ship them unconditionally,
  where distro packaging varies (Arch keeps them with the main package; Debian splits them out or
  omits a CMake config for them — see release-plan M7 notes).
- **Node 26.8.2**, the official linux-x64 tarball (checksum-pinned), meson 1.12.0, ninja 1.13.0,
  aqtinstall 3.3.0.

[`scripts/clean-room-check.sh --manylinux`](../scripts/clean-room-check.sh) automates the exact
same provider steps as CI inside a local podman container — run it to reproduce a CI failure
locally, or `--manylinux --local` to validate uncommitted changes in this worktree. Verified
end to end on 2026-10-11 (see release-plan M7 notes for the run's timings and Qt download size).

## Requirements

### Arch / Manjaro (supported local dev-machine path, verified)

This is the easiest path for day-to-day development on an Arch/Manjaro machine (e.g. the author's
own), and is what [`scripts/clean-room-check.sh`](../scripts/clean-room-check.sh) verifies by
default (no flag). It is **not** the CI/reference environment above — the dependency versions here
track whatever Arch's pinned archive snapshot carries, which can drift from the exact Qt 6.11.2
reference; CI is the environment to trust for "does this actually work on the pinned preview
stack."

```sh
sudo pacman -S base-devel git cmake meson ninja nodejs npm \
               qt6-base qt6-declarative qt6-svg \
               libvterm wayland-protocols \
               noto-fonts noto-fonts-emoji
```

| Package | Why |
|---|---|
| `base-devel` | gcc/g++, make, pkgconf — the C++ toolchain. |
| `git` | clones the repo **and** the `vendor/qml-css-engine` submodule. |
| `cmake` | **not used to build anything** — meson's `cmake` module shells out to the real `cmake` binary to generate the installed CMake package config (`SolidQmlConfig.cmake`, via `cmake.write_basic_package_version_file`). Omit it and `meson setup` fails outright with `ERROR: Program 'cmake' not found` (first failure hit in the clean-room run). |
| `meson` ≥ 1.1 | the build system (`license_files` in `meson.build` needs ≥ 1.1; this repo is tested on 1.12). |
| `ninja` | meson's backend. |
| `nodejs`, `npm` | the transpiler and its test suite. Tested on Node 26; `vite@7` (a dependency) requires Node `^20.19 \|\| >=22.12`, so anything from 20.19 up should work, untested below that. |
| `qt6-base` | Core, Gui, Widgets, Network, DBus, Test — **and their private headers** (Arch does not split `-dev`/private-header packages; other distros may). |
| `qt6-declarative` | Qml, QmlModels, Quick, QuickTemplates2 — **and their private headers**. `TabButton`/`Menu`/`MenuItem`/`MenuSeparator` subclass private `QtQuickTemplates2/private` classes (same precedent as the private `QZipReader` used by `<RichText>`'s ODF loader); `csstext.cpp` reads `QtQuick/private` for `LayoutMirroring`. |
| `qt6-svg` | runtime only: SVG icon rendering (`QtQuickVectorImage`/`svgtoqml`, pulled in automatically as a `qt6-declarative` optional dependency on Arch). Nothing in this repo links against it directly today. |
| `libvterm` | **SolidTerm** (`apps/solidterm`), built whenever `-Dsolidterm` is `enabled` or `auto` (default) and vterm is found. Pass `-Dsolidterm=disabled` to skip it and this dependency entirely. |
| `wayland-protocols` ≥ 1.45 (+ `wayland`) | optional: SolidTerm's background-blur window effect (`ext-background-effect-v1`). The build configures cleanly without them — blur is just unavailable. |
| `noto-fonts`, `noto-fonts-emoji` | **runtime**, not a build dependency — but without *any* system font the text engine's metrics are effectively undefined (`fc-list` on a bare `archlinux:latest` image returns **zero** fonts) and its pixel-oracle tests fail outright. This is what `fc-match sans-serif` resolves to on the project's own dev machine; install the same family rather than guessing (e.g. `ttf-dejavu` gives different metrics and still fails the oracle tests — see Troubleshooting). |

Optional, feature-gated (skip unless you use the feature):

- `qt6-graphs`, `qt6-quick3d` — `<Chart>`/`<Surface>` and `<Scene3D>`. Lazily imported QML modules,
  never linked by the C++ build; an app that doesn't use those tags pulls nothing in.
- `qt6-webengine` — `<WebView>`.
- `qt6-multimedia` — `<MediaPlayer>`.

### Debian / Ubuntu / Fedora

**Untested.** The package names below are a best-effort mapping, not verified on those distros —
treat them as a starting point, not a promise:

- Debian/Ubuntu: `build-essential git cmake meson ninja-build nodejs npm qt6-base-dev qt6-base-private-dev qt6-declarative-dev qt6-declarative-private-dev qt6-svg-dev libvterm-dev libwayland-dev wayland-protocols fonts-noto fonts-noto-color-emoji`
- Fedora: `@development-tools git cmake meson ninja-build nodejs npm qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel qt6-qtdeclarative-private-devel qt6-qtsvg-devel libvterm-devel wayland-devel wayland-protocols-devel google-noto-sans-fonts google-noto-emoji-fonts`

If you try one of these and it needs adjustment, please say so in an issue — this table is honest
about being unverified, not a guess dressed up as fact.

## Build and test

```sh
git clone --recurse-submodules https://github.com/caetanus/solid-qml.js.git
cd solid-qml.js

npm ci
meson setup build
ninja -C build -j$(nproc)
QT_QPA_PLATFORM=offscreen meson test -C build
```

`qml-css-engine:text` is a strict pixel/metric oracle comparing the CSS engine's text layout
against a live `QQuickText`. It is sensitive to the *exact* installed font and HarfBuzz version and
is known to drift on a rolling-release distro's "latest" packages even with the right font family
installed — this is a known limitation of exact cross-machine reproducibility for that one suite,
not a missing dependency; every other suite (13/14, including the transpiler's own 580+ tests) is
expected green.

## Install

```sh
meson install -C build --destdir /opt/solidqml   # or --prefix /opt/solidqml, your choice
```

This installs: `libsolidqml.so` + the `qmlcss`/`solidqml` headers + `.pc` pkg-config files +
`SolidQmlConfig.cmake` (for `find_package(SolidQml)`), the `solid-qml-loader` executable, and (if
`-Dsolidterm` built) the `solidterm` executable plus its desktop entry/icon/generated UI under
`share/solidterm`. For writing apps you don't need any of this: the npm SDK (`npm create solid-qml`,
[sdk.md](sdk.md)) ships the transpiler and a prebuilt runtime. The recipe below is the from-source
path — a new app authored and transpiled against a cloned checkout and an installed build.

## Your first app outside the checkout

> **Use the SDK for new apps:** `npm create solid-qml@latest my-app` gives you a project that
> imports `Window` from the `solid-qml` npm package — no checkout paths, no Qt install — see
> [sdk.md](sdk.md) (preview: install from tarballs until the packages are published). This section
> is the from-source equivalent, kept for framework development and the installed-artifact CI smoke.

This is the part that was previously impossible without `cd`-ing into the framework checkout — a
clean-room run on 2026-10-11 hit two real bugs on the way to making it work, both fixed upstream in
`transpiler/src/babel/transform.ts` (see git log). The steps below are what actually works today,
not a workaround.

Make a new directory *anywhere* — it does not need to be inside, next to, or related to the
checkout on disk:

```sh
mkdir -p ~/my-first-solid-qml-app && cd ~/my-first-solid-qml-app
```

`counter.tsx` (swap `CHECKOUT` for the path where you cloned the framework):

```tsx
import { createSignal } from "solid-js";
import { div, text, button, Window } from "CHECKOUT/src/solid-qml/runtime";
import "./counter.css";

function Counter() {
  const [count, setCount] = createSignal(0);
  return (
    <div class="counter">
      <text>count: {count()}</text>
      <button onClick={() => setCount(count() + 1)}>increment</button>
    </div>
  );
}

export function App() {
  return (
    <Window title="my-first-solid-qml-app" width={320} height={220} visible>
      <div class="app">
        <Counter />
      </div>
    </Window>
  );
}
```

`counter.css`:

```css
.qml-window { background: #1e2233; color: #fff; font-size: 16px; }
.app { display: flex; flex-direction: column; gap: 16px; padding: 24px; }
.counter button { background: #41cd52; color: #fff; padding: 10px 18px; border-radius: 8px; }
```

The exported component (`App`) is picked up automatically as the entry — there's no `render(...)`
boilerplate to write for the native target (that's a web-preview-only concern).

**Why `CHECKOUT/src/solid-qml/runtime` and not an npm import:** there is no published
`solid-qml` npm package yet (release-plan, LATER). `Window`/`div`/`text`/`button` are plain
Solid components the transpiler recognizes structurally (by the `data-qml-type` marker on their
JSX); importing them by absolute path from the checkout works today and is the documented path for
this preview. Packaging them for `npm install` is tracked separately.

Transpile (TSX → QML) against the checkout's transpiler, using an **absolute path** to both the
`tsx` loader and the generator script:

```sh
node --import CHECKOUT/node_modules/tsx/dist/loader.mjs \
  CHECKOUT/scripts/gen.mjs ./counter.tsx ./qmlout/App.generated.qml
```

Do **not** write `--import tsx` (bare specifier) here: Node resolves a bare `--import` specifier
starting from the *invoking file's own directory*, which is this new app, not the checkout — it
fails with `ERR_MODULE_NOT_FOUND: Cannot find package 'tsx'`. The absolute path sidesteps that.
(There is no installed `solid-qml` CLI/npm bin yet either; this is the documented path until one
ships.)

Run it with the **installed** loader (no checkout needed at runtime):

```sh
LD_LIBRARY_PATH=/opt/solidqml/usr/local/lib \
  /opt/solidqml/usr/local/bin/solid-qml-loader --qml ./qmlout/App.generated.qml
```

Without `--css`, the loader loads the stylesheet the generator wrote next to the entry
(`qmlout/App.generated.css` here), on top of the framework's built-in base sheet.

Add `QT_QPA_PLATFORM=offscreen --grab shot.png` to render one frame to a PNG headlessly (what
`scripts/clean-room-check.sh` does) instead of opening a window. To test interaction headlessly,
add `--click x,y,delayMs` (repeatable, window coordinates): it sends a real mouse click before the
frame is grabbed. There is no equivalent for keyboard input yet. `solid-qml-loader --help` lists
every option.

## Native dev loop (inside the checkout)

```sh
node --import tsx scripts/dev-native.mjs
```

Transpiles `src/mainqml.tsx` into QML (`qml/solidqml/App.generated.qml` + one `.qml` per component
+ `App.generated.css`) and opens the native window with hot reload: the loop watches `src/` and
`examples/`, regenerates the QML on every change, and the loader reloads the scene live.

```sh
npm run test:transpiler   # transpiler suite (node --test)
npm run dev               # web preview via Vite (Solid running in the browser)
```

See `package.json` for the current list of scripts.

## Troubleshooting

- **`meson setup` fails with `ERROR: Program 'cmake' not found`** — install `cmake` (see
  Requirements; it is a real build dependency, not building-with-CMake).
- **`qml-css-engine:text` fails in `meson test`** — known font/HarfBuzz-version sensitivity, see
  above. Confirm `fc-match sans-serif` resolves to Noto Sans; if it doesn't, install `noto-fonts`.
  Every other suite failing alongside it is a real regression, not this.
- **`Cannot find package 'tsx' imported from <your-app>`** — you ran `node --import tsx ...` with a
  bare specifier from outside the checkout. Use the absolute path to
  `CHECKOUT/node_modules/tsx/dist/loader.mjs` instead (see "Your first app outside the checkout").
- **`libsolidqml.so: cannot open shared object file`** when running an installed `solid-qml-loader`
  — set `LD_LIBRARY_PATH` to the install prefix's `lib/` dir (or install to a prefix already on
  your loader search path, or run `ldconfig` against it).

## Next steps

- [Support matrix](support-matrix.md): which Solid APIs, JSX forms, elements, CSS and shims work,
  and which constructs the transpiler rejects. Check it before porting an existing app.
- [Known issues](known-issues.md): open defects and limits, with workarounds.
- [Architecture](architecture.md): how the pieces fit together.
