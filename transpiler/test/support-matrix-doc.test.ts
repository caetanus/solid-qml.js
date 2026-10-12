// docs/support-matrix.md carries tables generated from the code (the attribute contract, the widget
// registrations, and the verdict of small Solid/JSX probes run through the transpiler). This test
// fails when the doc drifts from the code — e.g. a probe the doc lists as rejected starts compiling,
// or a tag gains an attribute. Fix: `node --import tsx scripts/support-matrix.mjs` and review the diff.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");

test("docs/support-matrix.md generated tables match the code", async () => {
  // @ts-expect-error — plain .mjs script, no type declarations
  const { render } = await import("../../scripts/support-matrix.mjs");
  const doc = await readFile(path.join(ROOT, "docs/support-matrix.md"), "utf8");
  const ow = console.warn;
  let next: string;
  try { next = await render(doc); } finally { console.warn = ow; }
  assert.equal(next, doc, "docs/support-matrix.md is out of date — run: node --import tsx scripts/support-matrix.mjs");
});
