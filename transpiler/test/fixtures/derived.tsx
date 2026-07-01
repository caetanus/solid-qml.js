import { createSignal, createMemo } from "solid-js";

export function Derived() {
  const [count, setCount] = createSignal(2);
  const double = createMemo(() => count() * 2);
  return (
    <div class="app">
      <text>count: {count()}</text>
      <text>double: {double()}</text>
    </div>
  );
}
