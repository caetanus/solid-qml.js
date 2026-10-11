import { transformAsync, type BabelFileResult } from "@babel/core";
// Imported directly (not by string name) so Babel never has to RESOLVE a plugin package from the
// AUTHORED file's own directory. Babel's default plugin/preset resolution walks up the node_modules
// chain starting at `filename`'s directory — fine for files inside this checkout, but it throws
// MODULE_NOT_FOUND for a new app's entry living elsewhere (release-plan M6 clean-room finding: the
// transpiler is otherwise usable from outside the checkout via absolute paths). Passing the already
// -imported plugin object sidesteps that resolution entirely: Node resolved these from THIS file's
// own location before `normalize()` ever runs.
import presetTypescript from "@babel/preset-typescript";
import pluginTransformReactJsx from "@babel/plugin-transform-react-jsx";
import pluginTransformAsyncToPromises from "babel-plugin-transform-async-to-promises";

export interface Normalized {
  /** Normalized JS: JSX lowered to h() calls, TS types stripped. */
  code: string;
  /** Source map v3: normalized JS → original .tsx (map1 in the compose chain). */
  map: BabelFileResult["map"];
  /** Babel AST of the normalized JS, for the walker. */
  ast: BabelFileResult["ast"];
}

/**
 * Run Babel on author .tsx: strip TS types and lower JSX to a plain `h(tag, props, ...children)`
 * pragma (hyperscript — NOT Solid's DOM compiler). Reactivity is the transpiler's job downstream,
 * decided by each expression's position in the h() tree.
 */
export async function normalize(source: string, filename: string): Promise<Normalized> {
  const res = await transformAsync(source, {
    filename,
    babelrc: false,
    configFile: false,
    sourceMaps: true,
    ast: true,
    code: true,
    presets: [[presetTypescript, { isTSX: true, allExtensions: true }]],
    plugins: [
      [pluginTransformReactJsx, { pragma: "h", pragmaFrag: "hFrag", throwIfNamespace: false }],
      pluginTransformAsyncToPromises,
    ],
  });
  if (!res || res.code == null) throw new Error(`Babel failed to normalize ${filename}`);
  return { code: res.code, map: res.map, ast: res.ast };
}
