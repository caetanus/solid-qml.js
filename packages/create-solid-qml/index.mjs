// Scaffolds a solid-qml app from ./template. Used by `npm create solid-qml <dir>` (this package's
// bin) and by `solid-qml create <dir>`.
import { cp, readFile, readdir, rename, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));

const toTitle = (s) => s.replace(/[-_.]+/g, " ").replace(/\b\w/g, (c) => c.toUpperCase()).trim();
const toPkgName = (s) => s.toLowerCase().replace(/[^a-z0-9._-]+/g, "-").replace(/^[-.]+|-+$/g, "") || "solid-qml-app";

/** @param {{ dir?: string, template?: string, log?: (s: string) => void }} opts */
export async function scaffold({ dir, template = "default", log = console.log } = {}) {
  if (!dir) throw Object.assign(new Error("usage: npm create solid-qml <dir>"), { usage: true });
  if (template !== "default") throw new Error(`unknown template "${template}" (available: default)`);
  const target = path.resolve(dir);
  if (existsSync(target) && (await readdir(target)).length > 0) {
    throw new Error(`${target} exists and is not empty`);
  }
  const { version } = JSON.parse(await readFile(path.join(here, "package.json"), "utf8"));
  const name = toPkgName(path.basename(target));
  const product = toTitle(path.basename(target));

  await cp(path.join(here, "template"), target, { recursive: true });
  // npm never publishes a file named .gitignore; the template ships it as _gitignore.
  if (existsSync(path.join(target, "_gitignore"))) await rename(path.join(target, "_gitignore"), path.join(target, ".gitignore"));

  const subst = (s) => s.replaceAll("__NAME__", name).replaceAll("__PRODUCT__", product).replaceAll("__SOLID_QML_VERSION__", version);
  for (const rel of ["package.json", "index.html", "README.md", "src/main.tsx"]) {
    const file = path.join(target, rel);
    await writeFile(file, subst(await readFile(file, "utf8")));
  }

  const rel = path.relative(process.cwd(), target) || ".";
  log(`\nScaffolded ${product} in ${target}\n`);
  log("Next steps:");
  log(`  cd ${rel}`);
  log("  npm install");
  log("  npm run dev                       # browser preview (Vite)");
  log("  npx solid-qml dev                 # native window, hot reload");
  log("  npx solid-qml build --target linux  # dist/linux/*.AppImage + .tar.gz\n");
  return target;
}
