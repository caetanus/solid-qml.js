// <Dynamic> with a reactive tag. Plain component (no <Window>/render).
import { createSignal } from "solid-js";
import { Dynamic } from "solid-js/web";
import { div, text, button } from "../src/solid-qml/runtime";

export function DynamicTag() {
  const [tag, setTag] = createSignal("h1");

  return (
    <div class="app">
      <button onClick={() => setTag(tag() === "h1" ? "bio" : "h1")}>render as: &lt;{tag()}&gt;</button>
      <Dynamic component={tag()}>Polymorphic element</Dynamic>
    </div>
  );
}
