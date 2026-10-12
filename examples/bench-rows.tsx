// Rows benchmark (js-framework-benchmark style): the SAME component runs on the web and natively,
// times each operation from inside the app and logs one line per operation:
//   BENCH <op> <sync ms> +tick <ms> +frame <ms> round <r>
// - sync: from the state write until it returns (on the web a forced layout is included —
//   `document.body.offsetHeight`; natively the CSS layout is synchronous);
// - +tick: until the next zero-delay timer turn (catches deferred work);
// - +frame: until the next PRESENTED frame after the mutation. On the web this is a double
//   requestAnimationFrame; natively the runner (scripts/bench/run.mjs) substitutes `presented()` in
//   the generated QML with a hook on the window's frameSwapped signal (there is no rAF in QML).
// Parameters: rows created by "create" (BENCH_N, default 1000) and rounds (BENCH_ROUNDS, default 3);
// natively from the environment (process.env), on the web from the URL query (?n=…&rounds=…).
// Methodology: docs/performance.md.
import { createSignal, For, onMount } from "solid-js";
import { div, text } from "../src/solid-qml/runtime";

const ADJ = ["pretty", "large", "big", "small", "tall", "short", "long", "handsome", "plain", "quaint"];
const OPS = 6;
const DEFAULT_N = 1000;
const DEFAULT_ROUNDS = 3;
const NOUN = ["table", "chair", "house", "bbq", "desk", "car", "pony", "cookie", "sandwich", "burger"];

export function BenchRows() {
  const [rows, setRows] = createSignal([]);
  let nextId = 1;

  // Native: process.env (the loader's desktop capability profile); web: the URL query.
  const param = (name, dflt) => {
    if (typeof process !== "undefined" && process.env && process.env["BENCH_" + name.toUpperCase()])
      return Number(process.env["BENCH_" + name.toUpperCase()]);
    if (typeof location !== "undefined") {
      const hit = location.search.slice(1).split("&").filter((p) => p.indexOf(name + "=") === 0);
      if (hit.length > 0) return Number(hit[0].slice(name.length + 1));
    }
    return dflt;
  };

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

  // Calls cb once the frame that contains the mutation has been presented. Web: double rAF (the
  // second callback runs at the start of the frame after the one that painted the mutation).
  // Native: replaced by the runner (frameSwapped); this fallback only keeps a plain run working.
  const presented = (cb) => {
    if (typeof requestAnimationFrame !== "undefined") {
      requestAnimationFrame(() => requestAnimationFrame(cb));
      return;
    }
    setTimeout(cb, 0);
  };

  // (A function, not a `const ops = [...]`: the transpiler hoists component-body consts that hold
  // closures into Component.onCompleted, out of reach of run() — a scoping bug found by this bench.)
  const opAt = (k) => [
    ["create1k", () => setRows(build(param("n", DEFAULT_N)))],
    ["update10th", () => setRows(rows().map((r, i) => (i % 10 === 0 ? { id: r.id, label: r.label + " !!!" } : r)))],
    ["swap", () => {
      // js-framework-benchmark swaps rows 1 and 998; for N < 1000 that index would extend the
      // array with holes, so clamp the second index to the last-but-one row instead. Below N=3
      // there is nothing meaningful to swap, so both indices fall back to -1 (a non-index property
      // read/written on the array, never touching .length) and the swap is a no-op.
      // (No `if`: the transpiler's inline-block emitter only supports var/return/expression
      // statements, so the "only when a.length >= 3" guard is folded into the index values.)
      const a = rows().slice();
      const i = a.length >= 3 ? 1 : -1;
      const j = a.length >= 3 ? Math.min(998, a.length - 2) : -1;
      const t = a[i];
      a[i] = a[j];
      a[j] = t;
      setRows(a);
    }],
    ["remove1", () => {
      const i = Math.min(500, Math.floor(rows().length / 2));
      setRows(rows().filter((r, idx) => idx !== i));
    }],
    ["append1k", () => setRows(rows().concat(build(1000)))],
    ["clear", () => setRows([])],
  ][k];

  // Logs one operation once BOTH its timer turn and its presented frame have been observed.
  const report = (step, name, t0, t1, st) => {
    if (st.tick < 0 || st.frame < 0 || st.done) return;
    st.done = true;
    console.log("BENCH " + name + " " + (t1 - t0).toFixed(2) + " +tick " + st.tick.toFixed(2)
      + " +frame " + st.frame.toFixed(2) + " round " + Math.floor(step / OPS));
    setTimeout(() => run(step + 1), 50);
  };

  const run = (step) => {
    const total = OPS * param("rounds", DEFAULT_ROUNDS);
    if (step >= total) {
      console.log("BENCH done");
      return;
    }
    const op = opAt(step % OPS);
    const st = { tick: -1, frame: -1, done: false };
    const t0 = performance.now();
    op[1]();
    settle();
    const t1 = performance.now();
    setTimeout(() => {
      st.tick = performance.now() - t0;
      report(step, op[0], t0, t1, st);
    }, 0);
    presented(() => {
      st.frame = performance.now() - t0;
      report(step, op[0], t0, t1, st);
    });
  };

  onMount(() => {
    console.log("BENCH mounted");
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
