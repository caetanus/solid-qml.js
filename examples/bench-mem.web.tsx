// Web entry of the memory probe (examples/bench-mem.tsx).
// `?overflow=visible` turns the list's scrolling off (the native runner edits the CSS instead).
import { render } from "solid-js/web";
import { BenchMem } from "./bench-mem";
import "./bench-rows.css";

const overflow = new URLSearchParams(location.search).get("overflow");
if (overflow) {
  const style = document.createElement("style");
  style.textContent = `.bench { overflow-y: ${overflow}; }`;
  document.head.appendChild(style);
}

render(() => <BenchMem />, document.getElementById("root")!);
