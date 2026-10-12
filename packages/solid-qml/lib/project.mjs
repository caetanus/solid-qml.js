// App project metadata: package.json + its optional "solidQml" block.
//
//   {
//     "name": "my-app", "version": "1.0.0",
//     "productName": "My App",            // window/desktop name; default: name
//     "solidQml": {
//       "entry": "src/main.tsx",          // the module whose exported component renders <Window>
//       "appId": "my-app",                // data dir / desktop file / Wayland app_id; default: name
//       "icon": "public/icon.svg",        // .svg or .png; default: the solid-qml icon
//       "categories": "Utility;"          // freedesktop Categories for the .desktop file
//     }
//   }
import { existsSync } from "node:fs";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const PACKAGE_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

export class CliError extends Error {}

/** Walk up from `start` to the nearest directory holding a package.json. */
function findProjectDir(start) {
  for (let d = path.resolve(start); ; d = path.dirname(d)) {
    if (existsSync(path.join(d, "package.json"))) return d;
    if (path.dirname(d) === d) throw new CliError(`no package.json found from ${start} — run inside a solid-qml app`);
  }
}

const slug = (s) => String(s).replace(/^@[^/]+\//, "").toLowerCase().replace(/[^a-z0-9._-]+/g, "-").replace(/^-+|-+$/g, "") || "app";

export async function loadProject(start = process.cwd()) {
  const dir = findProjectDir(start);
  const pkg = JSON.parse(await readFile(path.join(dir, "package.json"), "utf8"));
  const cfg = pkg.solidQml ?? {};
  const entry = cfg.entry
    ?? ["src/main.tsx", "src/App.tsx", "src/main.jsx", "src/App.jsx"].find((f) => existsSync(path.join(dir, f)));
  if (!entry || !existsSync(path.join(dir, entry))) {
    throw new CliError(`entry not found — set "solidQml.entry" in ${path.join(dir, "package.json")}`);
  }
  const name = pkg.name ?? path.basename(dir);
  const productName = pkg.productName ?? cfg.productName ?? name;
  const icon = cfg.icon
    ? path.resolve(dir, cfg.icon)
    : ["public/icon.svg", "public/icon.png"].map((f) => path.join(dir, f)).find((f) => existsSync(f))
      ?? path.join(PACKAGE_DIR, "assets/icon.svg");
  return {
    dir,
    pkg,
    name,
    version: pkg.version ?? "0.0.0",
    productName,
    appId: slug(cfg.appId ?? name),
    /** File-name-safe product name (AppImage / archive names). */
    fileName: String(productName).trim().replace(/[^A-Za-z0-9._-]+/g, "-").replace(/^-+|-+$/g, "") || slug(name),
    entry: path.join(dir, entry),
    icon,
    categories: cfg.categories ?? "Utility;",
    workDir: path.join(dir, ".solid-qml"),
  };
}
