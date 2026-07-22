import * as path from "node:path";
import { builtinModules } from "node:module";
import { createHash } from "node:crypto";
import * as t from "@babel/types";
import { normalize } from "../babel/transform.ts";
import { emitV4Module } from "./v4-compat.ts";

export interface NodeImportRef {
  spec: string;
  imported: string;
}

export interface MirroredModule {
  /** Output path relative to the QML output directory. */
  outPath: string;
  /** V4/QML-loadable ES module source with imports rewritten to sibling mirrors. */
  source: string;
}

export interface MirrorResult {
  /** Absolute source path -> relative mirror path. */
  byAbsPath: Map<string, string>;
  /** Relative mirror path -> mirrored source. */
  modules: Map<string, string>;
}

export type ReadFile = (absPath: string) => Promise<string>;

const BUILTINS = new Set([
  ...builtinModules,
  ...builtinModules.map((m) => `node:${m}`),
]);

export function isBareSpecifier(spec: string): boolean {
  return !spec.startsWith(".") && !spec.startsWith("/") && !spec.startsWith("qrc:");
}

export function isRuntimeOnlyImport(spec: string): boolean {
  // qml: imports are registered-module foreign types (Direção B embed) — no JS to mirror.
  if (spec.startsWith("qml:")) return true;
  return spec === "solid-js" || spec === "solid-js/web" || spec === "solid-js/store" ||
    spec === "qml-solid" ||
    spec.endsWith("/solid-qml/runtime") || spec.endsWith("/solid-qml/runtime.ts");
}

export function isNodeBuiltinSpecifier(spec: string): boolean {
  return BUILTINS.has(spec) || (spec.startsWith("node:") && BUILTINS.has(spec.slice(5)));
}

// Node builtins the loader provides as real importable modules (registerModule) backed by Qt:
// `process`, `fs` (sync), `child_process` (promise-like, QProcess). These are LEFT as bare specifiers
// in mirrored code — the engine resolves them at runtime — rather than mirrored or rejected.
const SHIMMED_BUILTINS = new Set(["fs", "fs/promises", "child_process", "process"]);

export function isShimmedNodeBuiltin(spec: string): boolean {
  const bare = spec.startsWith("node:") ? spec.slice(5) : spec;
  return SHIMMED_BUILTINS.has(bare);
}

export async function mirrorNodeImports(
  roots: NodeImportRef[],
  fromDir: string,
  readFile: ReadFile,
): Promise<MirrorResult> {
  const byAbsPath = new Map<string, string>();
  const modules = new Map<string, string>();
  const visiting = new Set<string>();

  const mirror = async (absPath: string): Promise<string> => {
    const existing = byAbsPath.get(absPath);
    if (existing) return existing;
    const outPath = await mirrorOutPath(absPath, readFile);
    byAbsPath.set(absPath, outPath);
    if (visiting.has(absPath)) return outPath;
    visiting.add(absPath);

    const src = await readFile(absPath);
    const { ast } = await normalize(src, absPath);
    const file = ast as t.File;

    for (const node of file.program.body) {
      const depSpec = importLikeSource(node);
      if (!depSpec) continue;
      if (isShimmedNodeBuiltin(depSpec)) continue; // leave `import ... from "fs"` for registerModule
      const depAbs = await resolveImport(absPath, depSpec, readFile);
      const depOut = await mirror(depAbs);
      setImportLikeSource(node, `./${path.basename(depOut)}`);
    }

    modules.set(outPath, emitV4Module(file));
    visiting.delete(absPath);
    return outPath;
  };

  for (const ref of roots) {
    if (!isBareSpecifier(ref.spec) || isRuntimeOnlyImport(ref.spec)) continue;
    if (isShimmedNodeBuiltin(ref.spec)) continue; // host-provided module; left bare for registerModule
    if (isNodeBuiltinSpecifier(ref.spec)) throw new Error(`Node builtin import is not supported in QML V4 mirror: ${ref.spec}`);
    await mirror(await resolvePackage(fromDir, ref.spec, readFile));
  }

  return { byAbsPath, modules };
}

export async function resolveNodeImport(fromDir: string, spec: string, readFile: ReadFile): Promise<string> {
  if (isNodeBuiltinSpecifier(spec)) throw new Error(`Node builtin import is not supported in QML V4 mirror: ${spec}`);
  if (isBareSpecifier(spec)) return resolvePackage(fromDir, spec, readFile);
  throw new Error(`resolveNodeImport expects a bare specifier, got ${spec}`);
}

async function resolveImport(importerAbsPath: string, spec: string, readFile: ReadFile): Promise<string> {
  if (isNodeBuiltinSpecifier(spec)) throw new Error(`Node builtin import is not supported in QML V4 mirror: ${spec}`);
  if (isBareSpecifier(spec)) return resolvePackage(path.dirname(importerAbsPath), spec, readFile);
  return resolveRelative(path.dirname(importerAbsPath), spec, readFile);
}

async function resolveRelative(dir: string, spec: string, readFile: ReadFile): Promise<string> {
  const base = path.resolve(dir, spec);
  return resolveFileOrDir(base, readFile);
}

