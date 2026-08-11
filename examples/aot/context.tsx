// M-AOT-1 fixture: createContext/useContext — a provider holds state, children consume it (no prop
// drilling). Provider State exposes count (signal) + increment (Q_INVOKABLE); consumers get the
// provider State injected. Clicking increment updates the Display via NOTIFY.
import type { ParentProps } from "solid-js";
import { createContext, useContext, createSignal } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";

type CounterCtx = { count: () => number; increment: () => void };
const CounterContext = createContext<CounterCtx>();

function CounterProvider(props: ParentProps<{ count?: number }>) {
  const [count, setCount] = createSignal(props.count || 0);
  const counter = { count, increment: () => setCount(count() + 1) };
  return <CounterContext.Provider value={counter}>{props.children}</CounterContext.Provider>;
}
function Display() {
  const counter = useContext(CounterContext)!;
  return <text class="h1">Count: {counter.count()}</text>;
}
function Increment() {
  const counter = useContext(CounterContext)!;
  return <button class="bio" onClick={counter.increment}>increment</button>;
}
function ContextDemo() {
  return (
    <CounterProvider count={5}>
      <div class="app">
        <Display />
        <Increment />
      </div>
    </CounterProvider>
  );
}
export function ContextApp() {
  return <Window title="Context" width={420} height={200} visible><ContextDemo /></Window>;
}
