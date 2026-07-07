/** CSS sidecar assembly: side-effect .css imports across the module graph, plus the runtime's
 *  own reset.css (on the web the runtime module injects it — `import "./reset.css"` in
 *  runtime.tsx — so the native sidecar must carry the SAME reset for box-model parity). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

const FILES: Record<string, string> = {
  "/app/main.tsx": [
    `import { div } from "../src/solid-qml/runtime";`,
    `import "./app.css";`,
    `export function F(){ return <div class="a" />; }`,
  ].join("\n"),
  "/src/solid-qml/reset.css": "*,\n*::before,\n*::after {\n  box-sizing: border-box;\n}\n",
  "/app/app.css": ".a { color: red; }\n",
};

const readFile = async (p: string) => {
  const v = FILES[p];
  if (v === undefined) throw new Error(`unexpected read: ${p}`);
  return v;
};

test("css sidecar: importing the runtime pulls its reset.css beside it (box-sizing web parity)", async () => {
  const app = await generate(FILES["/app/main.tsx"], "/app/main.tsx", { readFile });
  assert.ok(app.css.includes("box-sizing: border-box"), "reset is in the sidecar");
  assert.ok(app.css.includes(".a { color: red; }"), "author css is in the sidecar");
  // Discovery order mirrors the web's ESM execution: the runtime import precedes the author css.
  assert.ok(app.css.indexOf("box-sizing") < app.css.indexOf(".a {"), "reset precedes author css");
});

test("css sidecar: the reset is collected once across the module graph", async () => {
  const files = {
    ...FILES,
    "/app/main.tsx": [
      `import { div } from "../src/solid-qml/runtime";`,
      `import "./app.css";`,
      `import { Sub } from "./sub";`,
      `export function F(){ return <div class="a"><Sub /></div>; }`,
    ].join("\n"),
    "/app/sub.tsx": [
      `import { div } from "../src/solid-qml/runtime";`,
      `export function Sub(){ return <div class="b" />; }`,
    ].join("\n"),
  };
  const app = await generate(files["/app/main.tsx"], "/app/main.tsx", {
    readFile: async (p: string) => {
      const v = files[p as keyof typeof files];
      if (v === undefined) throw new Error(`unexpected read: ${p}`);
      return v;
    },
  });
  const hits = app.css.split("box-sizing: border-box").length - 1;
  assert.equal(hits, 1, "reset appears exactly once");
});
