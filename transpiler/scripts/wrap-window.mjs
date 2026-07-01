// Wrap a generated component QML (a bare CssRect/CssFill type) in a Window so it can be loaded and
// rendered for a real-render verification (not just a parse check). Reads a .qml path, prints the
// Window-wrapped QML to stdout.
//
//   node transpiler/scripts/wrap-window.mjs <component.qml>
import { readFileSync } from "node:fs";

const path = process.argv[2];
if (!path) {
  console.error("usage: wrap-window.mjs <component.qml>");
  process.exit(2);
}
const src = readFileSync(path, "utf8");
const lines = src.split("\n");

// Keep the import lines; the body is everything after the last top-level import.
const imports = lines.filter((l) => /^import\s/.test(l));
let bodyStart = 0;
for (let i = 0; i < lines.length; i++) if (/^import\s/.test(lines[i])) bodyStart = i + 1;
// skip a single blank line after the imports
while (bodyStart < lines.length && lines[bodyStart].trim() === "") bodyStart++;
const body = lines.slice(bodyStart).join("\n").trimEnd();

// Mimic the real app root: a Window holding a CssRect "window" surface that lays out the component
// (a bare Window doesn't size its children, so the component would render at zero size).
const out = [
  ...new Set([...imports, "import QtQuick"]), // QtQuick provides Window; dedupe
  "",
  "Window {",
  "    width: 480",
  "    height: 320",
  "    visible: true",
  "    Css.CssRect {",
  "        id: __root",
  "        anchors.fill: parent",
  "        cssClass: [\"qml-window\"]",
  "        cssPrimitive: \"window\"",
  body.replace(/^/gm, "        "),
  "    }",
  "}",
  "",
].join("\n");

process.stdout.write(out);
