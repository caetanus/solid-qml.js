// Native entry of the rows benchmark (examples/bench-mem.tsx).
import { render } from "solid-js/web";
import { Window } from "../src/solid-qml/runtime";
import { BenchMem } from "./bench-mem";

function App() {
  return (
    <Window title="bench-mem" width={800} height={900} visible>
      <BenchMem />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
