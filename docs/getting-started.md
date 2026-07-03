# Getting started

## Requirements

- Node.js + npm
- **Qt 6** (Core, Gui, Qml, Quick, Test)
- **Meson** and **Ninja**

## Install and build

```sh
# 1. JS dependencies
npm install

# 2. build the native loader (the C++ CSS engine is vendored under subprojects/)
meson setup build
ninja -C build
```

## Native dev

```sh
node --import tsx scripts/dev-native.mjs
```

This transpiles `src/mainqml.tsx` into QML (`qml/solidqml/App.generated.qml` + one `.qml` per
component + `App.generated.css`) and opens the native window with hot reload: the loop watches
`src/` and `examples/`, regenerates the QML on every change, and the loader reloads the scene
live.

## Other commands

```sh
npm run test:transpiler   # transpiler suite (node --test)
npm run dev               # web preview via Vite (Solid running in the browser)
```

See `package.json` for the current list of scripts.
