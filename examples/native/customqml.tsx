// Custom QML import (native escape hatch, plan phase 3.5): a hand-written .qml file
// imported with standard ES syntax and used as a component. Native only — on the web the
// vite plugin resolves .qml to a stub (the whole Native view is process-guarded anyway).
import { createSignal } from "solid-js";
import { div, text } from "../../src/solid-qml/runtime";
import Badge from "./Badge.qml";

export function CustomQml() {
  const [clicks, setClicks] = createSignal(0);
  const [stars, setStars] = createSignal(0);
  return (
    <div class="nv-section">
      <text class="nv-title">Custom QML (escape hatch)</text>
      <div class="nv-row">
        <Badge label="clicks" count={clicks()} onBumped={() => setClicks(clicks() + 1)} />
        <Badge label="stars" count={stars()} onBumped={() => setStars(stars() + 1)} />
        <text class="nv-echo">imported straight from Badge.qml</text>
      </div>
    </div>
  );
}
