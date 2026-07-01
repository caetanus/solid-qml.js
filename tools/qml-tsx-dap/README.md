# Solid-QML TSX Debug — VS Code proxy

Debug your **`main.tsx`** in VS Code with real breakpoints, even though the app runs as
generated **QML** in Qt's V4 engine.

## How it works

```
VS Code ──DAP──> [this proxy, MIT] ──DAP──> qml-debug adapter (GPL, unmodified) ──V4 proto──> Qt engine
                  rewrites source locations
                  main.tsx <-> App.generated.qml
                  via App.generated.qml.map
```

The proxy is a transparent DAP passthrough that only rewrites the **source locations** that
cross it, using the Source Map v3 the transpiler emits (`App.generated.qml.map`):

- outbound `setBreakpoints`: `main.tsx:line` → `App.generated.qml:line`
- inbound `stackTrace` / `stopped` / `breakpoint` / `loadedSource` / `output`: back to `main.tsx:line`

It never copies qml-debug's code — it spawns qml-debug's standalone adapter as a child
process and talks DAP to it (arm's length), so this stays MIT while qml-debug stays GPL.

## Prerequisites

1. The **qml-debug** VS Code extension (`orcun-gokbulut.qml-debug`) — provides the QML
   protocol adapter the proxy drives. (Or build it and point `innerAdapter` at its
   `out/debug-adapter.js`.)
2. This extension loaded in VS Code (dev): open `tools/qml-tsx-dap` and run the Extension
   Development Host, or symlink it into `~/.vscode/extensions`. (Loads from the repo — the
   proxy imports `../../scripts/sourcemap.mjs`.)
3. The build with QML debugging compiled in (default): `meson setup build && ninja -C build`.

## Use

1. Generate fresh QML + map: `npm run generate:qml` (emits `App.generated.qml` + `.map`).
2. Run the app with the V4 debug server, loading the QML **from the filesystem** (so paths
   match `generatedQml`):
   ```
   ./build/solid-qml-loader \
     --qml qml/solidqml/App.generated.qml --css src/style.css \
     -qmljsdebugger=port:12150,block
   ```
   `block` makes it wait for the debugger before running, so early breakpoints bind.
3. In VS Code, run the **“Debug main.tsx (via QML)”** attach config (see
   `.vscode/launch.json`). Set breakpoints in `main.tsx` — they stop in your TSX.

## Limitations

- Line-level mapping: a breakpoint resolves to the generated **element/handler block**, so a
  stop inside a handler shows the element's TSX line (refine to token/column later).
- Variable/scope inspection shows the QML/JS runtime values (e.g. `window.events`), not TSX
  locals — those don't exist at runtime (the transpiler lowered them).
- If the app loads QML from `qrc:` instead of the filesystem, set `paths` so qml-debug maps
  `qrc:/…` to the on-disk file.
