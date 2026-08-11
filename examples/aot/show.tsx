// M-AOT-1 fixture: <Show> conditional rendering with a fallback, toggled by a button. Proves the
// visible-guard binding (an invisible box leaves the layout) reacts to the condition's deps.
import { createSignal, Show } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function ShowDemo() {
  const [on, setOn] = createSignal(true);
  return (
    <div class="app">
      <button onClick={() => setOn(!on())}>toggle</button>
      <Show when={on()} fallback={<text class="bio">it is off</text>}>
        <text class="h1">it is on</text>
      </Show>
    </div>
  );
}
export function ShowApp() {
  return <Window title="Show" width={420} height={200} visible><ShowDemo /></Window>;
}
