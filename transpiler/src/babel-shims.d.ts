// @babel/preset-env ships no type declarations; the transpiler only passes it by reference to
// @babel/core as a preset entry, so an opaque module type is enough.
declare module "@babel/preset-env" {
  const preset: unknown;
  export default preset;
}
