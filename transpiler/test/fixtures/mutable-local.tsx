// Fixture: let nextId mutable local, mutated in a handler via nextId++.
// Expected: nextId becomes `property var nextId: 1` (not a var in onCompleted),
// and reads/writes self-qualify as __self.nextId in setup closures, bare in QML bindings.
import { createSignal } from "solid-js";
import { div, button, text } from "../../src/solid-qml/runtime";

export function IdGen() {
  const [label, setLabel] = createSignal("(none)");
  let nextId = 1;

  return (
    <div class="app">
      <button class="btn" onClick={() => setLabel("id:" + nextId++)}>next</button>
      <text class="out">{label()}</text>
    </div>
  );
}
