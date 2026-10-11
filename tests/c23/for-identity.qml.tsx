// Native entry of the C23 <For> identity contract test (tests/c23/for-identity.tsx, run by run.sh).
import { render } from "solid-js/web";
import { Window } from "../../src/solid-qml/runtime";
import { ForIdentity } from "./for-identity";

function App() {
  return (
    <Window title="c23" width={400} height={400} visible>
      <ForIdentity />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
