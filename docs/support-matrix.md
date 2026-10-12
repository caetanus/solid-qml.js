# Support matrix

What solid-qml 0.3.0-preview.1 supports, and what it does not. Every row points at a test, an
example or the code that backs it.

Legend: ✅ supported · 🟡 partial · ❌ unsupported, **rejected at compile time** (the transpiler
prints an error) or simply absent · ⚠️ unsupported and **accepted without an error, but wrong at
runtime**. Avoid the ⚠️ rows: nothing warns you about them.

Tables marked *generated* are produced from the code by `scripts/support-matrix.mjs`, and
`transpiler/test/support-matrix-doc.test.ts` fails when they drift. To refresh them, run
`node --import tsx scripts/support-matrix.mjs` from the checkout. The other tables are written by
hand and were checked against the code on 2026-10-11.

- [Solid APIs, JSX and statements](#solid-apis-jsx-and-statements)
- [Elements and their attributes](#elements-and-their-attributes)
- [Widgets](#widgets)
- [CSS](#css)
- [Runtime shims and capability profiles](#runtime-shims-and-capability-profiles)
- [npm packages and the V4 engine](#npm-packages-and-the-v4-engine)
- [Platforms](#platforms)

## Solid APIs, JSX and statements

Each row is a small program run through the real transpiler, and the "Transpiler today" column is
its actual verdict. For the semantics of what is supported (for example the `<For>` row-identity
contract and its `key` extension), see [`SOLID-PARITY.md`](../SOLID-PARITY.md).

The rules that catch people most often:

- **Signals are read by calling them, stores are not.** `count()` reads a signal; a store is read
  like a plain object or array (`items.length`, `items.filter(…)`, `todo.done`), exactly as in
  Solid. Writing `items()` for a store is a mistake the transpiler does not catch
  (`examples/todomvc.tsx` shows both).
- **Input events** carry the element as `e.currentTarget` (and `e.target`):
  `onInput={(e) => setTitle(e.currentTarget.value)}`, `onChange={(e) => setOn(e.currentTarget.checked)}`
  (`examples/fetch.tsx`, `examples/widgets.tsx`).
- An **effect's dependencies** are what its body reads: signals, memos, props, store fields,
  context values, and properties of `QObject`s. A component's own plain (non-reactive) values are
  never dependencies (`transpiler/test/effect-reactive-deps.test.ts`).
- `onClick` takes an **expression** body (`() => setN(n() + 1)`) or a **named function** of the
  component (`onClick={save}`). The `fn`-kind events (`onInput`, `onChange`, `onKeyDown`…) also
  accept a block body.
- Component code (including module-level helper functions in a component's file) is translated
  statement by statement into QML JavaScript, with only the names rewritten: every statement V4
  runs (`if`, all loops, `switch`, `try`/`catch`/`finally`, `throw`, labels, destructuring) and
  every expression form (`new`, RegExp literals, `?.`, `??`, spread) is emitted as written. Two
  things V4 lacks are handled at compile time: object spread `{ ...a }` is lowered to
  `Object.assign`, and object rest (`{ a, ...rest }`), `class`, generators and `BigInt` are
  rejected with an error. Code that needs those belongs in an npm package, which is mirrored and
  executed unmodified (see [npm packages](#npm-packages-and-the-v4-engine)).

*Generated:*

<!-- generated:probes:begin (scripts/support-matrix.mjs — do not edit by hand) -->
| Area | Construct | Status | Transpiler today (generated) | Notes / evidence |
|---|---|---|---|---|
| reactivity | `createSignal` | ✅ | accepted | `transpiler/test/symbols.test.ts`, `examples/counter.tsx` |
| reactivity | `createMemo` | ✅ | accepted | `transpiler/test/derived.test.ts` |
| reactivity | `createEffect` (deps = signals, props, store fields, context, QObject properties read in the body) | ✅ | accepted | `transpiler/test/effect-reactive-deps.test.ts`; a component's own plain (non-reactive) values are never dependencies |
| reactivity | `onCleanup` inside an effect | ✅ | accepted | the effect's own cleanups run (newest first) before each re-run and on unmount, before the component's — `transpiler/test/reactivity-api.test.ts`; runtime probe in the M2 commit |
| reactivity | effect returning a cleanup function | ✅ | accepted | as in Solid, the return value is the next run's `prev` (`createEffect((prev) => …, init)`), not a cleanup — use `onCleanup` |
| reactivity | `untrack` | ✅ | accepted | in bindings the read runs inside a signal handler (Qt records no dependency there); in effects it is left out of the dependencies — `transpiler/test/reactivity-api.test.ts` |
| reactivity | `batch` | 🟡 | accepted | effects (and `createComputed`/`createRenderEffect`) re-run once, after the batch (shared runtime module `modules/solid-batch.mjs`). Bindings and memos still update on each write, so a memo that reads a signal through `untrack` can keep a value from an intermediate write |
| reactivity | `on(deps, fn)` | ✅ | accepted | only `deps` are dependencies; `fn(input, prevInput, prev)` runs untracked; `{ defer: true }`; also in `createMemo(on(…))` — `transpiler/test/reactivity-api.test.ts` |
| reactivity | `createComputed` / `createRenderEffect` | ✅ | accepted | same computation as `createEffect`, first run at its position in the component body (before `onMount`/`createEffect`) |
| reactivity | `createSelector` | ✅ | accepted | `sel(k)` is `k === s()` (or `fn(k, s())`), reactive on the source |
| reactivity | `createRoot` | ❌ | rejected: ``createRoot(…)`: createRoot is not supported natively — a component's computations live exactly as long as its QML object (there is no detac` | a component's computations live as long as its QML object; there is no detached owner |
| reactivity | `createMemo((prev) => …, init)` | ✅ | accepted | `transpiler/test/reactivity-api.test.ts` |
| reactivity | a primitive (`createEffect`, `createSignal`, `on`…) called outside a component's top level | ❌ | rejected: ``createEffect(…)`: createEffect is only supported at the top level of a component body (`const x = createEffect(…)` / `createEffect(…);`) — ` | declare it at the top level of the component body |
| resources | `createResource(source, fetcher)` | ✅ | accepted | `examples/fetch.tsx`; value + `loading`/`error`, re-fetch on source change |
| resources | `createResource` with an inline fetcher | ❌ | rejected: `createResource supports `const [r] = createResource(<signal>, <fetcherIdent>)` only` | name the fetcher (a module-level function) |
| stores | `createStore` + value / `prev => next` / `produce` / `(pred, "key", updater)` setters | ✅ | accepted | `examples/todomvc.tsx`, `transpiler/test/effect-reactive-deps.test.ts` |
| stores | path setter `set("a", 0, "b", v)` | ❌ | rejected: `store setter form not supported for st` |  |
| stores | `reconcile` | ❌ | rejected: `store setter form not supported for st` |  |
| stores | `createMutable` | ❌ | rejected: ``createMutable(…)`: createMutable is not supported natively — QML bindings cannot observe in-place mutation of a plain object. Use createSto` | QML bindings cannot observe in-place mutation: use `createStore` |
| control flow | `<Show when fallback>` | ✅ | accepted | lazy (mounted only while `when` holds) — `transpiler/test/control.test.ts`, `transpiler/test/silent-drops.test.ts` |
| control flow | `<Switch>`/`<Match>` | ✅ | accepted | lazy; `transpiler/test/control.test.ts` |
| control flow | `<For>` (reference identity) + index accessor | ✅ | accepted | `transpiler/test/for-index-accessor.test.ts`; `meson test for-identity for-identity-virtual` |
| control flow | `<For key="id">` (reuse rows by a field; native-only extension) | ✅ | accepted | `transpiler/test/for-key.test.ts`; contract in SOLID-PARITY.md |
| control flow | `<Index>` | ✅ | accepted | `examples/index-list.tsx` |
| control flow | `<Suspense>` | ✅ | accepted | `examples/fetch.tsx` |
| control flow | `<Dynamic component="tag">` (intrinsic tag) | ✅ | accepted | `examples/dynamic.tsx` |
| control flow | `<Dynamic component={Comp}>` (component value) | ✅ | accepted | candidates written as a conditional over component names (and tag strings): one lazily mounted branch each, props and children forwarded — `transpiler/test/components-jsx.test.ts` |
| control flow | `<Dynamic component={c()}>` with a component held in a signal/variable | ❌ | rejected: `<Dynamic component={…}>: the component must be visible at compile time — write it as a conditional over component names (`cond ? CardA : Car` | write the selector as a conditional over component names |
| control flow | `<Portal>` | ❌ | rejected: `unknown component or control flow: Portal` |  |
| control flow | `<ErrorBoundary>` | ❌ | rejected: `unknown component or control flow: ErrorBoundary` |  |
| control flow | `lazy()` | ❌ | rejected: `unknown component or control flow: L` | out of scope |
| components | props, `props.children`, callbacks | ✅ | accepted | `examples/children.tsx`, `transpiler/test/props.test.ts` |
| components | `mergeProps` / `splitProps` (local part) | ✅ | accepted | `examples/props-helpers.tsx` |
| components | context: a provider component wrapping `props.children`, `useContext` in descendants | 🟡 | accepted | `examples/context.tsx`; nested/multi-instance providers untested |
| components | context: `<Ctx.Provider>` written inline in the consumer's parent | ❌ | rejected: `consumer Child uses context Ctx with no provider in scope` | use the provider-component form above |
| components | spread props on a component `<C {...o} />` | ✅ | accepted | every prop the component declares reads the sources last to first (first defined value wins); `{...props}` forwards — `transpiler/test/components-jsx.test.ts` |
| components | spread attributes on an element `<div {...o}>` | ❌ | rejected: `<div>: spread attributes ({...props}) are not supported natively — pass each attribute explicitly` |  |
| components | `children()` helper | 🟡 | accepted | `{c()}` in the root element slots the children; inspecting them (`c.toArray()`) fails at compile time |
| components | `mergeProps(props, { … })` (an object after props overrides) | ❌ | rejected: `mergeProps(props, { … }): an object after props OVERRIDES the passed props — not supported natively; put the defaults first: mergeProps({ … ` | put the defaults first: `mergeProps({ … }, props)` |
| components | destructured props `function C({ a })` | ❌ | rejected: `a component's props must be one parameter (`function C(props)`): destructuring it (`({ a, b })`) reads each prop once, untracked — not suppo` | use `props.a` (destructuring reads each prop once, untracked) |
| components | `ref={el}` (variable) | ✅ | accepted | `examples/refs.tsx`, `transpiler/test/ref-render.test.ts` |
| components | `ref={(e) => …}` (callback) | ❌ | rejected: `<div> ref must be a variable: ref={x}` |  |
| JSX | inline `style=` | ❌ | rejected: `<div> does not support `style` natively (it would be silently dropped); supported: class, classList, ref, onClick, onKeyDown, onContextMenu,` | use a class and CSS |
| JSX | `classList={{…}}` | ✅ | accepted | `transpiler/test/qml.test.ts` |
| JSX | `use:` directives | ❌ | rejected: `<div> does not support `use:d` natively (it would be silently dropped); supported: class, classList, ref, onClick, onKeyDown, onContextMenu,` |  |
| JSX | nested fragments | ✅ | accepted |  |
| JSX | events beyond the contract (`onMouseMove`, `onDblClick`, `onFocus`, `onSubmit`…) | ❌ | rejected: `<div> does not support `onDblClick` natively (it would be silently dropped); supported: class, classList, ref, onClick, onKeyDown, onContext` | see the element table for each tag's events |
| handlers | `onClick` with an expression body | ✅ | accepted |  |
| handlers | `onClick` with a block body `() => { … }` | ✅ | accepted | the block runs as its own function — `transpiler/test/components-jsx.test.ts` |
| handlers | `onClick={go}` (named function in the component, `if`/`for…of` inside) | ✅ | accepted | `transpiler/test/module-fn-positions.test.ts` |
| handlers | block body in `fn`-kind events (`onInput`, `onChange`, `onKeyDown`…) with `if` | ✅ | accepted | `examples/widgets.tsx` |
| handlers | `async` named handler | ❌ | rejected: `only inline arrow handlers (or a context fn-member reference) are supported in this plan` | call an async module-level function from an expression-bodied handler, or use `createResource` |
| statements | `while` / `do…while` in component code | ✅ | accepted | `transpiler/test/statements.test.ts` (emitted methods are executed) |
| statements | classic `for (;;)` in component code | ✅ | accepted | `let`/`const` heads keep per-iteration bindings — `transpiler/test/statements.test.ts` |
| statements | `switch` in component code | ✅ | accepted | `transpiler/test/statements.test.ts` |
| statements | `try`/`catch` in component code | ✅ | accepted | with `finally`, `throw`, labelled `break`/`continue` — `transpiler/test/statements.test.ts` |
| statements | destructuring (declarations, params, `for…of` heads; defaults; array rest) | ✅ | accepted | `transpiler/test/statements.test.ts` |
| statements | object rest `const { a, ...rest } = o` | ❌ | rejected: `object rest in a destructuring pattern (`{ a, ...rest }`) is not supported by the V4 engine — read the fields you need explicitly` | V4 has no object rest: read the fields you need |
| statements | `class` in component code | ❌ | rejected: `unsupported statement in component code: ClassDeclaration — declare the class in a plain .ts module and import it (loaded as a JS module, un` | declare the class in a plain `.ts` module and import it |
| modules | a helper function imported from a relative non-component module (`import { f } from "./util"`) | ✅ | accepted | a plain module (no JSX, no Solid import — checked transitively) is mirrored as a JS module next to the QML, one shared instance — `transpiler/test/modules.test.ts` |
| modules | a non-component binding imported from a module that also has JSX or imports solid-js | ❌ | rejected: `import { f } from "./ui": ui.tsx also contains JSX (components) — a non-component binding can only be imported from a plain module; move it ` | move the helper into a plain module |
| modules | module-level reactive state (`const [n, setN] = createSignal(0)` outside a component) | ❌ | rejected: `module-level createSignal(…) (n, setN): reactive state created outside a component is not supported natively — create it in a component and ` | create it in a component and share it through a context provider or props |
| modules | module-level `let`/`var` read by a component | ❌ | rejected: `module-level `let total`: module state is not shared between components natively (each component gets its own copy) — make it a const, move ` | each component would get its own copy: use a const, or a plain `.ts` module (shared) |
| modules | a module-level const mutated in place (`LIST.push(…)`, `MAP.set(…)`) | ❌ | rejected: `module-level const SEEN is mutated (.push(…)) — each component instance gets its own copy, so the change is not shared as in JS: move SEEN i` | module consts are per component instance: keep shared mutable state in a plain `.ts` module |
| expressions | RegExp literal in component code | ✅ | accepted | `transpiler/test/statements.test.ts` |
| expressions | RegExp literal in a module-level constant | ✅ | accepted | module consts with any initializer are emitted (one value per component instance) — `transpiler/test/modules.test.ts` |
| expressions | `new` expressions in component code (`new Date()`, `new Map()`…) | ✅ | accepted | `transpiler/test/statements.test.ts` |
| expressions | a module-level constant initialised with `new` (`const M = new Map()`) | ✅ | accepted | `transpiler/test/modules.test.ts`; mutating it in place fails at compile time (see above) |
| modules | an `async` module-level function called from a handler | ✅ | accepted | Babel lowers it to a const; emitted with its helpers — `transpiler/test/modules.test.ts` |
| expressions | optional chaining `a?.b` | ✅ | accepted | `?.`, `?.[k]`, `?.()` — `transpiler/test/statements.test.ts` |
| expressions | object spread `{ ...a, x: 1 }` | ✅ | accepted | lowered to `Object.assign({}, …)` (V4 has no object spread) — `transpiler/test/statements.test.ts` |
| expressions | nullish coalescing `a ?? b` | ✅ | accepted |  |
| expressions | `BigInt` literal | ❌ | rejected: `unsupported expression in component code: BigIntLiteral — the V4 engine has no BigInt` | V4 has no BigInt |
<!-- generated:probes:end -->

## Elements and their attributes

Every built-in element has an **attribute contract** (`transpiler/src/emit/attrs.ts`). An attribute
outside it is a compile error, so nothing is silently dropped. The exceptions only warn: `data-*`,
and `role`/`tabIndex`/`aria-*` on a widget that already has native semantics. `aria-expanded` has
no Qt Quick equivalent and is never mapped. `transpiler/test/attrs-contract.test.ts` checks every
tag against every attribute.

Value forms: `expr` any expression (reactive) · `lit` string literal only (`lit: a/b` = one of
those values) · `fn` inline function · `handler` inline function or a named component function ·
`bool` attribute or expression · `flag` bare attribute only · `ident` a variable (`ref={el}`) ·
`object` object literal (`classList`) · `elem` an element · `numlit` numeric literal · `key` field
name or field accessor.

How `tabIndex`, `role` and `aria-*` map to Qt Quick's focus chain and `Accessible` attached type is
documented in [keyboard & accessibility](keyboard-accessibility.md).

*Generated* (the "Emits" column is read back from the transpiler's output for each tag):

<!-- generated:elements:begin (scripts/support-matrix.mjs — do not edit by hand) -->
| Tag | Emits (QML type) | Attributes (value form) |
|---|---|---|
| `div`, `section`, `nav`, `li`, `ul`, `label`, `table`… (any other lowercase tag) | `W.Div` | `class` expr, `classList` object, `ref` ident, `onClick` handler, `onKeyDown` fn, `onContextMenu` fn, `draggable` flag, `dragData` expr, `onDrop` fn, `title` expr, `tabIndex` numlit, `role` lit, `aria-label` expr, `aria-description` lit, `aria-describedby` lit, `aria-checked` expr, `aria-pressed` expr, `aria-selected` expr, `aria-hidden` expr, `aria-disabled` expr, `onMouseEnter` handler, `onMouseLeave` handler, `onScroll` fn |
| `text`, `span`, `h1`, `h2`, `h3`, `h4`, `h5`, `h6`, `p`, `cite`, `bio` | `W.Text` | `class` expr, `classList` object, `ref` ident, `onClick` handler, `onKeyDown` fn, `onContextMenu` fn, `draggable` flag, `dragData` expr, `onDrop` fn, `title` expr, `tabIndex` numlit, `role` lit, `aria-label` expr, `aria-description` lit, `aria-describedby` lit, `aria-checked` expr, `aria-pressed` expr, `aria-selected` expr, `aria-hidden` expr, `aria-disabled` expr |
| `button` | `W.Button` | `class` expr, `classList` object, `onClick` handler, `type` lit: submit/button, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `img` | `W.Image` | `class` expr, `classList` object, `src` expr, `alt` expr |
| `input` (type text/password/email/search/tel/url) | `W.TextField` | `class` expr, `classList` object, `ref` ident, `type` lit: text/password/email/search/tel/url/checkbox/radio/range/number/date, `value` expr, `placeholder` expr, `onInput` fn, `onChange` fn, `onKeyDown` fn, `onKeydown` fn, `disabled` bool, `readOnly` bool, `readonly` bool, `maxLength` expr, `maxlength` expr, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `input type=checkbox` | `W.Checkbox` | `class` expr, `classList` object, `ref` ident, `type` lit: text/password/email/search/tel/url/checkbox/radio/range/number/date, `checked` expr, `onChange` fn, `disabled` bool, `role` lit: switch, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `input type=radio` | `Css.CssFill` → `T.RadioButton` → `Item` | `class` expr, `classList` object, `ref` ident, `type` lit: text/password/email/search/tel/url/checkbox/radio/range/number/date, `checked` expr, `onChange` fn, `disabled` bool, `name` lit, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `input type=range` | `W.Slider` | `class` expr, `classList` object, `ref` ident, `type` lit: text/password/email/search/tel/url/checkbox/radio/range/number/date, `value` expr, `onInput` fn, `onChange` fn, `min` expr, `max` expr, `step` expr, `disabled` bool, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `input type=number` | `W.SpinBox` | `class` expr, `classList` object, `ref` ident, `type` lit: text/password/email/search/tel/url/checkbox/radio/range/number/date, `value` expr, `onChange` fn, `min` expr, `max` expr, `step` expr, `disabled` bool, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `input type=date` | `W.DateField` | `class` expr, `classList` object, `type` lit: text/password/email/search/tel/url/checkbox/radio/range/number/date, `value` expr, `onChange` fn, `disabled` bool, `min` expr, `max` expr, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `textarea` | `W.TextArea` | `class` expr, `classList` object, `ref` ident, `value` expr, `placeholder` expr, `onInput` fn, `onChange` fn, `disabled` bool, `readOnly` bool, `readonly` bool, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `select` | `W.Select` | `class` expr, `classList` object, `value` expr, `onChange` fn, `disabled` bool, `options` expr, `aria-label` expr, `aria-description` lit, `aria-describedby` lit |
| `option` | consumed by `select` | `value` lit |
| `progress` | `W.Progress` | `class` lit, `classList` object, `value` expr, `max` expr |
| `fieldset` | `W.Fieldset` | `class` lit, `classList` object |
| `legend` | consumed by `fieldset` | `class` lit, `classList` object |
| `dialog` | `W.Dialog` | `class` lit, `classList` object, `open` expr, `title` expr, `onClose` fn |
| `details` | `W.Details` → `W.Text` | `class` lit, `classList` object, `open` expr |
| `summary` | consumed by `details` | `class` lit, `classList` object |
| `ToolBar` | `W.ToolBar` | `class` lit, `classList` object |
| `Swipe` | `W.Swipe` | `class` lit, `classList` object, `right` expr, `farRight` expr, `left` expr, `farLeft` expr, `onSwipe` fn |
| `TabBar` | `W.TabBar` → `W.TabButton` | `class` lit, `classList` object, `current` expr, `onChange` fn |
| `TabButton` | consumed by `TabBar` | none |
| `SplitView` | `W.SplitView` | `class` lit, `classList` object, `orientation` lit: horizontal/vertical |
| `Drawer` | `W.Drawer` | `class` lit, `classList` object, `open` expr, `onClose` fn, `size` expr, `edge` lit: left/right/top/bottom |
| `StackView` | `W.StackView` → `W.Div` | `class` lit, `classList` object, `current` expr |
| `SwipeView` | `W.SwipeView` | `class` lit, `classList` object, `current` expr, `onChange` fn |
| `PageIndicator` | `W.PageIndicator` | `class` lit, `classList` object, `count` expr, `current` expr |
| `RangeSlider` | `W.RangeSlider` | `class` lit, `classList` object, `first` expr, `second` expr, `min` expr, `max` expr, `step` expr, `onChange` fn, `disabled` bool |
| `Dial` | `W.Dial` | `class` lit, `classList` object, `value` expr, `min` expr, `max` expr, `step` expr, `onChange` fn, `disabled` bool |
| `Tumbler` | `W.Tumbler` | `class` lit, `classList` object, `options` expr, `value` expr, `onChange` fn, `disabled` bool |
| `DelayButton` | `W.DelayButton` | `class` lit, `classList` object, `delay` expr, `onActivated` fn, `disabled` bool |
| `BusyIndicator` | `W.BusyIndicator` | `class` lit, `classList` object, `running` expr |
| `RoundButton` | `W.RoundButton` | `class` lit, `classList` object, `onClick` fn, `disabled` bool |
| `ToolButton` | `W.ToolButton` | `class` lit, `classList` object, `onClick` fn, `disabled` bool |
| `ToolSeparator` | `W.ToolSeparator` | `class` lit, `classList` object |
| `Menu` | `Css.CssRect` → `W.Button` → `W.Menu` | `class` lit, `ref` ident, `trigger` elem, `onClose` fn |
| `Menu` inside `MenuBar` | consumed by `MenuBar` | `title` expr |
| `MenuItem` | consumed by `Menu` | `onClick` fn, `disabled` bool, `checked` expr |
| `MenuSeparator` | consumed by `Menu` | none |
| `MenuBar` | `Platform.MenuBar` → `Platform.Menu` → `Platform.MenuItem` | none |
| `ContextMenu` | `Item` → `MouseArea` → `W.Menu` | `class` lit, `onClose` fn, `suppressWhen` fn |
| `TreeView` | `W.TreeView` | `class` lit, `classList` object, `data` expr, `onSelect` fn |
| `ListView` | `W.ListView` | `class` lit, `classList` object, `data` expr, `onSelect` fn |
| `TableView` | `W.TableView` | `class` lit, `classList` object, `data` expr, `columns` expr, `onSelect` fn |
| `Tray` | `W.Tray` | `icon` expr, `tooltip` expr, `onActivate` fn |
| `Chart` | `WChart.Chart` (opt-in module `solidqml.Widgets.Chart`) | `class` lit, `accent` expr, `xMax` expr |
| `Scene3D` | `WScene3D.Scene3D` (opt-in module `solidqml.Widgets.Scene3D`) | `class` lit, `modelColor` expr, `spinning` expr |
| `Surface` | `WSurface.Surface` (opt-in module `solidqml.Widgets.Surface`) | `class` lit, `heightMap` expr, `valueMin` expr, `valueMax` expr |
| `MediaPlayer` | `WMedia.MediaPlayer` (opt-in module `solidqml.Widgets.Media`) | `class` lit, `src` expr, `subtitles` expr, `autoplay` expr |
| `WebView` | `WWeb.WebView` (opt-in module `solidqml.Widgets.Web`) | `class` lit, `src` expr, `html` expr, `remoteContent` expr |
| `RichText` | `WRich.RichText` (opt-in module `solidqml.Widgets.RichText`) | `class` expr, `classList` object, `html` expr, `variant` expr, `onChange` fn |
| `DocumentEditor` | `WRich.DocumentEditor` (opt-in module `solidqml.Widgets.RichText`) | `class` expr, `classList` object, `ref` ident, `toolbar` bool, `onModifiedChanged` fn, `onContentChanged` fn, `onStylesChanged` fn, `pageWidth` expr, `pageMargins` expr, `zoom` expr, `onZoomRequested` fn, `onHeadingsChanged` fn, `onBlocksLoaded` fn, `onBlockChanged` fn, `onBlockRemoved` fn, `onMarkClicked` fn, `onMarkContextMenu` fn, `onMarkHovered` fn |
| `CodeEditor` | `WCode.CodeEditor` (opt-in module `solidqml.Widgets.Code`) | `class` lit, `language` expr, `text` expr |
| `Shortcut` | `Shortcut` | `keys` expr, `enabled` expr, `onActivated` fn |
| `Calendar` | `W.Calendar` | `class` expr, `classList` object, `value` expr, `onChange` fn |
| `Window` | consumed by `(app root)` | `title` expr, `width` expr, `height` expr, `visible` flag |
| `Show` | `Repeater` → `W.Text` | `when` expr, `fallback` elem |
| `For` | `Css.CssRepeater` → `W.Text` | `each` expr, `key` key |
| `Index` | `Repeater` → `W.Text` | `each` expr |
| `VirtualList` | `W.Div` → `ListView` → `WheelHandler` | `class` expr, `classList` object, `ref` ident, `each` expr, `onEndReached` fn, `onKeyDown` fn |
| `Switch` | `Css.CssIncubator` → `W.Text` | `fallback` elem |
| `Match` | consumed by `Switch` | `when` expr |
| `Dynamic` | `Css.CssText` | `component` expr |
| `Suspense` | `W.Text` | `fallback` elem |
<!-- generated:elements:end -->

Form controls have no default look: an `<input type="checkbox">` or `type="radio"` with no CSS
has zero size and does not show. Give it a size and style its indicator, for example:

```css
.row input { width: 18px; height: 18px; }
.row input .indicator { border: 2px solid #8899aa; border-radius: 4px; background: #fff; }
.row input:checked .indicator { background: #41cd52; border-color: #41cd52; }
.row input .indicator-glyph { color: #fff; font-size: 11px; }
```

`examples/widgets.css` styles every control this way.

Any other lowercase tag (`section`, `ul`, `li`, `table`, `label`, `canvas`…) is a generic box
(`W.Div`) that CSS type selectors can target. It takes the `div` contract, so `<a href>`,
`<form onSubmit>` and `<svg>` attributes are rejected. `<input>` types other than the eleven listed
(`file`, `color`…) are rejected.

## Widgets

All built-in widgets are **C++** types in `src/widgets/`. Only the opt-in modules below are `.qml`.
Each opt-in module needs its Qt module installed (QtGraphs for `Chart`/`Surface`, QtQuick3D for
`Scene3D`, QtMultimedia for `MediaPlayer`, QtWebEngine for `WebView`) and is imported only by
components that use it.

*Generated:*

<!-- generated:widgets:begin (scripts/support-matrix.mjs — do not edit by hand) -->
**C++ types registered in `solidqml.Widgets`** (43, `src/widgets/solidwidgets.cpp`): `Div`, `Text`, `Image`, `Button`, `RoundButton`, `Fieldset`, `StackView`, `Progress`, `BusyIndicator`, `Checkbox`, `Toggle`, `Slider`, `RangeSlider`, `TextField`, `TextArea`, `SpinBox`, `Dial`, `DelayButton`, `Tumbler`, `ToolButton`, `ToolBar`, `ToolSeparator`, `TabBar`, `SplitView`, `SwipeView`, `PageIndicator`, `Drawer`, `ToolTip`, `Details`, `Swipe`, `Dialog`, `Select`, `MonthGrid`, `Calendar`, `DateField`, `Tray`, `ListView`, `TableView`, `TreeView`, `TabButton`, `Menu`, `MenuItem`, `MenuSeparator`.

**Opt-in modules** (`.qml`, imported only by an app that uses the tag; `qml/solidqml/Widgets/*/qmldir`):

| Module | Types |
|---|---|
| `solidqml.Widgets.Chart` | `Chart` |
| `solidqml.Widgets.Code` | `CodeEditor` |
| `solidqml.Widgets.Media` | `MediaPlayer` |
| `solidqml.Widgets.RichText` | `RichText`, `DocumentEditor` |
| `solidqml.Widgets.Scene3D` | `Scene3D` |
| `solidqml.Widgets.Surface` | `Surface` |
| `solidqml.Widgets.Web` | `WebView` |
<!-- generated:widgets:end -->

Status of the widget set as a whole:

| Area | Status | Evidence / notes |
|---|---|---|
| Keyboard focus chain, Tab ring (`::tab-stop`), default button on Enter | ✅ | `transpiler/test/tabstop.test.ts`, `transpiler/test/widgets.test.ts` |
| Screen-reader semantics of native controls and plain elements | ✅ | `meson test widgets-a11y widgets-a11y-generic` |
| `<Menu>`/`<ContextMenu>`/`<MenuBar>` | 🟡 | positioning and theming limits; `&` mnemonics are only visual. See [known issues](known-issues.md) |
| `<ListView>`/`<TableView>`/`<TreeView>` | 🟡 | data views over plain JS data, built from plain items rather than Qt's item views (`docs/native-behavior-audit.md` P5) |
| `<WebView>` | 🟡 | experimental; see [known issues](known-issues.md) |
| `<Chart>`, `<Scene3D>`, `<Surface>` | 🟡 | experimental; 3D cannot be verified offscreen |
| `<RichText>`/`<DocumentEditor>` | ✅ | ODF round trip; `meson test richtext-handler`; limits in [document-editor.md](document-editor.md) |
| Drag and drop between apps, touch gestures | ❌ | only in-app `draggable`/`onDrop` on plain elements |

## CSS

The engine is `vendor/qml-css-engine`; its tests live in `vendor/qml-css-engine/tests/` and run
under `meson test`. **Unknown properties and selectors are ignored silently**: the parser keeps
every declaration, the engine never reads the ones it does not implement, and nothing warns. The
engine's own reference is `vendor/qml-css-engine/docs/css-support.md`; in places it is older than
this table (it omits `flex-wrap`, `flex-shrink`, `flex-basis`, `order`, `grid-template-areas` and
`box-sizing`, which are implemented).

| Feature | Status | Evidence / notes |
|---|---|---|
| Type, class, `#id`, universal selectors; specificity; `!important`; source order | ✅ | `cssthemeparser.cpp` `parseSimpleSelector`; `tests/test_css.cpp` |
| Descendant combinator (`.nav button`) | ✅ | full ancestor-chain match; `tests/test_css.cpp` |
| Child `>`, sibling `+`/`~` combinators | ⚠️ | parsed, but every one of them matches as the **descendant** combinator (`cssthemeparser.cpp` `parseFullSelector`) |
| Attribute selectors `[x=y]` | ⚠️ | not parsed; the rule matches the wrong elements or none |
| `:hover` | ✅ | tracked by the engine on any element (`tests/test_qml_css.cpp` `hoverRuleEnablesEngineHoverTracking`) |
| `:focus` | ✅ | read from the item's `activeFocus` (`csstheme.cpp`) |
| `:active`, `:checked`, `:disabled`, `:selected`… | 🟡 | applied when the widget publishes the state (`cssState`), as the built-in widgets do; arbitrary elements do not get them |
| Structural pseudo-classes (`:first-child`, `:nth-child()`, `:not()`, `:is()`, `:where()`, `:focus-visible`) | ⚠️ | not implemented; treated as an unknown class |
| `::before`/`::after` | ✅ | `tests/test_css.cpp` |
| `::tab-stop` (focus ring), `::part(…)` | ✅ | project extensions; [keyboard & accessibility](keyboard-accessibility.md) |
| `::placeholder`, `::selection` | ❌ | ignored |
| Custom properties `--x` / `var()` with fallbacks and inheritance | ✅ | `tests/test_css.cpp` `customPropertiesResolve` |
| `@media` (`min-`/`max-` `width`/`height`, `and`, comma lists), re-evaluated live | ✅ | `cssthemeparser.cpp`; other media features (`orientation`, `prefers-*`) are not supported |
| `@import` (local and remote, cached) | ✅ | `cssthemeloader.cpp` |
| `@font-face` with a remote `url()` | ✅ | `tests/test_text.cpp`, `tests/test_qml_css.cpp` |
| `@keyframes` + `animation` | ✅ | `tests/test_css.cpp` `parsesKeyframesAndAnimation`, `tests/test_qml_css.cpp` `cssRectCppKeyframesDriverLive` |
| `@supports`, `@container` | ⚠️ | the whole block is dropped |
| `@layer` | ⚠️ | not recognised |
| `display: block / flex / inline-flex / grid / none` | ✅ | `tests/test_layout.cpp` |
| `display: inline / inline-block / contents` | ⚠️ | not implemented |
| Flexbox: direction, wrap, grow, shrink, basis, order, justify/align, gaps | ✅ | `csslength.cpp`; `tests/test_layout.cpp` |
| Grid: template rows/columns, `fr`, `minmax`, `repeat`, `auto`, gaps, `grid-template-areas` + `grid-area` | ✅ | `tests/test_layout.cpp`, `tests/test_qml_css.cpp` `layoutGridTemplateAreas` |
| Grid: `grid-column`/`grid-row` spans, `grid-auto-flow`, dense packing | ⚠️ | not implemented (auto-placement is row-major) |
| `position: absolute` + `top/right/bottom/left/inset` | ✅ | `tests/test_layout.cpp` |
| `position: relative / fixed / sticky` | ⚠️ | treated as static |
| `z-index` | ✅ | |
| `overflow-y: auto / scroll` (with a desktop scrollbar), `overflow: hidden` | ✅ | |
| `overflow-x` (horizontal scrolling) | ❌ | |
| `box-sizing: border-box` (the runtime reset sets it everywhere) | ✅ | `csslength.cpp`; `src/solid-qml/reset.css` |
| `margin: auto` centring | ❌ | use flex/grid alignment |
| Units: `px`, `%`, `em`, `rem`, `vw`, `vh`, `vmin`, `vmax`; `calc()`, `min()`, `max()`, `clamp()` | ✅ | `valueparser.cpp`; `tests/test_layout.cpp` `percentAndCalcWidths` |
| Colours: named, `#rgb`, `#rrggbb`, `rgb()`, `rgba()`, `transparent` | ✅ | `valueparser.cpp` |
| Colours: `hsl()`, `currentColor`, 8-digit `#rrggbbaa` | ⚠️ | `hsl()`/`currentColor` are not parsed; an 8-digit hex goes to `QColor`, which reads it as `#aarrggbb` |
| `linear-gradient`, `radial-gradient` | ✅ | |
| `conic-gradient` | ❌ | |
| Borders (`solid`/`dotted`/`dashed`), `border-radius`, `outline` | ✅ | |
| `box-shadow`, `text-shadow`, `opacity` | ✅ | |
| `transform`: `translate*`, `scale*`, `rotate` | ✅ | no `skew`/`matrix` |
| `filter`, `backdrop-filter` | ❌ | |
| `transition` (opacity, width/height, background and border colours) | ✅ | `tests/test_qml_css.cpp` `transitionAnimatesItemOpacity`, `transitionAnimatesLayoutWidth` |
| Text: `font-family/size/weight`, `line-height`, `letter-spacing`, `text-align` (left/center/right), `text-decoration`, `text-transform`, `white-space: nowrap`, `text-overflow: ellipsis` | ✅ | `tests/test_text.cpp` |
| `font-style: italic`, `text-align: justify`, `word-break`, `white-space: pre*` | ❌ | |
| Inline `style=` attribute | ❌ | rejected by the transpiler (see above); use classes |

## Runtime shims and capability profiles

Installed both by `solid-qml-loader` and by `SolidQmlEmbed::init`.

| Global | Status | Evidence / notes |
|---|---|---|
| `fetch`, `Headers`, `Request`, `Response`, `AbortController` | ✅ | `src/shims/webfetch.cpp`; HTTPS through `QNetworkAccessManager`; `file://`/`qrc:` URLs read local files |
| `XMLHttpRequest` (in npm code) | 🟡 | free `XMLHttpRequest` references in mirrored packages are rewritten to a fetch-backed class; async only (`transpiler/test/node-imports.test.ts`) |
| `localStorage` | ✅ | `src/shims/weblocalstorage.cpp`; a JSON file written atomically |
| `sessionStorage` | ❌ | |
| `setTimeout`/`setInterval`/`clear*`, `queueMicrotask` | ✅ | `src/shims/webtimers.cpp` |
| `btoa`/`atob`, `TextEncoder`/`TextDecoder`, `crypto.getRandomValues`/`randomUUID`, `performance.now`, `structuredClone`, `navigator.clipboard` | ✅ | `src/shims/webplatform.cpp` |
| `crypto.subtle` | ❌ | |
| `URL`, `URLSearchParams`, `Blob`, `EventTarget`, DOM (`window`, `document`) | ❌ | |
| ES2019–2023 library methods (`replaceAll`, `at`, `flat`, `findLast`, `toSorted`, `Object.fromEntries`, `Promise.allSettled`…) and `globalThis` | ✅ | `src/shims/jspolyfill.cpp`, added only where V4 lacks them |
| `Worker`, `worker_threads` | ✅ | `src/shims/webworker.cpp`; each worker is its own QML engine on a `QThread` |
| `SharedArrayBuffer`, `Atomics.wait`/`notify` | ✅ | `src/shims/sharedbuffers.cpp` (private V4 API) |
| `process` (`versions.solidQml`, `platform`, `argv`, `env`, `cwd`, `exit`) | ✅ | `src/shims/nodeshims.cpp`; `argv`/`env`/`cwd`/`exit` depend on the profile |
| `fs`, `fs/promises`, `child_process` | ✅ | by profile; other Node built-ins (`path`, `os`, `Buffer`…) are not provided |
| Desktop notifications, tray | ✅ | `src/shims/notifications.cpp` (D-Bus), `<Tray>` |

Capability profiles (`src/shims/nodeshims.cpp`):

| Profile | `fs` | `process.argv/env/cwd/exit` | `child_process` |
|---|---|---|---|
| `browser` | no | empty / throws | no |
| `desktop` (default) | yes | yes | no |
| `trusted` | yes | yes | yes (logs a warning) |

Choose one with `solid-qml-loader --capabilities <profile>`, `SOLIDQML_CAPABILITIES=<profile>`, or
`SolidQmlEmbed::init(engine, appDir, "<profile>")`. A host that embeds untrusted UI should use
`browser`.

## npm packages and the V4 engine

A bare import (`import { encode } from "js-base64"`) is resolved through `node_modules`, and the
package's own files are rewritten into ES modules V4 can load (`transpiler/src/resolve/node.ts`,
`v4-compat.ts`): CommonJS becomes ESM, newer syntax is lowered, and known V4 defects are worked
around at the call site. Evidence: `transpiler/test/node-imports.test.ts` and
`examples/nodeimports.tsx` (`js-base64`, and `wrap-ansi` with its dependency tree).

| Topic | Status | Notes |
|---|---|---|
| Pure-JS packages (ESM or CommonJS) | 🟡 | they run unmodified, but only a few packages are tested; a package that breaks usually reveals a V4 gap that has to be fixed in the mirror or the shims |
| Native addons (node-gyp), packages importing Node built-ins other than `fs`/`child_process`/`process` | ❌ | a Node built-in import is rejected at transpile time |
| `BigInt`, `BigInt64Array` | ❌ | V4 does not have them |
| `fn.apply(this, typedArray)` | 🟡 | V4 reads every element as 0; call sites in mirrored packages are rewritten, your own component code is not |
| Newer syntax in packages | ✅ | lowered by Babel during mirroring |

Mirroring applies to npm packages only. Your own component code goes through the transpiler, with
the limits listed in the [first section](#solid-apis-jsx-and-statements).

## Platforms

| Platform | Status | Notes |
|---|---|---|
| Linux x86_64, Qt 6.11.x, Arch/Manjaro | ✅ | verified from a clean `archlinux:latest` container ([getting started](getting-started.md)) |
| Linux, other distributions (Debian, Ubuntu, Fedora) | 🟡 | untested; best-effort package names in [getting started](getting-started.md) |
| Wayland and X11 | ✅ | exercised by hand; the automated tests run offscreen |
| Qt 6.12, or any Qt other than 6.11 | ❌ | the library uses private Qt headers and ports 6.11's `QQuickText` layout (see [release policy](release-policy.md) and [known issues](known-issues.md)) |
| Windows, macOS, Android, iOS | ❌ | not built or tested |
| Distribution as an npm package, AppImage or binary SDK | ❌ | source only for this preview |
| AOT (C++) back-end | ❌ | experimental, counter-sized subset; not part of the preview |
