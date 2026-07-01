import { transformAsync, type BabelFileResult } from "@babel/core";

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
    presets: [["@babel/preset-typescript", { isTSX: true, allExtensions: true }]],
    plugins: [
      ["@babel/plugin-transform-react-jsx", { pragma: "h", pragmaFrag: "hFrag", throwIfNamespace: false }],
      "babel-plugin-transform-async-to-promises",
    ],
  });
  if (!res || res.code == null) throw new Error(`Babel failed to normalize ${filename}`);
  return { code: res.code, map: res.map, ast: res.ast };
}
