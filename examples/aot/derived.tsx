// M-AOT-1 fixture: derived accessor (a plain function reading a signal is reactive) + a counter.
// Self-contained (inlines the component + Window) until the cpp back-end follows imports.
import { createSignal } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function Derived() {
  const [count, setCount] = createSignal(0);
  const doubleCount = () => count() * 2;
  return (
    <div class="app">
      <button onClick={() => setCount(count() + 1)}>increment</button>
      <text>{count()} doubled is {doubleCount()}</text>
    </div>
  );
}

export function DerivedApp() {
  return <Window title="Derived" width={420} height={200} visible><Derived /></Window>;
}
