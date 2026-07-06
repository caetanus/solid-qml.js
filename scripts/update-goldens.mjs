// Regenerate the golden fixture expectations from transpiler/test/golden.test.ts.
// The golden tests bake exact QML output; when the emit intentionally changes (e.g. the widget
// migration), run `node --import tsx scripts/update-goldens.mjs` and eyeball `git diff` to confirm
// only the intended lines moved. Parses each test block for (fixture, selector, expected-file).
import { readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { generate } from "../transpiler/src/index.ts";

const testFile = fileURLToPath(new URL("../transpiler/test/golden.test.ts", import.meta.url));
const dir = fileURLToPath(new URL("../transpiler/test/fixtures/", import.meta.url));
const body = await readFile(testFile, "utf8");

// Split into individual test(...) blocks (best-effort on the `});` closer).
const blocks = body.split(/\ntest\(/).slice(1);
let written = 0;
for (const block of blocks) {
  let app;
  const dirSrc = block.match(/readFile\(`\$\{dir\}([^`]+?)`,\s*"utf8"\)/);
  const genM = block.match(/generate\(src,\s*(?:`\$\{dir\}([^`]+)`|"([^"]+)")\)/);
  const exSrc = block.match(/new URL\("\.\.\/\.\.\/([^"]+)"/);
  if (dirSrc && genM) {
    const name = genM[1] ? dir + genM[1] : genM[2];
    app = await generate(await readFile(dir + dirSrc[1], "utf8"), name);
  } else if (exSrc) {
    // `const f = fileURLToPath(new URL("../../examples/X.tsx")); generate(readFile(f), f)`
    const f = fileURLToPath(new URL("../" + exSrc[1], import.meta.url));
    app = await generate(await readFile(f, "utf8"), f);
  } else continue;
  const pick = (sel) => sel === "got" || sel === "app"
    ? undefined
    : sel.replace(/^app\./, "").split(".").reduce((o, k) => o?.[k], app);
  // `got` is `(await generate(...)).<selector>` — resolve its selector from the definition.
  const gm = block.match(/const got = \(await generate\(src,[^)]*\)\)\.(\w+(?:\.\w+)?)/);
  const gotVal = gm ? gm[1].split(".").reduce((o, k) => o?.[k], app) : undefined;

  const valueOf = (sel) => sel === "got" ? gotVal : pick(sel);
  // Map local `const VAR = await readFile(`${dir}EXP`)` so asserts can reference expected files by name.
  const varFile = new Map();
  for (const [, v, exp] of block.matchAll(/const (\w+) = await readFile\(`\$\{dir\}([^`]+?)`/g))
    varFile.set(v, exp);

  const pairs = [];
  // Pattern A (inline readFile in the assert): assert.equal(<sel>.trimEnd(), (await readFile(`${dir}<exp>`...)))
  for (const [, sel, exp] of block.matchAll(/assert\.equal\(\s*([\w.]+)\.trimEnd\(\)\s*,\s*\(await readFile\(`\$\{dir\}([^`]+?)`/g))
    pairs.push([valueOf(sel), exp]);
  // Pattern B (named variable): assert.equal(<sel>.trimEnd(), <var>.trimEnd()) where <var> is a readFile.
  for (const [, sel, v] of block.matchAll(/assert\.equal\(\s*([\w.]+)\.trimEnd\(\)\s*,\s*(\w+)\.trimEnd\(\)/g))
    if (varFile.has(v)) pairs.push([valueOf(sel), varFile.get(v)]);

  for (const [value, exp] of pairs) {
    if (typeof value !== "string") { console.warn("skip (no value):", exp); continue; }
    await writeFile(dir + exp, value.endsWith("\n") ? value : value + "\n");
    written++;
  }
}
console.log(`updated ${written} golden files`);
