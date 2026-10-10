// Rows benchmark (js-framework-benchmark style): the SAME component runs on the web and natively,
// times each operation from inside the app and logs `BENCH <op> <ms>` lines. Times are synchronous
// (the state write until it returns; on the web a forced layout is included, natively the CSS
// layout is synchronous) plus `+tick` (until the next timer turn, catching deferred work).
import { createSignal, For, onMount } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";

const ADJ = ["pretty", "large", "big", "small", "tall", "short", "long", "handsome", "plain", "quaint"];
const OPS = 6;
const ROUNDS = 3;
const NOUN = ["table", "chair", "house", "bbq", "desk", "car", "pony", "cookie", "sandwich", "burger"];

export function BenchRows() {
  const [rows, setRows] = createSignal([]);
  let nextId = 1;

  // (Array.from, not a `for (;;)`: the transpiler rejects ForStatement in a component body.)
  const build = (n) => {
    const base = nextId;
    nextId += n;
    return Array.from({ length: n }, (_, i) => ({
      id: base + i,
      // The id suffix keeps every label unique: a text/shaping cache must not win on 10 repeated strings.
      label: ADJ[(base + i) % 10] + " " + NOUN[Math.floor((base + i) / 10) % 10] + " " + (base + i),
    }));
  };

  const settle = () => {
    if (typeof document !== "undefined") return document.body.offsetHeight;
    return 0;
  };

  // (A function, not a `const ops = [...]`: the transpiler hoists component-body consts that hold
  // closures into Component.onCompleted, out of reach of run() — a scoping bug found by this bench.)
  const opAt = (k) => [
    ["create1k", () => setRows(build(1000))],
    ["update10th", () => setRows(rows().map((r, i) => (i % 10 === 0 ? { id: r.id, label: r.label + " !!!" } : r)))],
    ["swap", () => {
      const a = rows().slice();
      const t = a[1];
      a[1] = a[998];
      a[998] = t;
      setRows(a);
    }],
    ["remove1", () => setRows(rows().filter((r, i) => i !== 500))],
    ["append1k", () => setRows(rows().concat(build(1000)))],
    ["clear", () => setRows([])],
  ][k];

  const run = (step) => {
    const total = OPS * ROUNDS;
    if (step >= total) {
      console.log("BENCH done");
      return;
    }
    const op = opAt(step % OPS);
    const t0 = performance.now();
    op[1]();
    settle();
    const t1 = performance.now();
    setTimeout(() => {
      const t2 = performance.now();
      console.log("BENCH " + op[0] + " " + (t1 - t0).toFixed(1) + " +tick " + (t2 - t0).toFixed(1));
      setTimeout(() => run(step + 1), 50);
    }, 0);
  };

  onMount(() => {
    setTimeout(() => run(0), 300);
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
