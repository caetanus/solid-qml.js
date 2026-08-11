// M-AOT-1 fixture: the FULL async data view (createResource + Suspense + Show + img + input + member
// access) compiled to C++. Hits the live GitHub API, so it is NOT twin-renderable (non-deterministic)
// — this fixture proves the whole view emits VALID C++ that compiles; the sidecar mechanism itself is
// verified deterministically by resource.tsx.
import { Window } from "../../src/solid-qml/runtime";
import { Fetch } from "../fetch";
export function FetchApp() {
  return <Window title="Fetch" width={600} height={500} visible><Fetch /></Window>;
}
