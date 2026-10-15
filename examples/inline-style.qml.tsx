// Native entry of the inline-style showcase (examples/inline-style.tsx).
import { render } from "solid-js/web";
import { Window } from "../src/solid-qml/runtime";
import { InlineStyle } from "./inline-style";

function App() {
  return (
    <Window title="inline-style" width={520} height={260} visible>
      <InlineStyle />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
