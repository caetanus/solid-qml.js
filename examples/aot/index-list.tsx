// M-AOT-1 fixture: <Index> — index-keyed list (the row item is an accessor), plus a button that
// pushes a new item (spread). Proves the rebuild-on-change row factory + array signal state.
import { createSignal, Index } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function IndexList() {
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
export function IndexApp() {
  return <Window title="Index" width={420} height={240} visible><IndexList /></Window>;
}
