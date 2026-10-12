// Pinned, checksum-verified packaging tools, downloaded once into the user cache
// ($SOLID_QML_CACHE, else $XDG_CACHE_HOME/solid-qml, else ~/.cache/solid-qml).
import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { chmod, mkdir, readFile, rename, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { CliError } from "./project.mjs";

export const TOOLS = {
  // AppImage/appimagetool 1.9.1 (MIT; bundles mksquashfs). Run with --appimage-extract-and-run so
  // the build host needs no FUSE.
  appimagetool: {
    url: "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
    sha256: "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0",
    file: "appimagetool-1.9.1-x86_64.AppImage",
    executable: true,
  },
  // AppImage/type2-runtime 20251108 (MIT): the static type-2 runtime prepended to the squashfs.
  // Static (no libfuse2 dependency on the user's machine); APPIMAGE_EXTRACT_AND_RUN=1 runs it
  // without FUSE at all.
  appimageRuntime: {
    url: "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
    sha256: "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
    file: "type2-runtime-20251108-x86_64",
    executable: false,
  },
};

export function cacheDir() {
  return process.env.SOLID_QML_CACHE
    ?? path.join(process.env.XDG_CACHE_HOME || path.join(os.homedir(), ".cache"), "solid-qml");
}

const sha256 = (buf) => createHash("sha256").update(buf).digest("hex");

/** Path to a verified copy of the tool, downloading it on first use. */
export async function ensureTool(name, log = console.log) {
  const tool = TOOLS[name];
  const dir = path.join(cacheDir(), "tools");
  const dest = path.join(dir, tool.file);
  if (existsSync(dest) && sha256(await readFile(dest)) === tool.sha256) return dest;
  await mkdir(dir, { recursive: true });
  log(`[solid-qml] downloading ${tool.url}`);
  const res = await fetch(tool.url, { redirect: "follow" });
  if (!res.ok) throw new CliError(`download failed (${res.status}) for ${tool.url}`);
  const buf = Buffer.from(await res.arrayBuffer());
  const got = sha256(buf);
  if (got !== tool.sha256) throw new CliError(`checksum mismatch for ${tool.url}: got ${got}, expected ${tool.sha256}`);
  await writeFile(`${dest}.part`, buf);
  if (tool.executable) await chmod(`${dest}.part`, 0o755);
  await rename(`${dest}.part`, dest);
  return dest;
}
