import { createSignal, createMemo } from "solid-js";

export function BlockMemo() {
  const [filter, setFilter] = createSignal("all");
  const [todos, setTodos] = createSignal(["a", "b"]);
  const visible = createMemo(() => {
    const f = filter();
    return todos().filter(function(t) { return f === "all" || t === f; });
  });
  return (
    <div class="app">
      <text>{visible().length}</text>
    </div>
  );
}
