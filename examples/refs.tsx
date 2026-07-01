// Canonical Solid "Refs" tutorial example: grab a reference to a rendered element. Exercises the
// `ref={var}` binding plus onMount reading the ref. Plain component (no Window/render) for the gallery.
import { onMount } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";

export function Refs() {
  let myDiv: HTMLDivElement | undefined;

  onMount(() => {
    console.log("ref resolved:", myDiv);
  });

  return (
    <div class="app" ref={myDiv}>
      <text>the box above this text is captured by ref</text>
    </div>
  );
}
