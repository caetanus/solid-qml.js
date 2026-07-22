// solidterm entry — the ONLY place <Window> lives.
import { render } from "solid-js/web";
import { Window } from "../../../src/solid-qml/runtime";
import { Term } from "./term";

function App() {
  return (
    <Window title="solidterm" width={1000} height={640} visible>
      <Term />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
