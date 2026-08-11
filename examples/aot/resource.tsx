// M-AOT-1 fixture: createResource + Suspense with a DETERMINISTIC async fetcher (no network — the
// twin-render must be reproducible). Proves the "dynamic frontier": the async JS runs on the
// QJSEngine sidecar and writes state.row back, and the C++ Suspense/text bindings react via NOTIFY.
import { createSignal, createResource, Suspense } from "solid-js";
import { div, text, Window } from "../../src/solid-qml/runtime";
import "../examples.css";

const fetchRow = async (id: string): Promise<{ label: string }> => ({ label: "row " + id });

function ResourceDemo() {
  const [id] = createSignal("42");
  const [row] = createResource(id, fetchRow);
  return (
    <div class="app">
      <Suspense fallback={<text class="bio">loading…</text>}>
        <text class="h1">{row().label}</text>
      </Suspense>
    </div>
  );
}
export function ResourceApp() {
  return <Window title="Resource" width={420} height={200} visible><ResourceDemo /></Window>;
}
