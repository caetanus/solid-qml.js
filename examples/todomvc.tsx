// A reasonably idiomatic Solid TodoMVC — used as a GAP-ANALYSIS target for solid-qml.
// It deliberately exercises Solid features the current transpiler does NOT yet handle:
// createStore, createMemo, <For>, <Show>, <Switch>/<Match>, a child component with props,
// <input> + onInput/onKeyDown, classList, onMount, and store updates.
import { createSignal, createMemo, createEffect, For, Show, Switch, Match, onMount } from "solid-js";
import { createStore, produce } from "solid-js/store";
import { div, text, button } from "../src/solid-qml/runtime";
import "./todomvc.css";

type Todo = { id: number; title: string; done: boolean };
type Filter = "all" | "active" | "completed";

function TodoRow(props: {
  todo: Todo;
  onToggle: (id: number) => void;
  onRemove: (id: number) => void;
}) {
  return (
    <div class="todo-row" classList={{ completed: props.todo.done }}>
      <button class="todo-toggle" onClick={() => props.onToggle(props.todo.id)}>
        {props.todo.done ? "✓" : "○"}
      </button>
      <text class="todo-title">{props.todo.title}</text>
      <button class="todo-remove" onClick={() => props.onRemove(props.todo.id)}>
        ✕
      </button>
    </div>
  );
}

export function TodoMVC() {
  const [todos, setTodos] = createStore<Todo[]>([]);
  const [draft, setDraft] = createSignal("");
  const [filter, setFilter] = createSignal<Filter>("all");
  let nextId = 1;

  onMount(() => {
    const saved = localStorage.getItem("todos");
    if (saved) {
      const restored = JSON.parse(saved);
      setTodos(restored);
      nextId = restored.reduce((max, todo) => Math.max(max, todo.id), 0) + 1;
    }
  });

  // Persist to (app-segregated) localStorage whenever the list changes.
  createEffect(() => localStorage.setItem("todos", JSON.stringify(todos)));

  const remaining = createMemo(() => todos.filter((t) => !t.done).length);
  const visible = createMemo(() => {
    const f = filter();
    return todos.filter((t) => (f === "all" ? true : f === "active" ? !t.done : t.done));
  });

  function add() {
    const title = draft().trim();
    if (!title) return;
    setTodos(produce((list) => list.push({ id: nextId++, title, done: false })));
    setDraft("");
  }
  const toggle = (id: number) =>
    setTodos((t) => t.id === id, "done", (d) => !d);
  const remove = (id: number) => setTodos((list) => list.filter((t) => t.id !== id));
  // "all" marks every still-active todo done (shown only while some — but not all — are done).
  const selectAll = () => {
    for (const t of todos) {
      if (!t.done) toggle(t.id);
    }
  };

  return (
    <div class="todo-app">
      <text class="todo-h1">todos</text>

      <div class="todo-newline">
        <input
          class="todo-new"
          placeholder="What needs to be done?"
          value={draft()}
          onInput={(e) => setDraft(e.currentTarget.value)}
          onKeyDown={(e) => e.key === "Enter" && add()}
        />
        <button class="todo-add" onClick={add}>
          add
        </button>
      </div>

      <Show when={todos.length > 0} fallback={<text class="todo-empty">no todos yet</text>}>
        <div class="todo-list">
          <For each={visible()}>
            {(todo) => <TodoRow todo={todo} onToggle={toggle} onRemove={remove} />}
          </For>
        </div>

        <div class="todo-footer">
          <text class="todo-count">{remaining()} left</text>
          <div class="todo-filters">
            <Switch>
              <Match when={remaining() > 0 && remaining() < todos.length}>
                <button onClick={selectAll}>all</button>
              </Match>
            </Switch>
            <button onClick={() => setFilter("active")}>active</button>
            <button onClick={() => setFilter("completed")}>done</button>
          </div>
        </div>
      </Show>
    </div>
  );
}
