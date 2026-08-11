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
  assert.match(source, /connect\(v1, &SolidWidgets::Button::clicked, v1, \[=\] \{ state->setCount\(sq::add\(state->count\(\), QVariant\(1\)\)\); \}\)/);
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

test("cpp: <Show> guards each branch with a reactive visible binding (fallback inverted)", async () => {
  const src = `
import { createSignal, Show } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
function C() {
  const [on, setOn] = createSignal(true);
  return <div class="a">
    <button onClick={() => setOn(!on())}>t</button>
    <Show when={on()} fallback={<text class="b">off</text>}><text class="h">on</text></Show>
  </div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /setVisible\(sq::truthy\(state->on\(\)\)\)/);       // truthy branch
  assert.match(source, /setVisible\(!sq::truthy\(state->on\(\)\)\)/);      // fallback inverted
  assert.match(source, /&CState::onChanged, \w+, __vis\)/);               // reactive
});

test("cpp: <Index> emits a rebuild-on-change row factory over the model", async () => {
  const src = `
import { createSignal, Index } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
function L() {
  const [items, setItems] = createSignal(["a", "b"]);
  return <div class="a">
    <button onClick={() => setItems([...items(), "c"])}>add</button>
    <Index each={items()}>{(item, i) => <text>{i}: {item()}</text>}</Index>
  </div>;
}
export function App() { return <Window width={100} height={100}><L /></Window>; }`;
  const { source, header } = await generateCpp(src, "x.tsx");
  assert.match(header, /QVariant m_items = QVariant::fromValue\(QVariantList\{QStringLiteral\("a"\), QStringLiteral\("b"\)\}\)/);
  assert.match(source, /const QVariantList __model = state->items\(\)\.toList\(\);/);
  assert.match(source, /for \(int __i = 0; __i < __model\.size\(\); \+\+__i\)/);
  assert.match(source, /QVariant\(__i\)/); // the index local
  assert.match(source, /&LState::itemsChanged, state, __rebuild/); // reactive rebuild
  // the spread handler builds the new list imperatively
  assert.match(source, /for \(const auto &__e : state->items\(\)\.toList\(\)\) __l\.append\(__e\);/);
});

test("cpp: <For> object rows use safe member lookup; object init → QVariantMap", async () => {
  const src = `
import { createSignal, For } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
function L() {
  const [users] = createSignal([{ name: "Ada" }]);
  return <div class="a"><For each={users()}>{(u) => <text>{u.name}</text>}</For></div>;
}
export function App() { return <Window width={100} height={100}><L /></Window>; }`;
  const { source, header } = await generateCpp(src, "x.tsx");
  assert.match(header, /QVariant::fromValue\(QVariantMap\{\{QStringLiteral\("name"\), QStringLiteral\("Ada"\)\}\}\)/);
  assert.match(source, /sq::get\(__item, "name"\)/);
});

test("cpp: controlled <input> — value binding + textEdited handler; special types fail loud", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, input, Window } from "../../src/solid-qml/runtime";
function C() {
  const [name, setName] = createSignal("Ada");
  return <div class="a"><input class="f" value={name()} onInput={(e) => setName(e.currentTarget.value)} placeholder="n" /></div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /new SolidWidgets::TextField\(\)/);
  assert.match(source, /->setPlaceholder\(QStringLiteral\("n"\)\)/);
  assert.match(source, /->setText\(sq::str\(state->name\(\)\)\)/);            // value → field
  assert.match(source, /&SolidWidgets::TextField::textEdited, state, \[\w+, state\] \{ state->setName\(QVariant\(\w+->text\(\)\)\); \}/); // edit → signal

  const radio = `
import { div, input, Window } from "../../src/solid-qml/runtime";
export function App() { return <Window width={100} height={100}><div class="a"><input type="radio" /></div></Window>; }`;
  await assert.rejects(() => generateCpp(radio, "y.tsx"), /type="radio".*not supported/);
});

test("cpp: follows relative imports across files (multi-file graph)", async () => {
  const files: Record<string, string> = {
    "/app/badge.tsx": `
import { createSignal } from "solid-js";
import { div, text } from "./solid-qml/runtime";
export function Badge(props: { label: string }) {
  const [n] = createSignal(0);
  return <div class="badge"><text>{props.label}: {n()}</text></div>;
}`,
  };
  const entry = `
import { div, Window } from "./solid-qml/runtime";
import { Badge } from "./badge";
export function App() { return <Window width={100} height={100}><div class="a"><Badge label="x" /></div></Window>; }`;
  const { source, header } = await generateCpp(entry, "/app/main.tsx", {
    readFile: async (p) => { if (files[p]) return files[p]; throw new Error("nope"); },
  });
  assert.match(header, /class BadgeState : public QObject/);            // the imported component's state
  assert.match(source, /QQuickItem \*buildBadge\(QQmlContext \*ctx, BadgeState \*state\)/); // its build fn
  assert.match(source, /auto \*\w+ = buildBadge\(ctx, /);               // instantiated from the entry
});

test("cpp: createResource + Suspense — sidecar bridge + loading-gated visibility", async () => {
  const src = `
import { createSignal, createResource, Suspense } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
const fetchRow = async (id) => ({ label: "row " + id });
function R() {
  const [id] = createSignal("42");
  const [row] = createResource(id, fetchRow);
  return <div class="a"><Suspense fallback={<text class="b">loading</text>}><text class="h">{row().label}</text></Suspense></div>;
}
export function App() { return <Window width={100} height={100}><R /></Window>; }`;
  const { header, source } = await generateCpp(src, "x.tsx");
  // resource state props (loading starts true so the fallback shows first)
  assert.match(header, /QVariant m_row_loading = true;/);
  assert.match(header, /Q_PROPERTY\(QVariant row READ row/);
  // Suspense gates the child on "not loading" and the fallback on loading
  assert.match(source, /setVisible\(!sq::truthy\(state->row_loading\(\)\)\)/);
  assert.match(source, /setVisible\(sq::truthy\(state->row_loading\(\)\)\)/);
  // the async fetcher runs on the QJSEngine sidecar with the state QObject bound
  assert.match(source, /QJSEngine \*__js = ctx->engine\(\);/);
  assert.match(source, /__js->newQObject\(state\)/);
  assert.match(source, /state\.row = v; state\.row_loading = false;/);
  assert.match(source, /&RState::idChanged, state, \[__load_row\]\(\) mutable/); // re-fetch on source change
  // resource value member access → safe lookup
  assert.match(source, /sq::get\(state->row\(\), "label"\)/);
});

test("cpp: <textarea> → TextArea (textChanged); <img> → Image (setSrc), static src no state capture", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, img, textarea, Window } from "../../src/solid-qml/runtime";
function C() {
  const [n, setN] = createSignal("x");
  return <div class="a">
    <textarea value={n()} onInput={(e) => setN(e.target.value)} />
    <img class="l" src="/abs/logo.png" />
  </div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /new SolidWidgets::TextArea\(\)/);
  assert.match(source, /&SolidWidgets::TextArea::textChanged, state/);         // textarea uses textChanged
  assert.match(source, /new SolidWidgets::Image\(\)/);
  assert.match(source, /->setSrc\(QUrl\(sq::str\(QVariant\(QStringLiteral\("\/abs\/logo\.png"\)\)\)\)\);/); // static src, one-shot (no `state`)
});

test("cpp: controlled <select> — model/values + activated→setter with picked value", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, select, option, Window } from "../../src/solid-qml/runtime";
function C() {
  const [f, setF] = createSignal("b");
  return <div class="a"><select value={f()} onChange={(e) => setF(e.target.value)}>
    <option value="a">A</option><option value="b">B</option>
  </select></div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /new SolidWidgets::Select\(\)/);
  assert.match(source, /->setModel\(QVariant::fromValue\(QVariantList\{QStringLiteral\("A"\), QStringLiteral\("B"\)\}\)\)/);
  assert.match(source, /->setValues\(QVariant::fromValue\(QVariantList\{QStringLiteral\("a"\), QStringLiteral\("b"\)\}\)\)/);
  assert.match(source, /setCurrentIndex\(\w+->values\(\)\.toList\(\)\.indexOf\(state->f\(\)\)\)/); // controlled
  assert.match(source, /&SolidWidgets::Select::activated, state, \[\w+, state\]\(int __i\) \{ state->setF\(\w+->values\(\)\.toList\(\)\.value\(__i\)\); \}/);
});

test("cpp: props.children slotting — kids built in parent scope, appended at the marker", async () => {
  const src = `
import { div, text, Window } from "../../src/solid-qml/runtime";
function Card(props) { return <div class="card">{props.children}</div>; }
function C() { return <div class="a"><Card><text class="t">hi</text></Card></div>; }
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /QQuickItem \*buildCard\(QQmlContext \*ctx, const QList<QQuickItem \*> &__slot\)/); // slot param
  assert.match(source, /for \(auto \*__s : __slot\) sq::append\(\w+, __s\);/);                            // slotted at the marker
  assert.match(source, /QList<QQuickItem \*> __slot_\w+;[\s\S]*buildCard\(ctx, __slot_\w+\)/);            // kids collected + passed
});

