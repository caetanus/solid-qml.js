// Memory probe for the rows benchmark: logs MEM base, mounts 2000 rows, logs MEM rows, then idles.
import { createSignal, For, onMount } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";

export function BenchMem() {
  const [rows, setRows] = createSignal([]);
  onMount(() => {
    setTimeout(() => {
      console.log("MEM base");
      setTimeout(() => {
        setRows(Array.from({ length: 2000 }, (_, i) => ({ id: i + 1, label: "row label " + (i + 1) })));
        setTimeout(() => console.log("MEM rows"), 500);
      }, 2000);
    }, 1000);
  });
  return (
    <div class="bench">
      <For each={rows()}>
        {(row) => (
          <div class="bench-row">
            <text class="bench-id">{row.id}</text>
            <text class="bench-label">{row.label}</text>
          </div>
        )}
      </For>
    </div>
  );
}
