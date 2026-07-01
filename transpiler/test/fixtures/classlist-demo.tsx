import { createSignal } from "solid-js";

export function App() {
  const [done, setDone] = createSignal(false);
  return (
    <div class="todo-row" classList={{ completed: done() }}>
      <text class="label">item</text>
    </div>
  );
}
