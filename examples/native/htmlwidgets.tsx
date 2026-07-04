// Native HTML widgets — <progress>, <fieldset>/<legend>, <dialog>, <details>/<summary>.
// One .nv-section card in the native-only gallery (shell classes nv-section/nv-title/
// nv-row/nv-label come from examples/native.css).
import { createSignal } from "solid-js";
import { div, text, button } from "../../src/solid-qml/runtime";
import "./htmlwidgets.css";

export function HtmlWidgets() {
  const [pct, setPct] = createSignal(0.25);
  const [dlgOpen, setDlgOpen] = createSignal(false);

  return (
    <div class="nv-section">
      <text class="nv-title">HTML widgets</text>

      {/* determinate progress driven by a signal + an incrementing button */}
      <div class="nv-row">
        <text class="nv-label">Progress</text>
        <progress class="hw-progress" max={1} value={pct()} />
        <button class="hw-btn" onClick={() => setPct(pct() >= 1 ? 0 : pct() + 0.25)}>+25%</button>
      </div>

      {/* indeterminate progress: no value attribute → sliding busy bar */}
      <div class="nv-row">
        <text class="nv-label">Busy</text>
        <progress class="hw-progress" />
      </div>

      {/* fieldset with a legend around two lines of text */}
      <fieldset class="hw-fieldset">
        <legend>Shipping</legend>
        <text class="hw-line">Standard delivery takes 3 to 5 business days.</text>
        <text class="hw-line">Orders over $199 ship free anywhere.</text>
      </fieldset>

      {/* modal dialog opened by a button; closed from inside (or Escape / press outside) */}
      <div class="nv-row">
        <text class="nv-label">Dialog</text>
        <button class="hw-btn" onClick={() => setDlgOpen(true)}>Open dialog</button>
      </div>
      <dialog class="hw-dialog" open={dlgOpen()} onClose={() => setDlgOpen(false)}>
        <div class="hw-dialog-body">
          <text class="hw-dialog-title">Native dialog</text>
          <text class="hw-line">A modal T.Dialog centered on the window overlay.</text>
          <button class="hw-btn" onClick={() => setDlgOpen(false)}>Close</button>
        </div>
      </dialog>

      {/* details/summary disclosure */}
      <details class="hw-details">
        <summary>More details</summary>
        <text class="hw-line">The disclosure content only occupies space while open.</text>
        <text class="hw-line">Click the summary row to toggle.</text>
      </details>
    </div>
  );
}
