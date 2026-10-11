// Native entry of the document editor example (examples/document-editor.tsx).
import { render } from "solid-js/web";
import { Window } from "../src/solid-qml/runtime";
import { DocumentEditorDemo } from "./document-editor";

function App() {
  return (
    <Window title="Document editor" width={900} height={700} visible>
      <DocumentEditorDemo />
    </Window>
  );
}

render(() => <App />, document.getElementById("root")!);
