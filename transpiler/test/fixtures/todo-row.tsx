function TodoRow(props) {
  return (
    <div class="todo-row">
      <button class="todo-toggle" onClick={() => props.onToggle(props.todo.id)}>{props.todo.done ? "✓" : "○"}</button>
      <text class="todo-title">{props.todo.title}</text>
      <button class="todo-remove" onClick={() => props.onRemove(props.todo.id)}>✕</button>
    </div>
  );
}

export function App() {
  return (
    <div class="app">
      <TodoRow todo={myTodo} onToggle={handleToggle} onRemove={handleRemove} />
    </div>
  );
}
