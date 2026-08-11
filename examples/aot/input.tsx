// M-AOT-1 fixture: a controlled <input> — value bound to a signal, onInput writes it back, and a
// text node echoes it live (proves the two-way controlled-input binding).
import { createSignal } from "solid-js";
import { div, text, input, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function InputDemo() {
  const [name, setName] = createSignal("Ada");
  return (
    <div class="app">
      <input class="field" value={name()} onInput={(e) => setName(e.currentTarget.value)} placeholder="your name" />
      <text class="h1">hello, {name()}!</text>
    </div>
  );
}
export function InputApp() {
  return <Window title="Input" width={420} height={200} visible><InputDemo /></Window>;
}
