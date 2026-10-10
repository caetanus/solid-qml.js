// Web entry of the rows benchmark (examples/bench-rows.tsx).
import { render } from "solid-js/web";
import { BenchRows } from "./bench-rows";
import "./bench-rows.css";

render(() => <BenchRows />, document.getElementById("root")!);
