// Custom .qml import (native escape hatch): a hand-written QML file imported from TSX is
// copied VERBATIM to the output and instantiated by type name, with props bound and signal
// handlers wired — the transpiler never parses the QML.
import { test } from "node:test";
import assert from "node:assert/strict";
import { generate } from "../src/index.ts";

const BADGE = `import QtQuick
Rectangle { property string label: "b"; property int count: 0; signal bumped() }`;

async function gen(app: string, extra: Record<string, string> = {}) {
  const files: Record<string, string> = { "/app/main.tsx": app, "/app/Badge.qml": BADGE, ...extra };
  return generate(files["/app/main.tsx"], "/app/main.tsx", {
    readFile: async (p) => { if (files[p]) return files[p]; throw new Error(`ENOENT ${p}`); },
  });
}

const HOST = `
import { createSignal } from "solid-js";
import Badge from "./Badge.qml";
export function Host() {
  const [n, setN] = createSignal(0);
  return <div><Badge label="clicks" count={n()} onBumped={() => setN(n() + 1)} /></div>;
}`;

test("customqml: the .qml file is copied verbatim to components under its basename", async () => {
  const app = await gen(HOST);
  assert.equal(app.components["Badge"], BADGE);
});

test("customqml: the tag is instantiated by type name with props bound", async () => {
  const app = await gen(HOST);
  assert.match(app.entry, /Badge \{/);
  assert.match(app.entry, /label: "clicks"/);
  assert.match(app.entry, /count: n\b/);
});

test("customqml: on* handler props wire to the QML signal with setter translation", async () => {
  const app = await gen(HOST);
  assert.match(app.entry, /onBumped: function\(\) \{ return n = n \+ 1 \}/);
});

test("customqml: a lowercase .qml basename is rejected (must be a QML type name)", async () => {
  // Tag stays Capitalized (so it's discovered as a component), only the FILE is lowercase.
  const bad = `
    import Badge from "./badge.qml";
    export function Host() { return <div><Badge /></div>; }`;
  await assert.rejects(
    generate(bad, "/app/main.tsx", {
      readFile: async (p) => (p.endsWith("badge.qml") ? BADGE : p.endsWith("main.tsx") ? bad : (() => { throw new Error("ENOENT " + p); })()),
    }),
    /Capitalized/,
  );
});

test("customqml: the same .qml imported twice is copied once", async () => {
  const two = `
    import { createSignal } from "solid-js";
    import Badge from "./Badge.qml";
    function Row() { return <div><Badge label="a" /></div>; }
    export function Host() { return <div><Row /><Badge label="b" /></div>; }`;
  const app = await gen(two);
  assert.equal(Object.keys(app.components).filter((n) => n === "Badge").length, 1);
});
