// Canonical Solid Context example (createContext/useContext), as in the official tutorial: a
// provider holds state, children consume it via useContext — no prop drilling. Exercises
// createContext, <Ctx.Provider>, props.children slotting, useContext, and components reading
// shared state. Plain component (no Window/render) for the gallery.
import { createContext, useContext, createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";

const CounterContext = createContext();

function CounterProvider(props) {
  const [count, setCount] = createSignal(props.count || 0);
  const counter = { count, increment: () => setCount(count() + 1) };
  return (
    <CounterContext.Provider value={counter}>
      {props.children}
    </CounterContext.Provider>
  );
}

function Display() {
  const counter = useContext(CounterContext);
  return <text>Count: {counter.count()}</text>;
}

function Increment() {
  const counter = useContext(CounterContext);
  return <button onClick={counter.increment}>increment</button>;
}

export function ContextDemo() {
  return (
    <CounterProvider count={5}>
      <div class="app">
        <Display />
        <Increment />
      </div>
    </CounterProvider>
  );
}
