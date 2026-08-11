// M-AOT-1 fixture: controlled <input type="checkbox"> (+ a role="switch" toggle) with live echo.
import { createSignal } from "solid-js";
import { div, text, input, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function CheckDemo() {
  const [on, setOn] = createSignal(true);
  const [sw, setSw] = createSignal(false);
  return (
    <div class="app">
      <input type="checkbox" class="bio" checked={on()} onChange={(e) => setOn(e.target.checked)} />
      <input type="checkbox" role="switch" class="bio" checked={sw()} onChange={(e) => setSw(e.target.checked)} />
      <text class="h1">{on() ? "ON" : "off"}</text>
    </div>
  );
}
export function CheckboxApp() {
  return <Window title="Checkbox" width={420} height={200} visible><CheckDemo /></Window>;
}
