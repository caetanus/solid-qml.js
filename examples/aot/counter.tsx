// M-AOT-0 twin-render fixture: the canonical per-instance counter, wrapped in a fixed-size Window
// so the interpreted (QML) and AOT (C++) back-ends render byte-for-byte comparable frames. Kept
// deliberately self-contained (its own CSS, no gallery chrome) so the pixel compare is deterministic.
import { createSignal } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
import "./counter.css";

function Counter(props: { label: string }) {
  const [count, setCount] = createSignal(0);
  return (
    <div class="counter">
      <button onClick={() => setCount(count() + 1)}>{props.label}: {count()}</button>
    </div>
  );
}

export function CounterApp() {
  return (
    <Window title="AOT Counter" width={420} height={180} visible>
      <div class="app">
        <Counter label="A" />
        <Counter label="B" />
        <Counter label="C" />
      </div>
    </Window>
  );
}
