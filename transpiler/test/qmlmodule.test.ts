/** `qml:` imports — registered-module foreign types (Direção B of the embed plan): pre-existing
 *  C++ shipped as a canonical QML module (qmldir + plugin), consumed from TSX. */

import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

const SRC = `
  import { createSignal } from "solid-js";
  import { AcmeGauge } from "qml:Acme";
  export function App() {
    const [v, setV] = createSignal(30);
    return (
      <div class="gd">
        <AcmeGauge value={v()} width={140} height={140} />
        <button onClick={() => setV(v() + 10)}>+10</button>
      </div>
    );
  }
`;

test("qml-module: import { T } from \"qml:Uri\" emits the namespaced module import", async () => {
  const app = await generate(SRC, "/tmp/t/app.tsx");
  assert.match(app.entry, /import Acme 1\.0 as QM_Acme/);
});

test("qml-module: the type instantiates namespaced inside the foreign Css box wrap", async () => {
  const app = await generate(SRC, "/tmp/t/app.tsx");
  // Same escape-hatch contract as a foreign `.qml` file: bare Css box + anchors.fill, with
  // width/height routed to the foreign's implicit size.
  assert.match(app.entry, /Css\.CssRect \{\n\s*cssPrimitive: "div"\n\s*QM_Acme\.AcmeGauge \{\n\s*anchors\.fill: parent/);
  assert.match(app.entry, /implicitWidth: 140/);
  // Reactive props bind like any instance prop.
  assert.match(app.entry, /value: v/);
});

test("qml-module: a version suffix selects the module version", async () => {
  const src = `
    import { Thing } from "qml:Acme.Extras@2.3";
    export function App() { return <div><Thing /></div>; }
  `;
  const app = await generate(src, "/tmp/t/app.tsx");
  assert.match(app.entry, /import Acme\.Extras 2\.3 as QM_Acme_Extras/);
  assert.match(app.entry, /QM_Acme_Extras\.Thing \{/);
});

test("qml-module: a lowercase type name is rejected with a clear error", async () => {
  const src = `
    import { gauge } from "qml:Acme";
    export function App() { return <div><gauge /></div>; }
  `;
  // Lowercase tags route to the HTML registry, so the import is simply never used as a
  // component — but importing it explicitly under a Capitalized local alias must validate.
  const bad = `
    import { widget as Widget } from "qml:9cme";
    export function App() { return <div><Widget /></div>; }
  `;
  await assert.rejects(generate(bad, "/tmp/t/app.tsx"), /module URI|Capitalized/);
});

test("qml-module: qml: specifiers are excluded from the npm mirror pass", async () => {
  const app = await generate(SRC, "/tmp/t/app.tsx");
  // No .mjs module emitted for the qml: import.
  assert.equal(Object.keys(app.modules ?? {}).length, 0);
});
