// Web entry of the rows benchmark (examples/bench-mem.tsx).
import { render } from "solid-js/web";
import { BenchMem } from "./bench-mem";
import "./bench-rows.css";

render(() => <BenchMem />, document.getElementById("root")!);
