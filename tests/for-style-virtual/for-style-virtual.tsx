// A virtualized <For> whose rows are coloured by an inline style object (tests/for-style-virtual/run.sh):
// 2000 rows, each `style={{ background: row.c }}`, with a swatch coloured by a component signal
// (`style={{ background: tone(), width: 20 }}`: paint keys from any source, a layout key from a literal —
// the row's size stays predictable). The row is certified (forCertify.ts), so the repeater materializes
// only the window; run.sh drives the scroll and the signal from the generated QML and grabs frames.
import { createSignal, For, onMount } from "solid-js";
import { div } from "../../src/solid-qml/runtime";
import "./for-style-virtual.css";

const palette = ["#e53935", "#fb8c00", "#fdd835", "#43a047", "#00acc1", "#1e88e5", "#5e35b1"];

export function ForStyleVirtual() {
  const [rows, setRows] = createSignal([]);
  const [tone, setTone] = createSignal("#ffffff");
  // Called by the probe (run.sh) through the component instance: the same setter a handler would call.
  function retone() {
    setTone("#00ff00");
  }

  // Rows arrive after the first layout (like the rows bench): the window is decided by then.
  onMount(() => {
    setTimeout(() => {
      const t0 = Date.now();
      setRows(Array.from({ length: 2000 }, (_, i) => ({ id: i, c: palette[i % 7] })));
      const t1 = Date.now();
      setTimeout(() => console.log("FSV mounted sync " + (t1 - t0) + " ms +tick " + (Date.now() - t0) + " ms"), 0);
    }, 100);
  });

  return (
    <div class="slist">
      <For each={rows()}>
        {(row) => (
          <div class="srow" style={{ background: row.c }}>
            <div class="sdot" style={{ background: tone(), width: 20 }} />
          </div>
        )}
      </For>
    </div>
  );
}
