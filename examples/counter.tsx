// Canonical Solid "Counter" example (solidjs.com/examples/counter): a signal incremented on a
// timer, with onCleanup to clear it. Exercises createSignal + setInterval/clearInterval + onCleanup
// + reactive text interpolation. Plain component (no Window/render) for the gallery.
import { createSignal, onCleanup } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";

export function TimerCounter() {
  const [count, setCount] = createSignal(0);
  const timer = setInterval(() => setCount(count() + 1), 1000);
  onCleanup(() => clearInterval(timer));

  return (
    <div class="app">
      <text>Count value is {count()}</text>
    </div>
  );
}
