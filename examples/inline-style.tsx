// Inline `style` showcase: a palette of swatches coloured ONLY by their inline style (one shared
// class, one context node, eight colours), bars sized by numeric inline widths (px, as in React DOM —
// a layout property relays out), an inline colour beating a class, and a text coloured inline.
// Static, so a frame grab proves it (`--grab`).
import { For } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";
import "./inline-style.css";

const colors = ["#e53935", "#fb8c00", "#fdd835", "#43a047", "#00acc1", "#1e88e5", "#5e35b1", "#d81b60"];
const widths = [40, 120, 80, 200];

export function InlineStyle() {
  return (
    <div class="inline-demo">
      <div class="palette">
        <For each={colors}>{(c) => <div class="swatch" style={{ background: c }} />}</For>
      </div>
      <div class="bars">
        <For each={widths}>{(w) => <div class="bar" style={{ width: w }} />}</For>
      </div>
      <div class="row">
        <div class="tinted" style="background-color: #ff9800">inline wins</div>
        <div class="tinted">class only</div>
        <text style={{ color: "#4caf50", fontWeight: 700 }}>inline text</text>
      </div>
    </div>
  );
}
