// QML entry: the ONLY place <Window> lives (current side-by-side phase). It wraps <Gallery>, which
// is a plain component composing the example components. The transpiler follows the imports and
// emits one QML per component (App.generated.qml + Gallery.qml + Counters.qml + …).
import { render } from "solid-js/web";
import { Window } from "./solid-qml/runtime";
import { Gallery } from "../examples/gallery";

function App() {
  return (
    <Window title="solid-qml" width={760} height={520} visible>
      <Gallery />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
