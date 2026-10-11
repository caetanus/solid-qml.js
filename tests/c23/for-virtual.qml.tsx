// Native entry of the C23 virtualized <For> contract test (tests/c23/for-virtual.tsx, run-virtual.sh).
import { render } from "solid-js/web";
import { Window } from "../../src/solid-qml/runtime";
import { ForVirtual } from "./for-virtual";

function App() {
  return (
    <Window title="c23v" width={400} height={400} visible>
      <ForVirtual />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