async function resolvePackage(fromDir: string, spec: string, readFile: ReadFile): Promise<string> {
  const { packageName, subpath } = parsePackageSpec(spec);
  const root = await findPackageRoot(fromDir, packageName, readFile);
  const pkg = JSON.parse(await readFile(path.join(root, "package.json"))) as Record<string, unknown>;
  const target = resolvePackageTarget(pkg, subpath);
  return resolveFileOrDir(path.resolve(root, target), readFile);
}

function parsePackageSpec(spec: string): { packageName: string; subpath: string } {
  const parts = spec.split("/");
  if (spec.startsWith("@")) return { packageName: parts.slice(0, 2).join("/"), subpath: parts.slice(2).join("/") };
  return { packageName: parts[0], subpath: parts.slice(1).join("/") };
}

async function findPackageRoot(fromDir: string, packageName: string, readFile: ReadFile): Promise<string> {
  let dir = path.resolve(fromDir);
  while (true) {
    const root = path.join(dir, "node_modules", packageName);
    try {
      await readFile(path.join(root, "package.json"));
      return root;
    } catch {
      const parent = path.dirname(dir);
      if (parent === dir) break;
      dir = parent;
    }
  }
  throw new Error(`Cannot resolve npm package "${packageName}" from ${fromDir}`);
}

function resolvePackageTarget(pkg: Record<string, unknown>, subpath: string): string {
  const key = subpath ? `./${subpath}` : ".";
  const exportsField = pkg.exports;
  if (exportsField) {
    const resolved = resolveExportTarget(exportsField, key);
    if (resolved) return resolved;
    throw new Error(`Package ${pkg.name ?? "<unknown>"} does not export ${key}`);
  }
  if (subpath) return subpath;
  if (typeof pkg.module === "string") return pkg.module;
  if (typeof pkg.main === "string") return pkg.main;
  return "index.js";
}

function resolveExportTarget(node: unknown, key: string): string | null {
  if (typeof node === "string") return key === "." ? node : null;
  if (!node || typeof node !== "object") return null;
  const rec = node as Record<string, unknown>;
  if (key in rec) return resolveConditionalTarget(rec[key]);
  if (key === "." && !Object.keys(rec).some((k) => k.startsWith("."))) return resolveConditionalTarget(rec);
  return null;
}

function resolveConditionalTarget(node: unknown): string | null {
  if (typeof node === "string") return node;
  if (!node || typeof node !== "object") return null;
  const rec = node as Record<string, unknown>;
  for (const key of ["import", "default", "module", "browser"]) {
    if (key in rec) {
      const hit = resolveConditionalTarget(rec[key]);
      if (hit) return hit;
    }
  }
  for (const [key, value] of Object.entries(rec)) {
    if (key === "types" || key === "require") continue;
    const hit = resolveConditionalTarget(value);
    if (hit) return hit;
  }
  return null;
}

async function resolveFileOrDir(base: string, readFile: ReadFile): Promise<string> {
  for (const cand of [
    base,
    `${base}.mjs`,
    `${base}.js`,
    `${base}.ts`,
    `${base}.tsx`,
    path.join(base, "index.mjs"),
    path.join(base, "index.js"),
    path.join(base, "index.ts"),
    path.join(base, "index.tsx"),
  ]) {
    try {
      await readFile(cand);
      return cand;
    } catch {
      // next candidate
    }
  }
  throw new Error(`Cannot resolve module file ${base}`);
}

function importLikeSource(node: t.Statement): string | null {
  if (t.isImportDeclaration(node)) return node.source.value;
  if (t.isExportNamedDeclaration(node) && node.source) return node.source.value;
  if (t.isExportAllDeclaration(node)) return node.source.value;
  return null;
}

function setImportLikeSource(node: t.Statement, value: string): void {
  if (t.isImportDeclaration(node)) node.source.value = value;
  else if (t.isExportNamedDeclaration(node) && node.source) node.source.value = value;
  else if (t.isExportAllDeclaration(node)) node.source.value = value;
}

async function mirrorOutPath(absPath: string, readFile: ReadFile): Promise<string> {
  const identity = await moduleIdentity(absPath, readFile);
  const parsed = path.parse(absPath);
  const safeBase = parsed.name.replace(/[^A-Za-z0-9_]+/g, "_").replace(/^(\d)/, "_$1") || "module";
  const hash = createHash("sha1").update(identity).digest("hex").slice(0, 10);
  return `modules/${safeBase}_${hash}.mjs`;
}

async function moduleIdentity(absPath: string, readFile: ReadFile): Promise<string> {
  let dir = path.dirname(absPath);
  while (true) {
    try {
      const pkgPath = path.join(dir, "package.json");
      const pkg = JSON.parse(await readFile(pkgPath)) as { name?: string; version?: string };
      const rel = path.relative(dir, absPath).split(path.sep).join("/");
      return `${pkg.name ?? path.basename(dir)}@${pkg.version ?? "0"}:${rel}`;
    } catch {
      const parent = path.dirname(dir);
      if (parent === dir) break;
      dir = parent;
    }
  }
  return absPath;
}
