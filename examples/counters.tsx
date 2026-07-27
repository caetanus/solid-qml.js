// Example app = a plain component (no <Window>/render — the QML entry owns the Window). Multiple
// independent Counter instances prove per-instance state.
import { createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";

function Counter(props: { label: string }) {
  const [count, setCount] = createSignal(0);
  return (
    <div class="counter">
      <button onClick={() => setCount(count() + 1)}>{props.label}: {count()}</button>
    </div>
  );
}

export function Counters() {
  return (
    <div class="app">
      <Counter label="A" />
      <Counter label="B" />
      <Counter label="C" />
    </div>
  );
}
