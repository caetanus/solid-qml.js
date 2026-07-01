import { execFileSync, spawn, type ChildProcess } from "node:child_process";
import path from "node:path";
import type { Plugin } from "vite";

const qmlModulePrefix = "\0solid-qml:file:";

export type SolidQmlPluginOptions = {
  target?: "web" | "qml";
  entry?: string;
  qmlOutput?: string;
  generate?: boolean;
  // Native dev: open the QML window from `vite dev` and hot-reload it on .tsx edits.
  launch?: boolean;
  css?: string;
  loaderBin?: string;
};

function transformWebEvents(code: string) {
  // Web preview compatibility for QML/Qt signal-style handlers.
  return (
    code
      .replace(/\bonClicked=/g, "onClick=")
      .replace(/\bonPressed=/g, "onPointerDown=")
      .replace(/\bonReleased=/g, "onPointerUp=")
      // `<text>` is the CssText primitive; in the browser it must be a real element. A raw
      // lowercase <text> is an (invalid) HTMLUnknownElement that doesn't lay out in flex, so
      // map it to <span> — matching the native CssText (inline text) and keeping its class.
      .replace(/<text(\s|>|\/)/g, "<span$1")
      .replace(/<\/text>/g, "</span>")
  );
}

function qmlComponentModule(path: string) {
  return `
    import { qmlComponent } from "/src/solid-qml/runtime.tsx";
    export default qmlComponent(${JSON.stringify(path)});
  `;
}

export function solidQml(options: SolidQmlPluginOptions = {}): Plugin {
  const target = options.target ?? "web";
  const entry = options.entry ?? "src/main.tsx";
  const qmlOutput = options.qmlOutput ?? "qml/solidqml/App.generated.qml";
  const generate = options.generate ?? true;
  const launch = options.launch ?? false;
  const css = options.css ?? "qml/solidqml/App.generated.css";
  const loaderBin = options.loaderBin ?? "build/solid-qml-loader";
  let root = process.cwd();
  let loader: ChildProcess | undefined;

  function generateQml() {
    // QML is only needed for the native (qmldev) target. In the web preview, generating it is
    // useless work that (a) blocks with a synchronous transpiler run on every edit and (b) writes
    // App.generated.* into the tree the dev server watches — both of which break browser HMR.
    if (!generate || target === "web") {
      return;
    }

    execFileSync(
      process.execPath,
      [
        "--import",
        "tsx",
        path.join(root, "scripts/gen.mjs"),
        path.join(root, entry),
        path.join(root, qmlOutput),
      ],
      { stdio: "inherit" },
    );
  }

  return {
    name: "solid-qml",
    enforce: "pre",

    configResolved(config) {
      root = config.root;
    },

    buildStart() {
      this.addWatchFile(path.join(root, entry));
      generateQml();
    },

    // Web preview: point index.html at the entry. The example launcher (examples/gallery.tsx)
    // previews one app per iframe by requesting `/?app=<tsx path>`; honour that query (validated to
    // a project .tsx) so each example renders in its own document — no CSS/JS bleed between apps.
    transformIndexHtml: {
      order: "pre",
      handler(html, ctx) {
        let active = entry;
        const query = ctx.originalUrl?.split("?")[1];
        if (query) {
          const app = new URLSearchParams(query).get("app");
          if (app && /^[\w][\w./-]*\.[jt]sx$/.test(app) && !app.includes("..")) {
            active = app;
          }
        }
        return active === "src/main.tsx" ? html : html.replace("/src/main.tsx", `/${active}`);
      },
    },

    // Native dev loop: vite owns the watcher; it regenerates the QML on any .tsx change and the
    // loader (started here with --watch) reloads the live window. CSS is hot-reloaded by the
    // loader's own --css file watcher.
    configureServer(server) {
      if (!launch) {
        return;
      }

      // Generate once up front so the loader opens fresh QML (buildStart may run after this).
      generateQml();

      // Load the generated sidecar stylesheet (every imported .css, bundled by the transpiler) as
      // the base layer — deterministic for flex/grid. `css` (legacy single sheet) is layered first.
      const sidecar = path.join(root, qmlOutput.replace(/\.qml$/, ".css"));
      const bin = path.join(root, loaderBin);
      loader = spawn(
        bin,
        ["--qml", path.join(root, qmlOutput), "--css", path.join(root, css), "--css", sidecar, "--watch"],
        { stdio: "inherit" },
      );
      loader.on("error", (err) => {
        server.config.logger.error(`[solid-qml] cannot start ${bin} (build it: ninja -C build): ${err.message}`);
      });

      // Regenerate on any JSX source change in the project (not just src/ — the entry may live
      // in examples/ via SQ_ENTRY). Match only `.tsx`/`.jsx` SOURCE, never the generated `.js`
      // sidecar (that would self-trigger an infinite loop), and debounce so a burst of editor
      // save events coalesces into a single regeneration. The loader's --watch then reloads.
      const outDir = path.dirname(path.join(root, qmlOutput));
      const generatorScript = path.join(root, "scripts/gen.mjs");
      server.watcher.add(generatorScript);
      let regenTimer: ReturnType<typeof setTimeout> | undefined;
      server.watcher.on("change", (file) => {
        // Regenerate on JSX SOURCE edits, or when the generator script itself changes (so the
        // output never goes stale while the transpiler is being developed). Stay in-project, skip
        // node_modules, and NEVER react to the generated output dir — writing App.generated.* must
        // not re-trigger us (that's the infinite-loop trap).
        const isSource = /\.[jt]sx$/.test(file) || file === generatorScript;
        if (!isSource || !file.startsWith(root) || file.includes("node_modules")
            || file.startsWith(outDir)) {
          return;
        }
        clearTimeout(regenTimer);
        regenTimer = setTimeout(() => {
          server.config.logger.info(`[solid-qml] ${path.relative(root, file)} → regenerating QML`);
          generateQml();
        }, 150);
      });

      const stop = () => loader?.kill();
      server.httpServer?.once("close", stop);
      process.once("exit", stop);
    },

    resolveId(id, importer) {
      if (!id.endsWith(".qml")) {
        return null;
      }

      const resolved = this.resolve(id, importer, { skipSelf: true });
      return resolved.then((result) => {
        if (!result) {
          return null;
        }
        return qmlModulePrefix + result.id;
      });
    },

    load(id) {
      if (!id.startsWith(qmlModulePrefix)) {
        return null;
      }

      return qmlComponentModule(id.slice(qmlModulePrefix.length));
    },

    transform(code, id) {
      if (!/\.[tj]sx$/.test(id)) {
        return null;
      }

      if (target === "web") {
        return {
          code: transformWebEvents(code),
          map: null,
        };
      }

      return null;
    },
  };
}
