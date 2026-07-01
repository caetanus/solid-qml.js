# Solid parity — where we are, and the path to 99%

Goal: **99% of real-world Solid.js apps transpile and run unchanged** on solid-qml (native QML/C++).
This tracks the Solid API surface against the transpiler (`transpiler/`) + the
C++ engine/shims. "Real-world" weights the APIs apps actually use — the long tail of rarely-used
primitives counts for little.

Status legend: ✅ done · 🟡 partial · ❌ missing

Validated end-to-end (web reference == native render): **TodoMVC** and **GitHub-user fetch**
(`examples/todomvc.tsx`, `examples/fetch.tsx`).

### Canonical examples actually run (not just "no error")
Running the official Solid examples through the pipeline and **verifying behaviour out-of-band**
(timer ticks observed via a localStorage probe, since the loader suppresses console output):

| Example | Status | Notes |
|---|---|---|
| `examples/derived.tsx` | ✅ | signal + derived accessor + onClick; `doubled` reacts |
| `examples/counter.tsx` | ✅ | timer increments the signal (proven: count reached 6 after ~1.7s of 300ms ticks). **Was broken** — `setInterval`/`onCleanup` in the body were silently dropped; now emitted as component setup |
| `examples/children.tsx` | ✅ | a `Card` wrapper slots `{props.children}` into its body |
| `examples/context.tsx` | ✅ | `createContext`/`<Ctx.Provider value=…>`/`useContext` + `props.children`: provider value seeds a signal from its prop (`count={5}` → `Count: 5`), `useContext().count()` reads it, `useContext().increment` wires onClick |
| `examples/refs.tsx` | ✅ | `ref={el}` gives the element a QML id + a window property; onMount reads it (proven: the ref is a live object with width 32, not undefined) |

**Lesson:** a "✅" in the tables means the construct was verified to actually behave (rendered / probed
out-of-band) — "runs without a QML error" alone is NOT proof, since broken codegen can still load.

---

## Reactivity
| API | Status | Notes |
|---|---|---|
| `createSignal` | ✅ | → root `property`; getter `s()`→`window.s`, setter→assignment |
| `createMemo` | ✅ | → `readonly property` (inline) or sidecar fn bound reactively |
| `createEffect` | ✅ | sidecar fn: initial run + `Connections` per read dep |
| `createResource` | ✅ | value + `_loading`/`_error`; async loader (Promise) + reload on source change |
| `untrack` | ❌ | needed by some patterns; lower to a plain (non-tracked) read |
| `batch` | ❌ | QML coalesces bindings already; lower to sequential assignments |
| `on(deps, fn)` | ❌ | explicit-deps effect; map to a targeted `Connections` |
| `createComputed` / `createRenderEffect` | ❌ | like effect with different timing |
| `createSelector` | ❌ | keyed equality selector |
| `createRoot` / `getOwner` / `runWithOwner` | ❌ | render entry is implicit; ownership not modeled |

## Stores
| API | Status | Notes |
|---|---|---|
| `createStore` | 🟡 | array/object `property var`; reads OK; setter forms below |
| store setters: value / `prev=>next` / `produce` / `(pred,"key",upd)` | ✅ | reassign-to-rebind |
| deep path setters `set(a, b, c, val)` | 🟡 | only the forms TodoMVC uses; arbitrary depth untested |
| `reconcile` | ❌ | diff-merge; lower to reassign for now |
| `unwrap` | ❌ | identity on our plain `var` stores (likely a no-op) |
| `createMutable` | ❌ | mutable proxy store |

## Lifecycle
| API | Status | Notes |
|---|---|---|
| `onMount` | ✅ | merged into `Component.onCompleted` |
| component body setup (timers, subscriptions) | ✅ | imperative statements in the body now run on mount via a `_setup(window)` sidecar (were silently dropped) |
| `onCleanup` | 🟡 | body-level: registered on a `__cleanups` list, run in `Component.onDestruction`. Inside effects: not yet |
| effect cleanup (returned fn) | ❌ | run previous cleanup before re-running an effect |

## Control flow (components)
| API | Status | Notes |
|---|---|---|
| `<For>` | ✅ | → `Repeater`; delegate inlines its body (`item`→`modelData`) |
| `<Show>` | ✅ | visibility-gated children + `fallback` |
| `<Switch>`/`<Match>` | ✅ | cascading visibility (first match wins) |
| `<Suspense>` | ✅ | fallback while any read resource is loading |
| `<Index>` | ✅ | → `Repeater` (shared with `<For>`); the row item is an accessor (`item()`→`modelData`), index a plain number |
| `<Dynamic component=…>` | 🟡 | reactive intrinsic TAG → a generic element with a reactive `cssPrimitive` (type selectors track it). Component-valued (`component={options[sel()]}`) needs a QML `Loader` over the candidate types — deferred |
| `<Portal>` | ❌ | render elsewhere (overlay layer) |
| `<ErrorBoundary>` | ❌ | catch render/async errors + fallback |
| `<SuspenseList>` | ❌ | rare |

