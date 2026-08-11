// M-AOT-1 fixture: <For> — value-keyed list over object rows (item is the value; `item.name` is a
// safe member lookup). Complements index-list (which exercises <Index>'s accessor rows).
import { createSignal, For } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function ForList() {
  const [users] = createSignal([{ name: "Ada" }, { name: "Alan" }, { name: "Grace" }]);
  return (
    <div class="app">
      <For each={users()}>
        {(u) => <text class="bio">{u.name}</text>}
      </For>
    </div>
  );
}
export function ForApp() {
  return <Window title="For" width={420} height={200} visible><ForList /></Window>;
}
