// Keyboard & accessibility (release-plan M5, docs/keyboard-accessibility.md): a plain <div> with
// role/tabIndex/aria-* reaching Tab order, Accessible.role/name/state and Enter/Space activation —
// the exact gap that attribute used to be silently dropped before this release. Side by side with
// a real <button> so Tab visits both, in document order, with the same `::tab-stop` ring.
import { createSignal } from "solid-js";
import { div, text, button } from "../src/solid-qml/runtime";
import "./a11y-demo.css";

export function A11yDemo() {
  const [count, setCount] = createSignal(0);
  const [muted, setMuted] = createSignal(false);

  return (
    <div class="a11y-demo">
      <text class="a11y-demo-title">Keyboard &amp; accessibility</text>
      <text class="a11y-demo-copy">
        Tab between the custom div and the real button below — both are in the chain, both
        activate with Enter/Space, both carry a screen-reader name.
      </text>
      <div class="a11y-demo-row">
        {/* A custom "button": role maps to Accessible.role, aria-label to Accessible.name,
            tabIndex joins the Tab chain, and onClick fires on Enter/Space while focused. */}
        <div
          class="a11y-demo-divbtn"
          role="button"
          tabIndex={0}
          aria-label="Increment (custom div)"
          onClick={() => setCount(count() + 1)}
        >
          <text>div role="button": {count()}</text>
        </div>
        <button class="a11y-demo-button" onClick={() => setCount(count() + 1)}>
          real button: {count()}
        </button>
      </div>
      {/* role="switch" + aria-checked: a toggle built from a div, no T.Switch underneath. */}
      <div
        class="a11y-demo-divbtn a11y-demo-switch"
        role="switch"
        tabIndex={0}
        aria-checked={muted()}
        aria-label="Mute"
        onClick={() => setMuted(!muted())}
      >
        <text>{muted() ? "muted" : "unmuted"} (Space to toggle)</text>
      </div>
    </div>
  );
}
