/** AOT (C++) back-end goldens (M-AOT-0). Asserts the cpp emitter produces the proven counter shape
 *  from the same TSX the QML back-end consumes: a QVariant state QObject, connect-driven bindings,
 *  setter handlers, nested instances, and a Window-rooted app_main. The pixel-equivalence to the QML
 *  render is proven separately by the twin-render harness (build/aot-counter --grab). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { generateCpp } from "../src/emit/cpp/index.ts";

const COUNTER = `
import { createSignal } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
function Counter(props: { label: string }) {
  const [count, setCount] = createSignal(0);
  return <div class="counter"><button onClick={() => setCount(count() + 1)}>{props.label}: {count()}</button></div>;
}
export function CounterApp() {
  return <Window title="AOT Counter" width={420} height={180} visible>
    <div class="app"><Counter label="A" /><Counter label="B" /><Counter label="C" /></div>
  </Window>;
}
`;

test("cpp: state class is a QVariant QObject with one property per signal + prop", async () => {
  const { header } = await generateCpp(COUNTER, "counter.tsx");
  assert.match(header, /class CounterState : public QObject/);
  assert.match(header, /Q_PROPERTY\(QVariant count READ count WRITE setCount NOTIFY countChanged\)/);
  assert.match(header, /Q_PROPERTY\(QVariant label READ label WRITE setLabel NOTIFY labelChanged\)/);
  assert.match(header, /QVariant m_count = 0;/); // createSignal(0) init
});

test("cpp: reactive binding connects an update lambda to each dependency's NOTIFY", async () => {
  const { source } = await generateCpp(COUNTER, "counter.tsx");
  // text: "" + (label) + ": " + (count) → sq::add fold wrapped in sq::str
  assert.match(source, /setText\(sq::str\(sq::add\(/);
  assert.match(source, /QObject::connect\(state, &CounterState::labelChanged, .*, __upd\)/);
  assert.match(source, /QObject::connect\(state, &CounterState::countChanged, .*, __upd\)/);
});

test("cpp: onClick handler becomes a connect to Button::clicked calling the setter", async () => {
  const { source } = await generateCpp(COUNTER, "counter.tsx");
  assert.match(source, /connect\(v1, &SolidWidgets::Button::clicked, state, \[state\] \{ state->setCount\(sq::add\(state->count\(\), QVariant\(1\)\)\); \}\)/);
});

test("cpp: nested instances set props then build; Window root emits app_main", async () => {
  const { source, main, entry } = await generateCpp(COUNTER, "counter.tsx");
  assert.match(source, /new CounterState\(\);/);
  assert.match(source, /->setLabel\(QVariant\(QStringLiteral\("A"\)\)\);/);
  assert.match(source, /winBox->setCssPrimitive\(QStringLiteral\("window"\)\)/);
  assert.equal(entry.buildFn, "buildCounterApp");
  assert.equal(entry.width, 420);
  assert.ok(main && main.includes("int main(int argc, char **argv)"));
});

test("cpp: unsupported operator fails loudly (M-AOT-1 gap queue)", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, button, Window } from "../../src/solid-qml/runtime";
export function App() {
  const [n] = createSignal(0);
  return <Window width={100} height={100}><div class="x"><button>{n() ** 2}</button></div></Window>;
}`;
  await assert.rejects(() => generateCpp(src, "x.tsx"), /unsupported binary operator "\*\*"/);
});

test("cpp: raw text in a <div> becomes an anonymous CssText child (inherits, no type match)", async () => {
  const src = `
import { div, text, Window } from "../../src/solid-qml/runtime";
export function App() {
  return <Window width={100} height={100}><div class="x">hello</div></Window>;
}`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /new QmlCss::CssText\(\)/);
  assert.match(source, /setCssPrimitive\(QStringLiteral\(""\)\)/);
  assert.match(source, /setText\(sq::str\(sq::add\(QVariant\(QString\(\)\), QVariant\(QStringLiteral\("hello"\)\)\)\)\)/);
});

test("cpp: <text> keeps its class; completion is deferred bottom-up (assemble then complete)", async () => {
  const src = `
import { div, text, Window } from "../../src/solid-qml/runtime";
export function App() {
  return <Window width={100} height={100}><div class="a"><text class="b">hi</text></div></Window>;
}`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /new SolidWidgets::Text\(\)[\s\S]*setCssClass\(sq::classes\(\{"b"\}\)\)/);
  // all sq::complete calls come after all construction (deferred), in reverse creation order.
  const firstComplete = source.indexOf("sq::complete(");
  const lastNew = source.lastIndexOf("new ");
  assert.ok(firstComplete > lastNew, "completes must follow all construction");
});

test("cpp: functional setter updater inlines the prev value (setX(v => v + 1))", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, button, Window } from "../../src/solid-qml/runtime";
function C() {
  const [n, setN] = createSignal(0);
  return <div class="c"><button onClick={() => setN((v) => v + 1)}>go</button></div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /state->setN\(sq::add\(state->n\(\), QVariant\(1\)\)\)/);
});

test("cpp: derived accessor is inlined; its deps drive the binding", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
function C() {
  const [count, setCount] = createSignal(0);
  const doubled = () => count() * 2;
  return <div class="c"><text>{count()} x2 = {doubled()}</text></div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /\(sq::mul\(state->count\(\), QVariant\(2\)\)\)/); // inlined body
  assert.match(source, /&CState::countChanged/); // dep discovered THROUGH the derived
});

test("cpp: mergeProps defaults seed the state member; alias.member resolves to props", async () => {
  const src = `
import { mergeProps } from "solid-js";
import { text, div, Window } from "../../src/solid-qml/runtime";
function G(props: { greeting?: string; name?: string }) {
  const merged = mergeProps({ greeting: "Hello", name: "stranger" }, props);
  return <text class="h1">{merged.greeting}, {merged.name}!</text>;
}
export function App() { return <Window width={100} height={100}><div class="a"><G name="Solid" /></div></Window>; }`;
  const { header, source } = await generateCpp(src, "x.tsx");
  assert.match(header, /QVariant m_greeting = QStringLiteral\("Hello"\);/);
  assert.match(header, /QVariant m_name = QStringLiteral\("stranger"\);/);
  assert.match(source, /state->greeting\(\)/); // merged.greeting → the prop getter
  assert.match(source, /->setName\(QVariant\(QStringLiteral\("Solid"\)\)\)/); // instance override
});

test("cpp: a stateless component instance builds without a state class", async () => {
  const src = `
import { text, div, Window } from "../../src/solid-qml/runtime";
function Inner() { return <text class="b">hi</text>; }
export function App() { return <Window width={100} height={100}><div class="a"><Inner /></div></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /auto \*\w+ = buildInner\(ctx\);/); // no `new InnerState`
  assert.doesNotMatch(source, /new InnerState/);
});

test("cpp: comparison + logical operators map to sq:: helpers", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, button, Window } from "../../src/solid-qml/runtime";
function C() {
  const [t, setT] = createSignal("h1");
  return <div class="c"><button onClick={() => setT(t() === "h1" ? "p" : "h1")}>{t()}</button></div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /sq::strictEq\(state->t\(\), QVariant\(QStringLiteral\("h1"\)\)\)/);
});

test("cpp: ternary binding → sq::truthy(...) ? a : b", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, button, Window } from "../../src/solid-qml/runtime";
function C() {
  const [on, setOn] = createSignal(0);
  return <div class="c"><button onClick={() => setOn(1)}>{on() ? "yes" : "no"}</button></div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /\(sq::truthy\(state->on\(\)\) \? QVariant\(QStringLiteral\("yes"\)\) : QVariant\(QStringLiteral\("no"\)\)\)/);
});
