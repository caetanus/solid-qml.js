// Web entry of the rows benchmark (examples/bench-rows.tsx).
// `?overflow=visible` turns the list's scrolling off (the native runner edits the CSS instead).
import { render } from "solid-js/web";
import { BenchRows } from "./bench-rows";
import "./bench-rows.css";

const overflow = new URLSearchParams(location.search).get("overflow");
if (overflow) {
  const style = document.createElement("style");
  style.textContent = `.bench { overflow-y: ${overflow}; }`;
  document.head.appendChild(style);
}

render(() => <BenchRows />, document.getElementById("root")!);
