// Fixture: local helper function used as onClick handler + passed as prop.
// Tests: function declaration → QML method; onClick={inc} → onClicked: inc();
// IfStatement + ReturnStatement in body.
import { createSignal } from "solid-js";
import { div, button, text } from "../../src/solid-qml/runtime";

export function Counter() {
  const [count, setCount] = createSignal(0);

  function inc() {
    setCount(count() + 1);
  }

  return (
    <div class="app">
      <button class="btn" onClick={inc}>click</button>
      <text class="out">{count()}</text>
    </div>
  );
}
