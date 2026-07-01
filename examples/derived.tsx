// Derived signal (a plain accessor that reads a signal is reactive). No <Window>/render.
import { createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";

export function Derived() {
  const [count, setCount] = createSignal(0);
  const doubleCount = () => count() * 2;

  return (
    <div class="app">
      <button onClick={() => setCount(count() + 1)}>increment</button>
      <text>{count()} doubled is {doubleCount()}</text>
    </div>
  );
}
