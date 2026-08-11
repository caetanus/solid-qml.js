// M-AOT-1 fixture: multi-file — the entry imports a component defined in ANOTHER file (the real
// gallery shape). Proves the cpp back-end follows relative imports across the module graph.
import { Window } from "../../src/solid-qml/runtime";
import { Hello } from "../hello";
export function MultiApp() {
  return <Window title="Multi" width={800} height={600} visible><Hello /></Window>;
}
