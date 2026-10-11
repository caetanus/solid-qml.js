// The "hello world" fixture for release-plan M6 (install story from a clean machine) and its M7
// CI successor: a tiny app that lives OUTSIDE the solid-qml-native checkout, proving a new app can
// be transpiled and run against an INSTALLED framework. See docs/getting-started.md, "Your first
// app outside the checkout", and scripts/clean-room-check.sh, which runs this inside a fresh
// container.
//
// `__RUNTIME__` is replaced by clean-room-check.sh with the absolute path to the checkout's
// src/solid-qml/runtime (there is no npm-published runtime module yet — see release-plan M6/LATER).
import { createSignal } from "solid-js";
import { div, text, button, Window } from "__RUNTIME__";
import "./counter.css";

function Counter() {
  const [count, setCount] = createSignal(0);
  return (
    <div class="counter">
      <text>count: {count()}</text>
      <button onClick={() => setCount(count() + 1)}>increment</button>
    </div>
  );
}

export function App() {
  return (
    <Window title="my-first-solid-qml-app" width={320} height={220} visible>
      <div class="app">
        <Counter />
      </div>
    </Window>
  );
}
