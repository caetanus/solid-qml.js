// Native entry of the rows benchmark (examples/bench-rows.tsx).
import { render } from "solid-js/web";
import { Window } from "../src/solid-qml/runtime";
import { BenchRows } from "./bench-rows";

function App() {
  return (
    <Window title="bench-rows" width={800} height={900} visible>
      <BenchRows />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
