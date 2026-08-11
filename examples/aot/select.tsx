// M-AOT-1 fixture: controlled <select> with <option>s (SolidWidgets::Select). value → currentIndex,
// activated → onChange with the picked value. A text node echoes the current selection.
import { createSignal } from "solid-js";
import { div, text, select, option, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function SelectDemo() {
  const [fruit, setFruit] = createSignal("banana");
  return (
    <div class="app">
      <select class="bio" value={fruit()} onChange={(e) => setFruit(e.target.value)}>
        <option value="apple">Apple</option>
        <option value="banana">Banana</option>
        <option value="cherry">Cherry</option>
      </select>
      <text class="h1">{fruit()}</text>
    </div>
  );
}
export function SelectApp() {
  return <Window title="Select" width={420} height={220} visible><SelectDemo /></Window>;
}
