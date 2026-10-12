# __PRODUCT__

A [solid-qml](https://github.com/caetanus/solid-qml.js) app: Solid.js + Vite for the browser
preview, and the same TSX/CSS rendered natively with Qt Quick.

| Command | What it does |
| --- | --- |
| `npm run dev` | Browser preview (Vite + Solid). |
| `npx solid-qml dev` | Native window with hot reload — edit `src/` and it reloads. |
| `npx solid-qml build --target linux` | `dist/linux/<App>-x86_64.AppImage` + a portable `.tar.gz`. |
| `npx solid-qml doctor` | Checks the native runtime, system libraries and packaging tools. |

- `src/main.tsx` is the entry for both targets: its exported component renders `<Window>`.
- Static files go in `public/` (served by Vite, copied next to the generated QML natively — reference
  them as `assets/x.png` from `public/assets/x.png`).
- App identity comes from `package.json`: `productName`, `version`, and the `solidQml` block
  (`entry`, `appId`, `icon`).

No Qt or C++ toolchain is needed: the native runtime (Qt 6.11.2 + the solid-qml engine) arrives as
the `@solid-qml/runtime-<platform>` npm package.
