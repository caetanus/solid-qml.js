// Native entry of the virtualized inline-style <For> test (tests/for-style-virtual/run.sh).
import { render } from "solid-js/web";
import { Window } from "../../src/solid-qml/runtime";
import { ForStyleVirtual } from "./for-style-virtual";

function App() {
  return (
    <Window title="for-style-virtual" width={400} height={400} visible>
      <ForStyleVirtual />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
