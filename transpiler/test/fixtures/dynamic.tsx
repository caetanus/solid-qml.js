import { createSignal } from "solid-js";
import { Dynamic } from "solid-js/web";

export function DynamicTag() {
  const [tag, setTag] = createSignal("h1");
  return (
    <div class="app">
      <button onClick={() => setTag(tag() === "h1" ? "bio" : "h1")}>toggle</button>
      <Dynamic component={tag()}>polymorphic</Dynamic>
    </div>
  );
}
