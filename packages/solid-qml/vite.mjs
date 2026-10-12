// `solid-qml/vite` — the Vite plugin for the WEB preview of a solid-qml app. Pair it with
// vite-plugin-solid:
//
//   import { defineConfig } from "vite";
//   import solid from "vite-plugin-solid";
//   import solidQml from "solid-qml/vite";
//   export default defineConfig({ plugins: [solidQml(), solid()] });
//
// It maps the QML-flavoured authoring surface onto the DOM (signal-style handlers, the <text>
// primitive, `import X from "./X.qml"`), and keeps the "solid-qml" package (shipped as TSX) out of
// esbuild's dependency pre-bundling so vite-plugin-solid compiles it. The NATIVE side (TSX → QML
// + the native window) is `npx solid-qml dev` / `npx solid-qml build`, not Vite.

const qmlModulePrefix = "\0solid-qml:qml:";

/** Web preview compatibility for QML/Qt signal-style handlers and the <text> primitive. */
export function transformWebEvents(code) {
  return (
    code
      .replace(/\bonClicked=/g, "onClick=")
      .replace(/\bonPressed=/g, "onPointerDown=")
      .replace(/\bonReleased=/g, "onPointerUp=")
      // `<text>` is the native CssText primitive; in the browser a raw <text> is an
      // HTMLUnknownElement that does not lay out in flex — map it to <span> (inline text).
      .replace(/<text(\s|>|\/)/g, "<span$1")
      .replace(/<\/text>/g, "</span>")
  );
}

/** @param {{ runtimeModule?: string }} [options] */
export default function solidQml(options = {}) {
  const runtimeModule = options.runtimeModule ?? "solid-qml";
  return {
    name: "solid-qml",
    enforce: "pre",

    config() {
      return {
        resolve: {
          // The legacy authoring alias used inside the solid-qml repo.
          alias: { "qml-solid": runtimeModule },
        },
        optimizeDeps: { exclude: ["solid-qml"] },
      };
    },

    async resolveId(id, importer) {
      if (!id.endsWith(".qml")) return null;
      const resolved = await this.resolve(id, importer, { skipSelf: true });
      return resolved ? qmlModulePrefix + resolved.id : null;
    },

    load(id) {
      if (!id.startsWith(qmlModulePrefix)) return null;
      const file = id.slice(qmlModulePrefix.length);
      // A hand-written .qml imported into TSX renders as a labelled placeholder on the web.
      return `import { qmlComponent } from ${JSON.stringify(runtimeModule)};\n` +
        `export default qmlComponent(${JSON.stringify(file)});\n`;
    },

    transform(code, id) {
      if (!/\.[tj]sx$/.test(id) || id.includes("/node_modules/")) return null;
      return { code: transformWebEvents(code), map: null };
    },
  };
}

export { solidQml };
