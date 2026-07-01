import { createContext, createSignal } from "solid-js";

const CounterContext = createContext();

export function CounterProvider(props) {
  const [count, setCount] = createSignal(props.count || 0);
  const counter = { count, increment: () => setCount(count() + 1) };
  return (
    <CounterContext.Provider value={counter}>
      {props.children}
    </CounterContext.Provider>
  );
}
