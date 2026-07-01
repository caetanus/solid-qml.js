// The original solid-qml "hello world": the Q mark with orbiting sparks (a CSS @keyframes spin)
// plus a headline and an event counter. Plain component (no <Window>/render) for the gallery —
// the very first thing we built, kept as a smoke test for grid, absolute positioning + animation.
// Its classes are `hello-` prefixed so they don't collide with the other examples' stylesheets.
import { createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
import "./hello.css";

export function Hello() {
  const [events, setEvents] = createSignal(0);

  return (
    <div class="hello-stage" aria-label="Solid QML hello world">
      <div class="hello-panel">
      <div class="hello-mark" aria-hidden="true">
        <div class="hello-orbit">
          <div class="hello-spark hello-spark-a" />
          <div class="hello-spark hello-spark-b" />
          <div class="hello-spark hello-spark-c" />
        </div>
        <div class="hello-core">Q</div>
      </div>

      <div class="hello-content">
        <text class="hello-eyebrow">solid qml native</text>
        <text class="hello-headline">welcome to solid qml</text>
        <text class="hello-copy">
          A hello world in Solid.js — and the very same code already renders natively through QML.
        </text>

        <button class="hello-cta" onClick={() => setEvents((value) => value + 1)}>
          <span class="hello-cta-label">count events</span>
          <div class="hello-badge">{events()}</div>
        </button>
      </div>
      </div>
    </div>
  );
}
