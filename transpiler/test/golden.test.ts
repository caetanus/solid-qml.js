import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { generate } from "../src/index.ts";

const dir = fileURLToPath(new URL("./fixtures/", import.meta.url));

test("golden: static.tsx generates the expected QML", async () => {
  const src = await readFile(`${dir}static.tsx`, "utf8");
  const got = (await generate(src, "static.tsx")).entry;
  const want = await readFile(`${dir}static.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
});

test("golden: counter.tsx generates the expected reactive QML", async () => {
  const src = await readFile(`${dir}counter.tsx`, "utf8");
  const got = (await generate(src, "counter.tsx")).entry;
  const want = await readFile(`${dir}counter.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
});

test("golden: derived.tsx generates the expected memo QML", async () => {
  const src = await readFile(`${dir}derived.tsx`, "utf8");
  const got = (await generate(src, "derived.tsx")).entry;
  const want = await readFile(`${dir}derived.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
});

test("golden: list.tsx generates the expected keyed CssRepeater QML", async () => {
  const src = await readFile(`${dir}list.tsx`, "utf8");
  const got = (await generate(src, "list.tsx")).entry;
  const want = await readFile(`${dir}list.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
});

test("golden: dynamic.tsx generates the expected reactive-tag QML", async () => {
  const src = await readFile(`${dir}dynamic.tsx`, "utf8");
  const got = (await generate(src, "dynamic.tsx")).entry;
  const want = await readFile(`${dir}dynamic.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
});

test("golden: card.tsx emits the entry + the Greeting component", async () => {
  const src = await readFile(`${dir}card.tsx`, "utf8");
  const app = await generate(src, "card.tsx");
  const entryWant = await readFile(`${dir}card.entry.qml`, "utf8");
  const greetWant = await readFile(`${dir}card.Greeting.qml`, "utf8");
  assert.equal(app.entry.trimEnd(), entryWant.trimEnd());
  assert.equal(app.components.Greeting.trimEnd(), greetWant.trimEnd());
});

test("golden: app.tsx follows the ./Button import into its own QML type", async () => {
  const src = await readFile(`${dir}app.tsx`, "utf8");
  const app = await generate(src, `${dir}app.tsx`);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}app.entry.qml`, "utf8")).trimEnd());
  assert.equal(app.components.Button.trimEnd(), (await readFile(`${dir}app.Button.qml`, "utf8")).trimEnd());
});

test("golden: slot.tsx mounts instance children into the component's default slot", async () => {
  const src = await readFile(`${dir}slot.tsx`, "utf8");
  const app = await generate(src, `${dir}slot.tsx`);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}slot.entry.qml`, "utf8")).trimEnd());
  assert.equal(app.components.Card.trimEnd(), (await readFile(`${dir}slot.Card.qml`, "utf8")).trimEnd());
});

test("golden: alias.tsx resolves <Hello> to the imported Greeting type", async () => {
  const src = await readFile(`${dir}alias.tsx`, "utf8");
  const app = await generate(src, `${dir}alias.tsx`);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}alias.entry.qml`, "utf8")).trimEnd());
  assert.equal(app.components.Greeting.trimEnd(), (await readFile(`${dir}alias.Greeting.qml`, "utf8")).trimEnd());
});

