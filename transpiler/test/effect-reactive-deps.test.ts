import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

// End-to-end (full transpile pipeline) coverage for the effect-deps fix: a createEffect body that
// reads something OTHER than a bare Solid signal/memo must still get a Connections re-run handler,
// wired to the RIGHT target. Unit-level AST coverage for effectDeps itself lives in symbols.test.ts;
// these confirm component.ts actually wires what effectDeps reports (propsParam/ctxBindings/
// refLocals/knownNonReactive all flow through correctly end-to-end).

test("office app bug: createEffect reading a context-property (office.spellReady) gets a Connections on office", async () => {
  const src = `
    import { createEffect } from "solid-js";
    export default function App() {
      function recheckAll() {}
      createEffect(() => { if (office.spellReady) recheckAll(); });
      return <div>office spellcheck</div>;
    }
  `;
  const app = await generate(src, "office-app.tsx");
  assert.match(app.entry, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; if \(office\.spellReady\) \{ recheckAll\(\); \} \}/);
  assert.match(app.entry, /Connections \{\s*target: office\s*function onSpellReadyChanged\(\) \{ __effect0\(\); \}\s*\}/);
});

test("office app bug, 2nd case: createEffect reading office.picked gets a Connections on office", async () => {
  const src = `
    import { createEffect } from "solid-js";
    export default function App() {
      function onPicked(v) {}
      createEffect(() => { if (office.picked) onPicked(JSON.parse(office.picked)); });
      return <div>picker</div>;
    }
  `;
  const app = await generate(src, "office-app2.tsx");
  assert.match(app.entry, /Connections \{\s*target: office\s*function onPickedChanged\(\)/);
  // JSON is a builtin global — must not get its own (bogus) Connections block.
  assert.doesNotMatch(app.entry, /target: JSON/);
});

test("createEffect reading props.x gets a Connections on __self (the prop IS a self property)", async () => {
  const src = `
    import { createEffect } from "solid-js";
    export default function App(props) {
      function doSomething() {}
      createEffect(() => { if (props.x) doSomething(); });
      return <div>{props.x}</div>;
    }
  `;
  const app = await generate(src, "props-eff.tsx");
  // `x` is reserved: the prop property is `x_`, and the handler must name IT, not the Item's own `x`.
  assert.match(app.entry, /property var x_\b/);
  assert.match(app.entry, /Connections \{\s*target: __self\s*function onX_Changed\(\)/);
});

test("createEffect reading a store field gets a Connections on __self for the whole store", async () => {
  const src = `
    import { createEffect } from "solid-js";
    import { createStore } from "solid-js/store";
    export default function App() {
      const [store, setStore] = createStore({ field: 1 });
      function doSomething() {}
      createEffect(() => { if (store.field) doSomething(); });
      return <div>{store.field}</div>;
    }
  `;
  const app = await generate(src, "store-eff.tsx");
  assert.match(app.entry, /Connections \{\s*target: __self\s*function onStoreChanged\(\)/);
});

test("createEffect that reads a signal and writes a DIFFERENT one does not loop (one Connections, on the read signal only)", async () => {
  const src = `
    import { createSignal, createEffect } from "solid-js";
    export default function App() {
      const [count, setCount] = createSignal(0);
      const [other, setOther] = createSignal(0);
      createEffect(() => setOther(count() * 2));
      return <div>{count()} {other()}</div>;
    }
  `;
  const app = await generate(src, "no-loop.tsx");
  const matches = [...app.entry.matchAll(/Connections \{/g)];
  assert.equal(matches.length, 1, "exactly one Connections block");
  assert.match(app.entry, /function __effect0\(\) \{ if \(!__started\.__effect0\) return; other = count \* 2; \}/);
  assert.match(app.entry, /function onCountChanged\(\) \{ __effect0\(\); \}/);
  assert.doesNotMatch(app.entry, /function onOtherChanged/, "writing `other` must not itself be tracked as a read dep");
});

test("office app bug, 3rd case: a member only checked with typeof, or only called, is not a dependency", async () => {
  // The office shape: a helper reached from an effect feature-checks a method on a ref before
  // calling it. Before the fix this emitted `function onSetCursorPositionChanged()` on the ref and
  // QML warned "no signal of the target matches".
  const src = `
    import { createEffect } from "solid-js";
    export default function App(props) {
      let doc;
      function resetCursor() {
        if (typeof doc.setCursorPosition === "function") doc.setCursorPosition(0);
        doc.select?.(0, 0);
        if (typeof office.reload === "function") office.reload();
        if (typeof props.onReset === "function") props.onReset?.();
      }
      createEffect(() => { if (doc.documentPageMargins) resetCursor(); });
      return <textarea ref={doc} />;
    }
  `;
  const app = await generate(src, "typeof-dep.tsx");
  assert.match(app.entry, /target: _ref_doc\s*function onDocumentPageMarginsChanged\(\) \{ __effect0\(\); \}/); // a real read still tracks
  assert.doesNotMatch(app.entry, /onSetCursorPositionChanged/);
  assert.doesNotMatch(app.entry, /onSelectChanged/);
  assert.doesNotMatch(app.entry, /onReloadChanged/);
  assert.doesNotMatch(app.entry, /onOnResetChanged/);
  assert.equal([...app.entry.matchAll(/Connections \{/g)].length, 1, "only the documentPageMargins dependency");
});

test("a member read plainly is still a dependency even when it is also typeof-checked", async () => {
  const src = `
    import { createEffect } from "solid-js";
    export default function App() {
      function show(v) {}
      createEffect(() => { if (typeof office.status === "string") show(office.status); });
      return <div>s</div>;
    }
  `;
  const app = await generate(src, "typeof-and-read.tsx");
  assert.match(app.entry, /target: office\s*function onStatusChanged\(\)/);
});

test("a plain component-level value (let array mutated in place, const object, function-local) is never an effect dependency", async () => {
  const src = `
    import { createEffect } from "solid-js";
    export default function App() {
      let spellQueue: string[] = [];
      const cache = { size: 0 };
      function scheduleSpell() { if (spellQueue.length > 0 && cache.size >= 0) spellQueue.push("x"); }
      function recheckAll() { scheduleSpell(); }
      createEffect(() => { if (office.spellReady) recheckAll(); });
      return <div>q</div>;
    }
  `;
  const app = await generate(src, "office-queue.tsx");
  assert.match(app.entry, /target: office\s*function onSpellReadyChanged\(\)/);
  assert.doesNotMatch(app.entry, /target: spellQueue/);
  assert.doesNotMatch(app.entry, /target: cache/);
});
