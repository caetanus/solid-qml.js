// C23 app-level contract on a VIRTUALIZED <For> (a certified passive row in a scrolling container,
// run by tests/c23/run-virtual.sh). Certified rows hold no state a script can observe, so the probe is
// the engine's row census (SQ_PHASE_STATS=1, printed at exit): rows CREATED or REBOUND from the pool.
// The script generates this once, runs one operation per process (OP), with the generated `key: "id"`
// line kept (keyed) or deleted (reference identity, the default), and compares against the mount-only
// run's window: a new object with an existing id is a fresh row by default (+1), reuses its row keyed.
import { createSignal, For, onMount } from "solid-js";
import { div, text } from "../../src/solid-qml/runtime";

declare const Qt: { quit: () => void };

export function ForVirtual() {
  const [rows, setRows] = createSignal([]);
  const op = "none";
  const build = (n) => Array.from({ length: n }, (_, i) => ({ id: i + 1, label: "row " + (i + 1) }));
  const swapped = (a) => {
    const r = a.slice();
    const t = r[1];
    r[1] = r[998];
    r[998] = t;
    return r;
  };
  const mutated = (a) => {
    a[3].label = "mutated";
    return a.slice();
  };
  const apply = (name) =>
    name === "replace" ? setRows(rows().map((r, i) => (i === 3 ? { id: r.id, label: "new four" } : r)))
    : name === "refetch" ? setRows(rows().map((r) => ({ id: r.id, label: r.label })))
    : name === "swap" ? setRows(swapped(rows()))
    : name === "remove" ? setRows(rows().filter((r, i) => i !== 500))
    : name === "mutate" ? setRows(mutated(rows()))
    : name === "clear" ? setRows([])
    : 0;

  // Rows arrive after the first layout (like the rows bench): the window is decided by then.
  onMount(() => {
    setTimeout(() => setRows(build(1000)), 100);
    setTimeout(() => {
      apply(op);
      setTimeout(() => {
        console.log("C23V done " + op + " " + rows().length);
        Qt.quit();
      }, 100);
    }, 300);
  });

  return (
    <div class="vlist">
      <For each={rows()} key="id">
        {(row) => (
          <div class="vrow">
            <text class="vid">{row.id}</text>
            <text class="vlabel">{row.label}</text>
          </div>
        )}
      </For>
    </div>
  );
}
