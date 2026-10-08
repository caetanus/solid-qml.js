// solidterm entry — the ONLY place <Window> lives.
import { createSignal } from "solid-js";
import { render } from "solid-js/web";
import { Window } from "../../../src/solid-qml/runtime";
import { Term } from "./term";

function App() {
  // The window title follows the focused pane's OSC title (shell/prompt), like any terminal.
  // NOT named `title`: it would shadow Window's own `title` (`title: title` binding loop).
  const [uiTitle, setUiTitle] = createSignal("solidterm");
  return (
    <Window title={uiTitle()} width={1000} height={640} visible>
      <Term onTitle={(t) => setUiTitle(t || "solidterm")} />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
