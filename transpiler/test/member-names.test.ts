// Component members whose author name starts upper-case. QML rejects a property, method or signal
// name whose first character is upper-case ("Property names cannot begin with an upper case
// letter") and the WHOLE type then fails to load — silent at transpile time, fatal at runtime.
// memberName (names/safe.ts) prefixes such a name with `__uc_`, at the declaration and at every
// reference. The prefix keeps the first LETTER lower-case so `on__Uc_XChanged` resolves back to
// the property (Qt lower-cases the first letter of a handler name to find its signal).
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { generate } from "../src/index.ts";
import { memberName, changedHandlerName } from "../src/names/safe.ts";

const PALETTE = `
import { createSignal, createMemo, createEffect, For } from "solid-js";

function Swatch(props) {
  return <text>{props.Label}</text>;
}

export function App() {
  const SIZE_STEPS = [8, 9, 10, 12];
  const [Size, setSize] = createSignal(10);
  const Double = createMemo(() => Size() * 2);
  let Clicks = 0;
  function Bump() { Clicks = Clicks + 1; setSize(SIZE_STEPS[Clicks % SIZE_STEPS.length]); }
  createEffect(() => console.log(Size()));
  return (
    <div>
      <text>{SIZE_STEPS.length} {Size()} {Double()}</text>
      <For each={SIZE_STEPS}>{(s) => <button onClick={() => setSize(s)}>{s}</button>}</For>
      <button onClick={() => { const i = SIZE_STEPS.indexOf(Size()); setSize(SIZE_STEPS[i + 1]); }}>next</button>
      <button onClick={Bump}>bump</button>
      <Swatch Label={String(Size())} />
    </div>
  );
}
`;

/** Every `property … <name>` / `function <name>` the QML declares. */
function declaredNames(qml: string): string[] {
  return [...qml.matchAll(/^\s*(?:readonly\s+)?property\s+\w+\s+([\w$]+)|^\s*function\s+([\w$]+)/gm)].map((m) => m[1] ?? m[2]);
}

test("memberName: an upper-case-initial name gets the __uc_ prefix; every other name is safeName", () => {
  assert.equal(memberName("SIZE_STEPS"), "__uc_SIZE_STEPS");
  assert.equal(memberName("Size"), "__uc_Size");
  assert.equal(memberName("size"), "size");
  assert.equal(memberName("visible"), "visible_"); // still escapes reserved names
  assert.equal(memberName("_Private"), "_Private"); // `_` is not upper-case: already valid
  assert.equal(changedHandlerName("count"), "onCountChanged");
  assert.equal(changedHandlerName("__uc_Size"), "on__Uc_SizeChanged");
});

