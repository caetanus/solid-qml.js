import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import solid from "vite-plugin-solid";
import { solidQml } from "./src/solid-qml/vite-plugin";

// `vite --mode qmldev` opens the native QML window and hot-reloads it on .tsx/.css edits;
// the default mode keeps the browser preview.
export default defineConfig(({ mode }) => ({
  resolve: {
    // The authoring package name; in the browser it resolves to the runtime shim (the QML
    // target compiles the import away instead).
    alias: { "qml-solid": fileURLToPath(new URL("./src/solid-qml/runtime", import.meta.url)) },
  },
  plugins: [
    solidQml(
      mode === "qmldev"
        // Native entry: the only file with <Window>; it wraps <Gallery>. The transpiler follows its
        // imports and emits one QML per component.
        ? {
            target: "qml",
            launch: true,
            entry: process.env.SQ_ENTRY ?? "src/mainqml.tsx",
            css: process.env.SQ_CSS ?? "examples/examples.css",
          }
        // Web entry: renders the same <Gallery> component (no Window).
        : { target: "web", entry: process.env.SQ_ENTRY ?? "src/main.tsx" },
    ),
    // QML's V4 engine has Promises but NOT async/await. Lower async/await to plain
    // Promise `.then().catch()` chains (no regenerator runtime) so the same source can run
    // under V4, not just the browser.
    solid({
      babel: {
        plugins: ["babel-plugin-transform-async-to-promises"],
      },
    }),
  ],
}));
