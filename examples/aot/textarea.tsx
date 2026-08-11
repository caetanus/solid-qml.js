// M-AOT-1 fixture: controlled <textarea> (TextArea widget, textChanged) + a live echo.
import { createSignal } from "solid-js";
import { div, text, textarea, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

function FormDemo() {
  const [notes, setNotes] = createSignal("first line");
  return (
    <div class="app">
      <textarea class="bio" value={notes()} onInput={(e) => setNotes(e.target.value)} placeholder="notes" />
      <text class="h1">{notes()}</text>
    </div>
  );
}
export function TextareaApp() {
  return <Window title="Textarea" width={420} height={240} visible><FormDemo /></Window>;
}
