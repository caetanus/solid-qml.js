# Gap analysis — Solid TodoMVC on solid-qml

Target: `examples/todomvc.tsx` (idiomatic Solid: `createStore`, `createMemo`, `<For>`,
`<Show>`, `<Switch>/<Match>`, a child component with props, `<input>` + `onInput`/`onKeyDown`,
`classList`, `onMount`, store updates). Method: ran the current transpiler and tried to load
the output.

## Result
The generated QML **does not even load**:
```
/tmp/todo.qml:60:46: Unexpected token `<'   ← the <For> delegate (todo) => <TodoRow/> leaked as raw JSX into a text: binding
```
Everything beyond the current "single App + createSignal + structural div/text/button +
simple handlers" subset broke. Chosen direction (from planning): **expand the static
transpiler**, mapping Solid constructs to existing QML (`<For>`→`Repeater`, `<Show>`→
`Loader`/`visible`, `<Switch>/<Match>`→`Loader`/states, input→a QML `TextField`).

## Gaps (observed)

### A. Reactivity primitives — only `createSignal` is detected
- **`createStore`** ignored → `todos` emits no property; every `todos.*` read (in memos/
  handlers/`<For>`) is undefined. Needs: store → a reactive collection (QML `ListModel`, or a
  JS-array property feeding a `Repeater`) + translation of updates (`push`, `filter`, path
  setter `setTodos(pred,"done",fn)`, `produce`).
- **`createMemo`** ignored → `remaining()`/`visible()` are calls to undefined. Needs: memo →
  `readonly property` whose binding recomputes (QML reactivity on the deps).
- **`createEffect`** (not in sample, but required) → `Connections`/`on<Prop>Changed` or an
  imperative effect.
- **local mutable** (`let nextId`) → lost. Needs: non-reactive locals kept on the root.
- **`onMount`** ignored → should become `Component.onCompleted` (the sample's
  `localStorage.getItem` would then work via our shim).

### B. Control flow components — emitted as dead `Item { objectName: "unsupported:X" }`
- **`<For each>`** → `Repeater { model; delegate }`. The render-prop child `(item) => <JSX>`
  must become the delegate — today it's dumped as text, producing **invalid QML**.
- **`<Show when fallback>`** → `Loader`/`visible`; today the condition AND the `fallback`
  (`<text class="empty">`) are dropped, children emitted unconditionally.
- **`<Switch>/<Match when>`** → `Loader`/QML `states`; today all `<Match>` branches render at
  once with no condition.
- Future: `<Index>`, `<Dynamic>`, `<Portal>`, `<ErrorBoundary>`, `<Suspense>`.

### C. Components & props — unsupported
- `TodoRow` is never emitted or instantiated; `<TodoRow todo=.. onToggle=..>` vanished (only
  reached via `<For>`, where it leaked as text). Needs: each component → an inline `Component`/
  generated type; instantiation wires `props` to QML properties (reactive); `props.x` reads.

### D. Component logic (local functions) — dropped
- `add`/`toggle`/`remove` (defined in `App`) aren't emitted. `onClick={add}` became
  `onClicked: add` (a reference to an undefined symbol, and never *called*). Needs: component
  functions → QML JS functions on the root; bare-function handlers → call form (`add()`).

### E. Primitives, attributes, events
- **No text-input primitive**: `<input>` → dead `Item`; `placeholder`/`value`/`onInput`/
  `onKeyDown` all dropped. Needs a `CssTextField` (QML `TextField`/`TextInput`) with
  value/placeholder and input/keydown.
- **`value={draft()}`** two-way binding — unsupported.
- **`classList={{ completed: ... }}`** — dropped. Map to dynamic `cssClass`/`cssState`.
- **Events beyond click**: `onInput`, `onKeyDown` (+ `e.currentTarget.value`, `e.key`) — only
  `onClick`/`onClicked` are handled; need event aliasing + event-arg mapping.

### F. Robustness / misc
- **Transpiler can emit invalid QML** (raw JSX leak). It must never do that — unknown
  constructs should fail loudly at generate time, not produce a broken `.qml`.
- **Text whitespace**: `{remaining()} left` → `... + "left"` (lost the space → "5left").
- **Component scoping**: `findAppReturn` collects *all* `createSignal` globally and picks the
  *last* JSX return — fragile with multiple components; needs real per-component handling with
  `render(() => <App/>)` as the entry.

## Proposed plan (static-transpiler expansion; nothing implemented yet)
1. **Front-end robustness**: component model (emit/instantiate components + props), local
   functions on the root, bare-fn handlers, and **never emit raw JSX** (hard error).
2. **Reactivity**: `createMemo`→`readonly property`; `createStore`→reactive list/model + update
   translation; `createEffect`; `onMount`→`Component.onCompleted`; per-component scope.
3. **Control flow → QML**: `<For>`→`Repeater`(+delegate); `<Show>`→`Loader`/`visible`(+fallback);
   `<Switch>/<Match>`→`Loader`/states.
4. **Primitives/attrs/events**: `CssTextField` (input) + two-way `value`; `classList`→`cssState`;
   `onInput`/`onKeyDown` + event-arg mapping; generic attribute passthrough.
5. **Polish**: text whitespace; keyed `<For>`; `<Index>`/`<Dynamic>`/`<Suspense>`.
