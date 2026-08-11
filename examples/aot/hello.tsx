// M-AOT-1 static-view fixture: the canonical "hello" (raw text in divs, spans, @keyframes spark
// orbit, an event counter button) wrapped in a fixed Window so QML and AOT back-ends twin-render.
// Self-contained (inlines the component + its own Window entry) until the cpp back-end follows imports.
import { createSignal } from "solid-js";
import { div, text, button, span, Window } from "../../src/solid-qml/runtime";
import "../hello.css";

function Hello() {
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

export function HelloApp() {
  return <Window title="Hello" width={800} height={600} visible><Hello /></Window>;
}