test("an upper-case component-body const used in JSX and in a handler is a valid, consistently renamed member", async () => {
  const app = await generate(PALETTE, "palette.tsx");
  const qml = app.entry;
  assert.match(qml, /readonly property var __uc_SIZE_STEPS: \[8, 9, 10, 12\]/);
  assert.match(qml, /text: "" \+ \(__uc_SIZE_STEPS\.length\)/); // JSX text binding
  assert.match(qml, /model: __uc_SIZE_STEPS/);                    // <For each>
  assert.match(qml, /onClicked: \(function\(\) \{ var i = __uc_SIZE_STEPS\.indexOf\(__uc_Size\); __uc_Size = __uc_SIZE_STEPS\[i \+ 1\]; \}\)\(\)/); // handler
  // signal, memo, mutable let, helper: declared and referenced under the same name
  assert.match(qml, /property var __uc_Size: 10/);
  assert.match(qml, /readonly property var __uc_Double: __uc_Size \* 2/);
  assert.match(qml, /property var __uc_Clicks: 0/);
  assert.match(qml, /function __uc_Bump\(\) \{ __uc_Clicks = __uc_Clicks \+ 1; __uc_Size = __uc_SIZE_STEPS\[__uc_Clicks % __uc_SIZE_STEPS\.length\]; \}/);
  assert.match(qml, /onClicked: __uc_Bump\(\)/);
  // the effect's dependency handler names the renamed property, in Qt's casing
  assert.match(qml, /target: __self\s*function on__Uc_SizeChanged\(\) \{ __effect0\(\); \}/);
  // a child's upper-case prop: declared and assigned under the same name
  assert.match(qml, /Swatch \{\s*__uc_Label: String\(__uc_Size\)/);
  assert.match(app.components.Swatch, /property var __uc_Label\b/);
  // no declared member starts upper-case, and no bare reference to an original name survives
  for (const src of [qml, ...Object.values(app.components)]) {
    for (const n of declaredNames(src)) assert.doesNotMatch(n, /^[A-Z]/, `declared member ${n}`);
    assert.doesNotMatch(src, /(?<![\w.])(SIZE_STEPS|Size|Double|Clicks|Bump|Label)\b/);
  }
});

test("a function param with the same upper-case name shadows the member alias", async () => {
  const app = await generate(`
export function App() {
  const STEP = 2;
  function next(STEP) { return STEP + 1; }
  return <text>{STEP} {next(1)}</text>;
}`, "shadow.tsx");
  assert.match(app.entry, /readonly property var __uc_STEP: 2/);
  assert.match(app.entry, /function next\(STEP\) \{ return STEP \+ 1; \}/);
});

// The emitted QML through Qt's own QML compiler (the IR builder that raises "Property names cannot
// begin with an upper case letter" when the type loads). qmllint does not report this error, and
// loading with `qml` needs the CSS engine's plugin; qmlcachegen --only-bytecode compiles one file
// without resolving its imports. Skipped where the tool is absent.
function qmlcachegen(): string | null {
  for (const exe of ["/usr/lib/qt6/qmlcachegen", "/usr/lib/qt6/libexec/qmlcachegen", "/usr/lib64/qt6/libexec/qmlcachegen", "qmlcachegen"]) {
    try { execFileSync(exe, ["--help"], { stdio: "ignore", timeout: 5000 }); return exe; } catch { /* next */ }
  }
  return null;
}
const tool = qmlcachegen();

test("the emitted QML of a component with upper-case members compiles (qmlcachegen)", { skip: tool === null ? "no qmlcachegen" : false }, async () => {
  const app = await generate(PALETTE, "palette.tsx");
  const dir = mkdtempSync(join(tmpdir(), "member-names-"));
  try {
    const files: Record<string, string> = { "App.qml": app.entry, ...Object.fromEntries(Object.entries(app.components).map(([n, q]) => [`${n}.qml`, q])) };
    for (const [name, qml] of Object.entries(files)) {
      const file = join(dir, name);
      writeFileSync(file, qml);
      let out = "";
      try {
        out = execFileSync(tool!, ["--only-bytecode", "--resource-path", `/${name}`, "-o", join(dir, `${name}.cpp`), file], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"], timeout: 20000 });
      } catch (e) {
        const err = e as { stdout?: string; stderr?: string };
        assert.fail(`${name} does not compile:\n${err.stderr ?? ""}${err.stdout ?? ""}\n${qml}`);
      }
      assert.doesNotMatch(out, /error/i, out);
    }
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test("qmlcachegen check is not vacuous: an upper-case property is rejected", { skip: tool === null ? "no qmlcachegen" : false }, () => {
  const dir = mkdtempSync(join(tmpdir(), "member-names-"));
  try {
    const file = join(dir, "Bad.qml");
    writeFileSync(file, "import QtQuick\nItem {\n    property var SIZE_STEPS: [1]\n}\n");
    assert.throws(() => execFileSync(tool!, ["--only-bytecode", "--resource-path", "/Bad.qml", "-o", join(dir, "Bad.cpp"), file], { stdio: "pipe", timeout: 20000 }),
      (e: { stderr?: Buffer }) => /cannot begin with an upper case letter/.test(String(e.stderr)));
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
