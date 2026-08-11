// M-AOT-1 fixture: <Switch>/<Match> — first-match-wins, with a fallback. Toggled by a button.
import { createSignal, Switch, Match } from "solid-js";
import { div, text, button, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function SwitchDemo() {
  const [n, setN] = createSignal(1);
  return (
    <div class="app">
      <button onClick={() => setN(n() + 1)}>next</button>
      <Switch fallback={<text class="bio">many</text>}>
        <Match when={n() === 1}><text class="h1">one</text></Match>
        <Match when={n() === 2}><text class="h1">two</text></Match>
      </Switch>
    </div>
  );
}
export function SwitchApp() {
  return <Window title="Switch" width={420} height={200} visible><SwitchDemo /></Window>;
}