test("cpp: createContext/useContext — provider method + injected consumer, reactive across the boundary", async () => {
  const src = `
import { createContext, useContext, createSignal } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
const Ctx = createContext();
function Provider(props) {
  const [count, setCount] = createSignal(props.count || 0);
  const counter = { count, increment: () => setCount(count() + 1) };
  return <Ctx.Provider value={counter}>{props.children}</Ctx.Provider>;
}
function Display() { const c = useContext(Ctx); return <text class="d">Count: {c.count()}</text>; }
function Bump() { const c = useContext(Ctx); return <button class="b" onClick={c.increment}>+</button>; }
function Demo() { return <Provider count={5}><div class="a"><Display /><Bump /></div></Provider>; }
export function App() { return <Window width={100} height={100}><Demo /></Window>; }`;
  const { header, source } = await generateCpp(src, "x.tsx");
  // provider exposes increment as a Q_INVOKABLE method operating on its own state (bare `this`)
  assert.match(header, /Q_INVOKABLE void increment\(\) \{ setCount\(sq::add\(count\(\), QVariant\(1\)\)\); \}/);
  // consumers take the injected provider State pointer
  assert.match(source, /QQuickItem \*buildDisplay\(QQmlContext \*ctx, ProviderState \*__ctx_Ctx\)/);
  // consumer reads go through the injected pointer, and the binding reacts to the provider's NOTIFY
  assert.match(source, /__ctx_Ctx->count\(\)/);
  assert.match(source, /QObject::connect\(__ctx_Ctx, &ProviderState::countChanged, \w+, __upd\)/);
  assert.match(source, /__ctx_Ctx->increment\(\);/);                          // button handler
  // ContextDemo creates the provider State, injects it into both consumers
  assert.match(source, /buildDisplay\(ctx, \w+_st\)/);
  assert.match(source, /buildBump\(ctx, \w+_st\)/);
});

