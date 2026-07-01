// <Index> — index-keyed list (the row item is an accessor). Plain component (no <Window>/render).
import { createSignal, Index } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";

export function IndexList() {
  const [items, setItems] = createSignal(["alpha", "beta", "gamma"]);

  return (
    <div class="app">
      <button onClick={() => setItems([...items(), "more"])}>add</button>
      <Index each={items()}>
        {(item, i) => <text>{i}: {item()}</text>}
      </Index>
    </div>
  );
}
