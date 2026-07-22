/** Refs (`ref={x}`) resolved in RENDER-tree handlers — not just onMount. A <Shortcut>/menu
 *  handler calling `x.method()` must emit `_ref_x.method()`, or QML throws "x is not defined". */

import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

test("ref: a render-tree handler resolves the ref var to its _ref_ id", async () => {
  const src = `
    export function App() {
      let box: any;
      return (
        <div>
          <Shortcut keys="Ctrl+K" onActivated={() => box.doThing()} />
          <div ref={box} class="target" />
        </div>
      );
    }
  `;
  const app = await generate(src, "/tmp/t/app.tsx");
  // The handler (emitted BEFORE the ref element in document order) must still resolve.
  assert.match(app.entry, /onActivated: \{ _ref_box\.doThing\(\) \}/);
  assert.match(app.entry, /id: _ref_box/);
  assert.doesNotMatch(app.entry, /\bbox\.doThing\b/); // never the bare, undefined name
});

test("ref: a menu handler after the ref also resolves it", async () => {
  const src = `
    export function App() {
      let view: any;
      return (
        <div>
          <div ref={view} class="v" />
          <ContextMenu>
            <MenuItem onClick={() => view.copy()}>Copy</MenuItem>
          </ContextMenu>
        </div>
      );
    }
  `;
  const app = await generate(src, "/tmp/t/app.tsx");
  assert.match(app.entry, /_ref_view\.copy\(\)/);
});

test("select: onChange with a bare value param resolves to the picked value (not undefined)", async () => {
  const src = `
    export function App() {
      const [n, setN] = createSignal(1);
      return (
        <select value={"" + n()} onChange={(v) => setN(parseInt(v))}>
          <option value="1">one</option>
          <option value="2">two</option>
        </select>
      );
    }
  `;
  const app = await generate(src, "/tmp/t/app.tsx");
  // The bare param `v` must become the picked value, not the undefined `__ev`.
  assert.match(app.entry, /onActivated: \(index\) => \{ n = parseInt\(__input0\.values\[index\]\) \}/);
  assert.doesNotMatch(app.entry, /__ev/);
});
