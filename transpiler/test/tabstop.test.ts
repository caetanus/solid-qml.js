/** Tab-navigation opt-out: `import { tabstop } from "qml-solid"` resolves to the loader's
 *  `solidTabstop` context property (Q_PROPERTY bool enabled, NOTIFY). The import compiles
 *  away entirely — nothing is mirrored — and both module-level and handler writes rewrite
 *  to the context property, so every widget's `activeFocusOnTab` binding re-evaluates. */

import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

const APP = `
import { createSignal } from "solid-js";
import { tabstop } from "qml-solid";

export function App() {
  const [tabs, setTabs] = createSignal(true);
  return <div>
    <input />
    <input type="checkbox" role="switch" checked={tabs()}
           onChange={(e) => { setTabs(e.target.checked); tabstop.enabled = e.target.checked; }} />
  </div>;
}
`;

test("tabstop: the qml-solid import compiles away (no mirrored modules, no import line)", async () => {
  const app = await generate(APP, "app.tsx");
  assert.equal(Object.keys(app.modules).length, 0);
  assert.doesNotMatch(app.entry, /qml-solid/);
});

test("tabstop: handler writes rewrite tabstop.enabled to the solidTabstop context property", async () => {
  const app = await generate(APP, "app.tsx");
  assert.match(app.entry, /solidTabstop\.enabled = __input\d+\.checked/);
  assert.doesNotMatch(app.entry, /\btabstop\.enabled/);
});

test("tabstop: module-level opt-out lands in the entry component's onCompleted", async () => {
  const app = await generate(
    `import { tabstop } from "qml-solid";
     tabstop.enabled = false;
     export function App() { return <div><input /></div>; }`,
    "app.tsx",
  );
  assert.match(app.entry, /Component\.onCompleted: \{ solidTabstop\.enabled = false;/);
});

test("tabstop: module-level opt-out in a non-entry module hoists to the entry, not per instance", async () => {
  // Import-time semantics: the statement runs once at app boot — it hoists to the entry
  // component's onCompleted and must NOT repeat per Child instance.
  const files: Record<string, string> = {
    "/app/child.tsx": `import { tabstop } from "qml-solid";
       tabstop.enabled = false;
       export function Child() { return <div><input /></div>; }`,
  };
  const app = await generate(
    `import { Child } from "./child";
     export function App() { return <div><Child /><Child /></div>; }`,
    "/app/main.tsx",
    { readFile: async (p) => { if (files[p]) return files[p]; throw new Error(`ENOENT ${p}`); } },
  );
  assert.doesNotMatch(app.components["Child"] ?? "", /solidTabstop\.enabled = false/);
  assert.match(app.entry, /Component\.onCompleted: \{ solidTabstop\.enabled = false;/);
});