test("cpp: <input type=checkbox> → Checkbox; role=switch → Toggle; toggled→setter", async () => {
  const src = `
import { createSignal } from "solid-js";
import { div, input, Window } from "../../src/solid-qml/runtime";
function C() {
  const [on, setOn] = createSignal(true);
  return <div class="a">
    <input type="checkbox" checked={on()} onChange={(e) => setOn(e.target.checked)} />
    <input type="checkbox" role="switch" checked={on()} onChange={(e) => setOn(e.target.checked)} />
  </div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  assert.match(source, /new SolidWidgets::Checkbox\(\)/);
  assert.match(source, /new SolidWidgets::Toggle\(\)/);                         // role="switch"
  assert.match(source, /->setChecked\(sq::truthy\(state->on\(\)\)\)/);         // controlled
  assert.match(source, /&SolidWidgets::Checkbox::toggled, state, \[\w+, state\] \{ state->setOn\(QVariant\(\w+->checked\(\)\)\); \}/);
});

test("cpp: <Switch>/<Match> — first-match-wins guards + fallback", async () => {
  const src = `
import { createSignal, Switch, Match } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
function C() {
  const [n] = createSignal(1);
  return <div class="a"><Switch fallback={<text class="o">many</text>}>
    <Match when={n() === 1}><text class="a">one</text></Match>
    <Match when={n() === 2}><text class="b">two</text></Match>
  </Switch></div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  // first match: just its when
  assert.match(source, /setVisible\(sq::truthy\(sq::strictEq\(state->n\(\), QVariant\(1\)\)\)\)/);
  // second match: its when AND NOT the first
  assert.match(source, /setVisible\(\(!sq::truthy\(sq::strictEq\(state->n\(\), QVariant\(1\)\)\)\) && \(sq::truthy\(sq::strictEq\(state->n\(\), QVariant\(2\)\)\)\)\)/);
  // fallback: NOT any match
  assert.match(source, /setVisible\(\(!sq::truthy\(sq::strictEq\(state->n\(\), QVariant\(1\)\)\)\) && \(!sq::truthy\(sq::strictEq\(state->n\(\), QVariant\(2\)\)\)\)\)/);
});

test("cpp: nested control flow combines guards (Suspense › Show)", async () => {
  const src = `
import { createSignal, createResource, Suspense, Show } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
const f = async (id) => ({ ok: id });
function C() {
  const [id] = createSignal("1");
  const [r] = createResource(id, f);
  return <div class="a">
    <Suspense fallback={<text class="l">loading</text>}>
      <Show when={r()} fallback={<text class="e">empty</text>}><text class="v">{r().ok}</text></Show>
    </Suspense>
  </div>;
}
export function App() { return <Window width={100} height={100}><C /></Window>; }`;
  const { source } = await generateCpp(src, "x.tsx");
  // the inner <Show> child is visible only when NOT loading AND the resource is truthy (guards AND'd)
  assert.match(source, /setVisible\(\(!sq::truthy\(state->r_loading\(\)\)\) && \(sq::truthy\(state->r\(\)\)\)\)/);
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
