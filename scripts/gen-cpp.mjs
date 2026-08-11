// One-shot AOT (C++) generation via the transpiler's cpp back-end: TSX entry → generated.h +
// generated.cpp + main.cpp. Mirrors scripts/gen.mjs (the QML back-end). Run with tsx:
//   node --import tsx scripts/gen-cpp.mjs [entry] [outDir]
import { generateCpp } from "../transpiler/src/emit/cpp/index.ts";
import { writeFile, mkdir, readFile } from "node:fs/promises";
import path from "node:path";

const entry = process.argv[2] ?? "examples/aot/counter.tsx";
const outDir = process.argv[3] ?? "examples/aot/generated";

const gen = await generateCpp(await readFile(entry, "utf8"), entry);
await mkdir(outDir, { recursive: true });
await writeFile(path.join(outDir, "generated.h"), gen.header);
await writeFile(path.join(outDir, "generated.cpp"), gen.source);
if (gen.main) await writeFile(path.join(outDir, "main.cpp"), gen.main);
console.log(`[gen-cpp] ${entry} → ${outDir} (entry ${gen.entry.buildFn}, ${gen.entry.width}x${gen.entry.height})`);