## Components & props
| API | Status | Notes |
|---|---|---|
| function component + props | ✅ | inlined at use site; `props.x` substituted |
| reactive props | ✅ | substituted exprs stay reactive |
| function props (callbacks) | ✅ | `props.onX(...)`→ resolved local fn via sidecar |
| `props.children` | ✅ | `<Comp>…</Comp>` slots into the body's `{props.children}` |
| `mergeProps` / `splitProps` | ✅ | treated as prop aliases: a `mergeProps` default becomes a property's initial value (a passed prop overrides it); `splitProps` `local` is a props alias. `splitProps` `rest` (for spread) deferred until spread props land |
| `createContext` / `useContext` | ✅ | compile-time value substitution: `<Ctx.Provider value=…>` pushes a value, `useContext(Ctx)` (single instance) resolves to it; provider state hoists to the window. Nested/multi-instance providers untested |
| `lazy` | ❌ | code-split components (likely out of scope for native) |

## JSX / rendering
| Feature | Status | Notes |
|---|---|---|
| intrinsics `div/text/span/button/input/hr` | ✅ | mapped to Css* primitives / TextInput |
| generic lowercase intrinsic | ✅ | → `CssRect` with a type-selectable `cssPrimitive` |
| `class` / `classList` | ✅ | classList → reactive `cssState` |
| inline `style=` | ✅ | synthesised `#id{}` fed to the engine |
| events (`onClick`/`onInput`/`onKeyDown`) | 🟡 | click/input/keydown + event-arg mapping; not every DOM event |
| `ref` | ✅ | `ref={el}` → element QML id + a window property bound to it (code reads it via window.el). `ref={setEl}` callback form: not yet. Refs inside a component-type: not yet |
| `use:` directives | ❌ | custom directives |
| spread props `{...obj}` | ❌ | attribute spread |
| Fragments `<>…</>` | 🟡 | top level handled implicitly; nested untested |
| text whitespace | ✅ | `{x} word` keeps the space |

## Language / expressions (`toQml`)
| Feature | Status |
|---|---|
| literals, binary, unary, postfix `x++`, ternary | ✅ |
| member / element access, calls | ✅ |
| array / object literals (computed keys, shorthand, spread-in-literal) | ✅ |
| arrow / function expressions (param-scoped) | ✅ |
| template literals | ✅ |
| `for…of` / `for` / `while` | ✅ |
| `async`/`await` | ✅ (Babel → Promise chains; V4 has no native async) |
| optional chaining `?.` / nullish `??` | 🟡 (passes through; V4 support to confirm) |
| destructuring in statements | 🟡 |
| labelled/try/throw/switch statements | ❌ |

## Runtime (browser shims, already C++)
localStorage ✅ · fetch (+Headers/Request/Response/Abort) ✅ · timers/microtask ✅ ·
btoa/atob/TextEncoder/Decoder/crypto/performance/structuredClone ✅ · `BigInt` ❌ (V4 lacks it).

---

## Estimate
Core that real apps lean on — signals/memo/effect/resource, stores, `<For>/<Show>/<Switch>/
<Suspense>`, components+props, events, async/fetch — is **done**. By weighted real-world usage we're
around **~75–80%**. The gap to 99% is a focused, finite list (below); none are research problems —
each maps to a known QML construct.

## Path to 99% (prioritised)
1. **Components glue** ✅ — `props.children`, `createContext`/`useContext`, `ref`, `mergeProps`/
   `splitProps` all done. (`splitProps` rest waits on spread props.)
2. **Control-flow completion**: `<Index>` ✅, `<Dynamic>` 🟡 (tag form) — remaining: `<ErrorBoundary>`, `<Portal>`, Dynamic component-form.
3. **Lifecycle/reactivity edges**: `onCleanup` (+ effect cleanup), `untrack`, `batch`, `on`.
4. **Stores depth**: arbitrary deep path setters, `reconcile`, `unwrap`.
5. **JSX edges**: spread props `{...}`, nested fragments, confirm `?.`/`??` on V4, broader event set.
6. **Polish**: keyed `<For>`, more DOM events, `createComputed`/`createRenderEffect`/`createSelector`.

Out of scope (won't chase for 99%): `lazy`/code-split, `getOwner`/`runWithOwner`, `BigInt`,
`SuspenseList`. These are rare and/or fundamentally tied to a JS runtime we drop at release (AOT→C++).
