// Node-module imports — an app component that pulls REAL npm packages, registered as the test case
// for the future npm JS-module mirror walk (queue item 3: resources + imports). It exercises two
// shapes the mirror must handle:
//   • js-base64       — a 0-dependency leaf module.
//   • wrap-ansi@10    — a small transitive tree WITH A DIAMOND:
//         wrap-ansi → ansi-styles
//                   → string-width → get-east-asian-width
//                                  → strip-ansi → ansi-regex   ┐ strip-ansi is reached via TWO
//                   → strip-ansi → ansi-regex                  ┘ paths → must be mirrored ONCE.
// On the web target this runs unchanged. On QML, the bare specifiers are NOT author components, so
// they are skipped today (v1 never transpiled node_modules; v2's resolveModule returns null for
// non-relative specifiers). Item 3 will mirror this whole tree as V4 .mjs modules, dedup the
// diamond, rewrite each mirror's import specifiers, and terminate on cycles — proven against THIS
// example (run `npm ls wrap-ansi --all` to see the tree the mirror set must equal).
import { Base64 } from "js-base64";
import wrapAnsi from "wrap-ansi";
import { div, text } from "../src/solid-qml/runtime";

export function NodeImports() {
  // Electron-idiom runtime probe: `process` is an ambient V4 global on the native
  // target and undefined in the browser — one shared source, two truths.
  const runtime = typeof process !== "undefined" && process.versions && process.versions.solidQml
    ? "solid-qml " + process.versions.solidQml + " (Qt " + process.versions.qt + ", " + process.platform + ")"
    : "browser";
  const encoded = Base64.encode("solid-qml");
  const decoded = Base64.decode(encoded);
  const wrapped = wrapAnsi(
    "wrap-ansi pulls a small dependency tree with a diamond on strip-ansi",
    24,
  );

  return (
    <div class="app">
      <text class="title">node module imports</text>
      <text>runtime: {runtime}</text>
      <text>Base64.encode("solid-qml") = {encoded}</text>
      <text>Base64.decode(...) = {decoded}</text>
      <text>wrapAnsi(text, 24) →</text>
      <text>{wrapped}</text>
    </div>
  );
}
