# @solid-qml/runtime-linux-x64

The prebuilt native runtime of [solid-qml](https://github.com/caetanus/solid-qml.js) for Linux
x86_64. You don't install this directly: it is an optional dependency of `solid-qml`, selected by
npm for Linux x64 hosts (`os`/`cpu`/`libc`). To package for Linux from another host:
`npm i -D @solid-qml/runtime-linux-x64 --force`.

Contents (`runtime/`):

- `solid-qml-run` — the launcher (sets up the bundled library path and, only when the host lacks
  them, the bundled fallback copies of desktop system libraries and a font).
- `bin/solid-qml-loader`, `lib/libsolidqml.so.0` — the solid-qml engine, widgets and shims.
- `lib/`, `plugins/`, `qml/` — the Qt 6.11.2 runtime (official binaries, shared libraries, LGPLv3).
- `lib/fallback/<soname>/` — system libraries used only when the host doesn't provide them.
- `licenses/` — license texts (Qt LGPLv3/GPL, the qml-css-engine LGPLv3, bundled system libraries).

`runtime.json` records the versions (solid-qml, Qt, glibc floor, build image, commits).

Built on manylinux_2_34 (AlmaLinux 9): runs on glibc >= 2.34 (Debian 12, Ubuntu 22.04, Fedora 35,
RHEL 9 and newer). See NOTICE.md for licensing and how to relink against your own Qt.
