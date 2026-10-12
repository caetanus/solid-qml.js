# TodoMVC on solid-qml — resolved gap analysis

`examples/todomvc.tsx` is idiomatic Solid (`createStore` + `produce` + a path setter, `createMemo`,
`createEffect`, `<For>`, `<Show>`, `<Switch>`/`<Match>`, a child component with props, `<input>` with
`onInput`/`onKeyDown`, `classList`, `onMount` + `localStorage`). It transpiles and runs today; the
transpiler side is pinned by `transpiler/test/golden.test.ts` ("examples/todomvc.tsx — createStore +
setters fully transpiles").

This file used to hold the original gap analysis, written when the generated QML did not even load
(the `<For>` delegate leaked as raw JSX). Every gap it listed has since been closed. It is kept as a
record of what had to be built; the current state of each Solid API is tracked in
[`SOLID-PARITY.md`](../SOLID-PARITY.md) and, for what the transpiler accepts or rejects today, in
[`docs/support-matrix.md`](../docs/support-matrix.md).

| Original gap | Resolution |
|---|---|
| `createStore` ignored | A store is a reactive `property var`; value, `prev => next`, `produce` and `(pred, "key", updater)` setters reassign it. Arbitrary path setters and `reconcile` are rejected at compile time. |
| `createMemo` / `createEffect` ignored | Memo → `readonly property`; effect → an initial run plus a `Connections` per dependency read. |
| local `let` lost, `onMount` ignored | Plain locals stay on the component root; `onMount` merges into `Component.onCompleted`. |
| `<For>`, `<Show>`, `<Switch>`/`<Match>` emitted as dead items | `<For>` → a repeater with a row delegate; `<Show>` and `<Switch>` mount lazily. |
| child components never emitted | One `.qml` type per component; props become reactive properties. |
| local functions dropped, `onClick={add}` never called | Component functions become QML functions; a bare function handler is called. |
| no text input, no `value`/`onInput`/`onKeyDown`, no `classList` | `<input>` → `W.TextField` with `value`/`placeholder`/`onInput`/`onKeyDown`; `classList` → reactive CSS state. |
| invalid QML on unknown constructs | The transpiler fails loudly at compile time instead (attribute contract in `transpiler/src/emit/attrs.ts`). |
| `{remaining()} left` lost its space | Text whitespace is kept. |
