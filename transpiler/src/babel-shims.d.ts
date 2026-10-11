// @babel/preset-env ships no type declarations; the transpiler only passes it by reference to
// @babel/core as a preset entry, so an opaque module type is enough.
declare module "@babel/preset-env" {
  const preset: unknown;
  export default preset;
}
// Same for the plugins transform.ts passes by reference (so Babel never resolves them from the
// authored file's own directory — an app outside the checkout).
declare module "@babel/preset-typescript" {
  const preset: import("@babel/core").PluginItem;
  export default preset;
}
declare module "@babel/plugin-transform-react-jsx" {
  const plugin: import("@babel/core").PluginItem;
  export default plugin;
}
declare module "babel-plugin-transform-async-to-promises" {
  const plugin: import("@babel/core").PluginItem;
  export default plugin;
}
