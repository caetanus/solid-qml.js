// Fixture: arrow helpers with params (become QML methods, not inlined-derived).
// Tests: const f = (p) => expr → method; onClick={f} → onClicked: f(); prop={f} → prop: f.
import { createSignal } from "solid-js";
import { div, button, text } from "../../src/solid-qml/runtime";

export function Clamped() {
  const [count, setCount] = createSignal(0);

  const inc = (n: number) => setCount(count() + n);

  function clampedInc() {
    if (count() < 5) {
      inc(1);
    }
  }

  return (
    <div class="app">
      <button class="btn" onClick={clampedInc}>inc</button>
      <text class="out">{count()}</text>
    </div>
  );
}
