import { createSignal, For } from "solid-js";

export function List() {
  const [items, setItems] = createSignal(["a", "b", "c"]);
  return (
    <div class="app">
      <For each={items()}>{(item) => <text class="row">{item}</text>}</For>
    </div>
  );
}