test("golden: two modules exporting Box get distinct, path-prefixed type names", async () => {
  const src = await readFile(`${dir}collide.tsx`, "utf8");
  const app = await generate(src, `${dir}collide.tsx`);
  assert.deepEqual(Object.keys(app.components).sort(), ["Box_blue", "Box_red"]);
  assert.match(app.entry, /Box_red \{[\s\S]*label: "r"/);
  assert.match(app.entry, /Box_blue \{[\s\S]*label: "b"/);
  assert.match(app.components.Box_red, /cssClass: \["red-box"\]/);
  assert.match(app.components.Box_blue, /cssClass: \["blue-box"\]/);
});

test("golden: examples/counter.tsx emits the timer setup + cleanup lifecycle", async () => {
  const f = fileURLToPath(new URL("../../examples/counter.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}counter-example.expected.qml`, "utf8")).trimEnd());
});

test("golden: examples/refs.tsx emits the ref id and resolves it in onMount", async () => {
  const f = fileURLToPath(new URL("../../examples/refs.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}refs.expected.qml`, "utf8")).trimEnd());
});

test("golden: examples/index-list.tsx emits a Repeater with item()->modelData", async () => {
  const f = fileURLToPath(new URL("../../examples/index-list.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}index-list.expected.qml`, "utf8")).trimEnd());
});

test("golden: examples/hello.tsx emits button with element child (label + nested badge)", async () => {
  const f = fileURLToPath(new URL("../../examples/hello.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}hello.expected.qml`, "utf8")).trimEnd());
});

test("golden: examples/props-helpers.tsx resolves mergeProps defaults + splitProps", async () => {
  const f = fileURLToPath(new URL("../../examples/props-helpers.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.equal(app.components.Greeting.trimEnd(), (await readFile(`${dir}props-helpers.Greeting.qml`, "utf8")).trimEnd());
  assert.equal(app.components.Tag.trimEnd(), (await readFile(`${dir}props-helpers.Tag.qml`, "utf8")).trimEnd());
  assert.match(app.entry, /Greeting \{[\s\S]*name: "Solid"/);
});

test("golden: context.CounterProvider.tsx emits provider with folded init (Option 1) + fn member", async () => {
  const src = await readFile(`${dir}context.CounterProvider.tsx`, "utf8");
  const app = await generate(src, `${dir}context.CounterProvider.tsx`);
  const want = await readFile(`${dir}context.CounterProvider.qml`, "utf8");
  assert.equal(app.entry.trimEnd(), want.trimEnd());
  // Key structural assertions (belt + suspenders)
  assert.match(app.entry, /property var count: 0/);           // Option 1: default only, no self-ref
  assert.doesNotMatch(app.entry, /count: count/);             // no binding loop
  assert.match(app.entry, /property var increment: function/); // fn member exposed
  assert.match(app.entry, /increment: function\(\) \{ return count = count \+ 1 \}/); // fn body bare
  assert.match(app.entry, /cssPrimitive: "div"/);             // provider root is transparent div
});

test("golden: show-fallback.tsx emits child with positive guard and fallback with inverted guard", async () => {
  const src = await readFile(`${dir}show-fallback.tsx`, "utf8");
  const got = (await generate(src, "show-fallback.tsx")).entry;
  const want = await readFile(`${dir}show-fallback.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  assert.match(got, /visible: !!\(ok\)[\s\S]*text: "yes"/);
  assert.match(got, /visible: !\(ok\)[\s\S]*text: "none"/);
});

test("golden: img-demo.tsx emits Css.CssImage with source binding", async () => {
  const src = await readFile(`${dir}img-demo.tsx`, "utf8");
  const got = (await generate(src, "img-demo.tsx")).entry;
  const want = await readFile(`${dir}img-demo.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  assert.match(got, /Css\.CssImage \{/);
  assert.match(got, /cssClass: \["hero"\]/);
  assert.match(got, /source: url \|\| ""/);
});

test("golden: effect-smoke.tsx emits onCompleted body + Connections dep-tracking", async () => {
  const src = await readFile(`${dir}effect-smoke.tsx`, "utf8");
  const app = await generate(src, `${dir}effect-smoke.tsx`);
  const want = await readFile(`${dir}effect-smoke.expected.qml`, "utf8");
  assert.equal(app.entry.trimEnd(), want.trimEnd());
  // Belt-and-suspenders structural assertions
  assert.match(app.entry, /property var b: 0/);                        // signal b initialised to 0
  assert.match(app.entry, /b = a \* 2/);                               // effect body in onCompleted (bare)
  assert.match(app.entry, /Connections \{[\s\S]*target: __self/);       // Connections block present
  assert.match(app.entry, /function onAChanged\(\) \{ b = a \* 2; \}/); // dep handler (bare)
});

test("golden: input-demo.tsx emits CssFill + T.TextField with Binding and placeholder", async () => {
  const src = await readFile(`${dir}input-demo.tsx`, "utf8");
  const got = (await generate(src, "input-demo.tsx")).entry;
  const want = await readFile(`${dir}input-demo.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  assert.match(got, /Css\.CssFill \{/);
  assert.match(got, /cssClass: \["search"\]/);
  assert.match(got, /T\.TextField \{/);           // native Template control (no visual chrome)
  assert.match(got, /id: __input0/);
  assert.match(got, /background: null/);
  assert.match(got, /onTextEdited: \{ s = text \}/);
  assert.match(got, /Binding \{[\s\S]*target: __input0/); // persistent Binding (survives user edits)
  assert.match(got, /import QtQuick\.Templates 6\.0 as T/); // Templates import prepended
  assert.match(got, /visible: parent\.text\.length === 0/);
  assert.match(got, /text: "search…"/);
});

test("golden: examples/fetch.tsx — createResource loader + Suspense gate + null-safe reads", async () => {
  const f = fileURLToPath(new URL("../../examples/fetch.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}fetch.expected.qml`, "utf8")).trimEnd());
  // Structural assertions (belt + suspenders)
  assert.match(app.entry, /property bool user_loading: true/);                 // loading starts true
  assert.match(app.entry, /property var user_error/);                          // error slot
  assert.match(app.entry, /__load_user = function\(\)/);                       // loader closure (bare)
  assert.match(app.entry, /Promise\.resolve\(fetchUser\(login\)\)/);            // inlined fetcher call (bare source)
  assert.match(app.entry, /user = v; user_loading = false/);                   // resolve writes value + flips loading
  assert.match(app.entry, /function onLoginChanged\(\) \{ __load_user\(\); \}/); // reload on source change (bare)
  assert.match(app.entry, /\(user \|\| \(\{\}\)\)\.avatar_url/);                // null-safe resource read
  assert.match(app.entry, /visible: !\(!\(user_loading\)\)[\s\S]*text: "loading…"/); // fallback while loading
});

test("golden: todo-row.tsx — object prop (props.todo.X) + function prop (props.onToggle(x))", async () => {
  const src = await readFile(`${dir}todo-row.tsx`, "utf8");
  const app = await generate(src, "todo-row.tsx");
  const entryWant = await readFile(`${dir}todo-row.entry.qml`, "utf8");
  const rowWant = await readFile(`${dir}todo-row.TodoRow.qml`, "utf8");
  assert.equal(app.entry.trimEnd(), entryWant.trimEnd());
  assert.equal(app.components.TodoRow.trimEnd(), rowWant.trimEnd());
  // Structural assertions (belt + suspenders)
  assert.match(app.components.TodoRow, /property var todo\b/);              // object prop declared
  assert.match(app.components.TodoRow, /property var onToggle\b/);          // function prop declared
  assert.match(app.components.TodoRow, /property var onRemove\b/);          // function prop declared
  assert.match(app.components.TodoRow, /todo\.done/);                       // nested member in binding
  assert.match(app.components.TodoRow, /todo\.title/);                      // nested member in binding
  assert.match(app.components.TodoRow, /onClicked: onToggle\(todo\.id\)/);  // fn-prop call in handler
  assert.match(app.components.TodoRow, /onClicked: onRemove\(todo\.id\)/);  // fn-prop call in handler
  assert.doesNotMatch(app.components.TodoRow, /todo\.done\b.*property/);    // 'todo.done' is NOT a prop
});

test("golden: block-memo.tsx emits block-bodied createMemo as IIFE binding", async () => {
  const src = await readFile(`${dir}block-memo.tsx`, "utf8");
  const got = (await generate(src, "block-memo.tsx")).entry;
  const want = await readFile(`${dir}block-memo.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  // Structural: IIFE binding, bare signal reads, no __self. inside
  assert.match(got, /readonly property var visible_: \(function\(\) \{/);  // IIFE declaration
  assert.match(got, /var f = filter;/);                                     // signal read is BARE
  assert.match(got, /return todos\.filter\(/);                              // todos read is BARE
  assert.doesNotMatch(got, /__self\.filter/);                               // no selfRef in IIFE body
  assert.doesNotMatch(got, /__self\.todos/);                                // no selfRef in IIFE body
});

test("golden: classlist-demo.tsx — classList={{ cls: expr }} emits reactive cssClass concat", async () => {
  const src = await readFile(`${dir}classlist-demo.tsx`, "utf8");
  const got = (await generate(src, "classlist-demo.tsx")).entry;
  const want = await readFile(`${dir}classlist-demo.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  // Static class always present, conditional class included in binding expression
  assert.match(got, /cssClass: \["todo-row"\]\.concat\(done \? \["completed"\] : \[\]\)/);
  // No classList entry when condition is absent (static-only cssClass unchanged)
  assert.match(got, /cssClass: \["label"\]/);
  // Reactive binding — condition is bare (no __self.) so QML binding tracker sees the dep
  assert.doesNotMatch(got, /__self\.done/);
});

test("golden: mutable-local.tsx — let nextId=1 mutated in handler becomes property var", async () => {
  const src = await readFile(`${dir}mutable-local.tsx`, "utf8");
  const got = (await generate(src, "mutable-local.tsx")).entry;
  const want = await readFile(`${dir}mutable-local.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  // Structural assertions
  assert.match(got, /property var nextId: 1/);           // promoted to property var with init
  assert.doesNotMatch(got, /var nextId = 1/);            // NOT a setup var
  assert.match(got, /onClicked: label = "id:" \+ nextId\+\+/); // handler uses bare nextId (QML obj scope)
  assert.doesNotMatch(got, /__self\.nextId/);            // no __self. in QML binding context handlers
});

test("golden: examples/context.tsx — useContext consumers via __ctx injection", async () => {
  const f = fileURLToPath(new URL("../../examples/context.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.equal(app.entry.trimEnd(), (await readFile(`${dir}context.entry.qml`, "utf8")).trimEnd());
  assert.equal(app.components.Display.trimEnd(), (await readFile(`${dir}context.Display.qml`, "utf8")).trimEnd());
  assert.equal(app.components.Increment.trimEnd(), (await readFile(`${dir}context.Increment.qml`, "utf8")).trimEnd());
  // the provider instance is injected into both consumers; reactivity is via the instance property read
  assert.match(app.entry, /Display \{[\s\S]*__ctx_CounterContext: __ctxprov0/);
  assert.match(app.components.Display, /text: "Count: " \+ \(__ctx_CounterContext\.count\)/);
  assert.match(app.components.Increment, /onClicked: __ctx_CounterContext\.increment\(\)/);
});

test("golden: helper-fn.tsx — function declaration becomes QML method, onClick={fn} → onClicked: fn()", async () => {
  const src = await readFile(`${dir}helper-fn.tsx`, "utf8");
  const got = (await generate(src, "helper-fn.tsx")).entry;
  const want = await readFile(`${dir}helper-fn.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  // Structural assertions
  assert.match(got, /function inc\(\) \{ count = count \+ 1; \}/);  // helper emitted as QML method
  assert.match(got, /onClicked: inc\(\)/);                          // handler calls the method
  assert.doesNotMatch(got, /var inc/);                              // NOT a setup var
  assert.doesNotMatch(got, /__self\.inc/);                          // no __self. qualification
});

test("golden: helper-arrow.tsx — arrow helper with params → QML method; helper calls sibling bare", async () => {
  const src = await readFile(`${dir}helper-arrow.tsx`, "utf8");
  const got = (await generate(src, "helper-arrow.tsx")).entry;
  const want = await readFile(`${dir}helper-arrow.expected.qml`, "utf8");
  assert.equal(got.trimEnd(), want.trimEnd());
  // Structural assertions
  assert.match(got, /function inc\(n\) \{ count = count \+ n; \}/);      // arrow helper → QML method
  assert.match(got, /function clampedInc\(\) \{ if \(count < 5\)/);       // IfStatement in helper body
  assert.match(got, /\{ inc\(1\); \}/);                                    // sibling call is bare
  assert.match(got, /onClicked: clampedInc\(\)/);                         // handler calls the method
  assert.doesNotMatch(got, /__self\./);                                    // no __self. anywhere
});

test("examples/todomvc.tsx — createStore + setters fully transpiles", async () => {
  const f = fileURLToPath(new URL("../../examples/todomvc.tsx", import.meta.url));
  const app = await generate(await readFile(f, "utf8"), f);
  assert.ok(app.components.TodoRow, "TodoRow discovered inside <For> delegate");
  assert.match(app.entry, /property var todos: \[\]/);                 // store is a reactive property
  assert.match(app.entry, /Repeater \{/);                              // <For> -> Repeater
  assert.match(app.entry, /todos = \(function\(\) \{ var __d = todos\.slice\(\)/); // produce: copy + reassign
  assert.match(app.entry, /todos = todos\.map\(function\(__t\)/);       // path setter: map + reassign
  assert.match(app.entry, /todos = \(function\(list\)/);               // function setter: reassign
  assert.match(app.entry, /function toggle\(id_\) \{ todos = todos\.map/); // helper param safeName'd (id->id_)
  assert.deepEqual(app.modules, {});                                    // solid-js/store is transpiler semantics, not mirrored JS
  assert.doesNotMatch(app.entry, /import "modules\/store_/);            // no invalid V4 mirror for createStore/produce
});
