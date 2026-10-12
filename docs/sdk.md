# The solid-qml SDK (npm) — developer flow

> **Preview, not yet published to npm.** The packages below exist in this repo (`packages/`) and
> are verified end to end from local tarballs (`scripts/sdk/e2e-linux.sh`), but they are not on the
> public registry until the owner publishes them. Until then, install from tarballs — see
> [Installing from tarballs](#installing-from-tarballs-before-the-npm-release).

With only **Node.js ≥ 20.19** (Node 26 is what CI uses) — no Qt, no C++ compiler:

```sh
npm create solid-qml@latest my-app     # or: npx create-solid-qml my-app
cd my-app && npm install
npm run dev                            # browser preview: Vite + Solid
npx solid-qml dev                      # native window (Qt Quick) with hot reload
npx solid-qml build --target linux     # dist/linux/<App>-x86_64.AppImage + .tar.gz
npx solid-qml doctor                   # runtime / system libraries / packaging tools check
```

Platforms in this preview: **building and running on Linux x86_64 (glibc ≥ 2.34)**. Android,
Windows and macOS follow (see [RELEASE-PLAN.md](RELEASE-PLAN.md#sdk-npm-package--phase-1-linux)).

## The packages

| Package | What it is |
| --- | --- |
| [`solid-qml`](../packages/solid-qml) | The CLI (`solid-qml create\|dev\|build\|doctor`), the Vite plugin `solid-qml/vite`, the transpiler (TSX → QML, bundled), and the authoring module apps import: `import { Window } from "solid-qml"` plus the JSX typings (`<text>`, `<Window>`, native widgets). Pure JS. |
| [`create-solid-qml`](../packages/create-solid-qml) | `npm create solid-qml` — the starter template (Vite + vite-plugin-solid + solid-qml, a counter with CSS). |
| [`@solid-qml/runtime-linux-x64`](../packages/runtime-linux-x64) | The prebuilt native runtime: `solid-qml-loader`, `libsolidqml` (engine + widgets + shims) and the Qt 6.11.2 runtime it needs. An `optionalDependency` of `solid-qml` with `os`/`cpu`/`libc` fields, so npm installs it only on Linux x64 (the esbuild pattern). |

All three share one version (`0.3.0-preview.1`, checked by `npm run check:version`); `solid-qml`
pins its runtime to exactly that version.

## The app

```
my-app/
  package.json        name, version, productName + a "solidQml" block (entry, appId, icon)
  index.html          the web page (Vite)
  vite.config.ts      plugins: [solidQml(), solid()]
  src/main.tsx        THE entry for both targets: exports the component that renders <Window>,
                      and calls render() for the browser (the native target ignores that call)
  src/app.css         plain CSS — the same sheet styles the web page and the native window
  public/             static files: served by Vite, copied next to the generated QML natively
```

`package.json` drives packaging:

```jsonc
{
  "name": "my-app",
  "productName": "My App",          // window/desktop name, AppImage file name
  "version": "0.1.0",               // AppImage/tarball version
  "solidQml": {
    "entry": "src/main.tsx",         // default: src/main.tsx, then src/App.tsx
    "appId": "my-app",               // data dir (localStorage), .desktop name, Wayland app_id
    "icon": "public/icon.svg",       // .svg or .png
    "categories": "Utility;"         // freedesktop Categories
  }
}
```

First cut: apps are **TSX/CSS/assets** (plus npm packages, mirrored onto the V4 engine as usual,
and the runtime's widgets/shims). User C++ modules come later.

## Commands

### `npm run dev` — browser preview

Plain Vite. `solid-qml/vite` maps the QML-flavoured surface onto the DOM (`onClicked`, the
`<text>` primitive → `<span>`, `import X from "./X.qml"` placeholders) and keeps the `solid-qml`
package (shipped as TSX) out of esbuild's pre-bundling so `vite-plugin-solid` compiles it.

### `npx solid-qml dev` — native window, hot reload

Transpiles the entry (following its imports: one `.qml` per component, mirrored npm modules, the
CSS sidecar) into `.solid-qml/dev/`, starts the runtime's loader with `--watch`, and regenerates on
every change under `src/`, `public/`, `assets/` — the loader reloads the live window (CSS restyles
in place). Options:

- `--grab out.png` — render one frame and exit (headless with `QT_QPA_PLATFORM=offscreen`).
- `-- <loader args>` — passed to `solid-qml-loader` (e.g. `-- --width 800 --capabilities trusted`).

### `npx solid-qml build --target linux`

Packaging is assembly, not compilation — nothing is compiled on your machine:

1. transpile into `usr/share/<appId>/` of an AppDir (`.solid-qml/build/linux/…`);
2. copy the runtime payload into `usr/lib/solid-qml/`;
3. write `AppRun` (runs the loader with `--app-name <appId>`), the `.desktop` file and the icon;
4. `dist/linux/<Product>-<version>-linux-x86_64.tar.gz` — the AppDir as a portable directory
   (run `./AppRun`);
5. `dist/linux/<Product>-x86_64.AppImage` — via **appimagetool 1.9.1** and the static **type-2
   runtime 20251108**, both downloaded once into `~/.cache/solid-qml/tools` and verified by
   SHA-256 (pinned in `packages/solid-qml/lib/tools.mjs`). appimagetool runs with
   `--appimage-extract-and-run`, so the build host needs no FUSE.

The AppImage runs on Linux x86_64 with glibc ≥ 2.34. Without FUSE (containers), run it with
`APPIMAGE_EXTRACT_AND_RUN=1` or `--appimage-extract-and-run`.

`--out-dir <dir>` changes the output directory; `--no-appimage` writes only the tarball.
Building for Linux from another host: `npm i -D @solid-qml/runtime-linux-x64 --force`.

### `npx solid-qml doctor`

Reports Node and glibc, the app's entry/appId, the runtime package (version, Qt, build image,
commits, glibc floor), unresolved shared libraries (`ldd` of the loader and the platform plugins
through the launcher's own library setup), which desktop libraries come from the runtime's
fallbacks, and the packaging tools.

## The runtime payload (Linux)

Built on **manylinux_2_34** (AlmaLinux 9, glibc 2.34, GCC 14) against the **official Qt 6.11.2
binaries** (aqtinstall) — `scripts/sdk/build-runtime-linux.sh` builds the tree in that container,
runs `meson test` offscreen, installs, and `scripts/sdk/assemble-runtime-linux.sh` collects:

- `solid-qml-run` — the launcher: `runtime/lib` on `LD_LIBRARY_PATH`, plus
  `runtime/lib/fallback/<soname>/` **only for sonames the host lacks** (a desktop keeps its own
  GL/fontconfig/X11/wayland stack; a bare machine still starts), a bundled font configuration
  only when the host has no fonts, and host `QT_PLUGIN_PATH`/`QML2_IMPORT_PATH` cleared so a
  system Qt can't inject foreign plugins;
- `bin/solid-qml-loader` (RUNPATH `$ORIGIN/../lib`) + `qt.conf`;
- `lib/` — libsolidqml and the Qt library closure (incl. ICU), each stored once under its SONAME
  (npm tarballs cannot carry symlinks);
- `plugins/` — platforms (xcb, wayland, offscreen, minimal), wayland client integrations,
  xcbglintegrations, imageformats, iconengines, tls, networkinformation, input contexts, the
  xdg-desktop-portal theme (the GTK3 theme is dropped: it needs the host's GTK);
- `qml/` — QtQuick (+ Templates, Shapes, Effects, Window, Layouts), QtQml, QtCore, QtNetwork,
  Qt.labs (platform, qmlmodels, settings, folderlistmodel, animation). Not QtQuick.Controls/Dialogs:
  the widgets are QtQuick.Templates + the CSS engine;
- `lib/fallback/<soname>/` — ~66 desktop system libraries from AlmaLinux 9 (GL/EGL dispatch,
  fontconfig/freetype/harfbuzz, X11/xcb, wayland, dbus, glib, OpenSSL 3, …);
- `fonts/` — Noto Sans + `fonts.conf`, used only on hosts with no fonts;
- `licenses/` + `NOTICE.md` — MIT (solid-qml), LGPL-3.0 (engine, Qt) with relinking notes, Qt SBOMs,
  and the license of every bundled system package.

Size (0.3.0-preview.1): **126 MB** installed, **48 MB** as the npm tarball; the template app's
AppImage is **45 MB** (tarball 48 MB). The opt-in viz/media modules (`<Chart>`, `<Surface>`,
`<Scene3D>`, `<WebView>`, `<MediaPlayer>`) need Qt modules this runtime does not ship yet
(QtGraphs, QtQuick3D, QtWebEngine, QtMultimedia) — separate runtime add-on packages later.

`runtime.json` records the versions (solid-qml, Qt, glibc/GLIBCXX floors, build image, commits) and
the library lists. Host requirements: glibc ≥ 2.34 and libstdc++ (GCC ≥ 11) — everything else is
either bundled or a fallback.

## Installing from tarballs (before the npm release)

In a checkout of this repo:

```sh
npm ci
scripts/sdk/build-runtime-linux.sh     # podman, manylinux_2_34 (-j1 behind a RAM gate; incremental reruns)
npm run sdk:pack -- ./sdk-tarballs     # create-solid-qml, solid-qml, runtime-linux-x64 .tgz
```

Then, anywhere:

```sh
npx --package=./sdk-tarballs/create-solid-qml-0.3.0-preview.1.tgz create-solid-qml my-app
cd my-app
npm install -D ../sdk-tarballs/solid-qml-0.3.0-preview.1.tgz \
  ../sdk-tarballs/create-solid-qml-0.3.0-preview.1.tgz \
  ../sdk-tarballs/solid-qml-runtime-linux-x64-0.3.0-preview.1.tgz
```

(`npm install` then pulls the template's other dependencies — solid-js, vite, vite-plugin-solid —
from the public registry.) Serving the tarballs from a local registry (e.g. verdaccio) makes the
commands exactly `npm create solid-qml@latest …` / `npm install`.

## End-to-end test

`scripts/sdk/e2e-linux.sh` (podman; needs the runtime payload assembled) packs the three
packages and runs the whole flow on clean `node:26-bookworm-slim` containers (glibc 2.36 — no Qt,
no compiler, no fonts, no GL, no `file`): create (tarball form) → install → `npm run dev` (page and
compiled entry served) → `doctor` → `solid-qml dev` (a native frame grabbed offscreen, then an
edit hot-reloaded into the live window) → `build --target linux` → the AppImage (no FUSE) clicked
and grabbed, and the tarball's `AppRun` grabbed, in a fresh container. One container at a time
(`--memory` 1–1.5 GB), gated on host RAM. Transcript and screenshots: `docs/evidence/sdk-linux/`.

## Working on the SDK in this repo

- The packages are npm workspaces (`packages/*`); `npm install` at the root links them.
- `npm run sdk:build` materialises the gitignored payloads of `solid-qml` (`dist/transpiler.mjs`,
  `runtime/`) from `transpiler/src` and `src/solid-qml` — the single sources of truth.
- `SOLID_QML_RUNTIME_DIR=<dir with runtime.json>` points the CLI at a locally assembled runtime.
