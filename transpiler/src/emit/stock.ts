// Stock QML components — hand-written primitives/hosts shipped alongside generated output
// (owner directive 2026-07-04: primitives are .qml components, not hand-wired multi-node hosts;
// this cuts scene-graph nodes now and gives a 1:1 target for future C++ types). They ride the
// `components` map like any emitted component (gen.mjs writes each as `<Name>.qml`); QML only
// instantiates one when a generated file references it by type name, so an unused stock file
// costs nothing at runtime.
//
// Naming is QML-native (NOT web/DOM names): these are Qt scene primitives, not HTML elements.

// SplitHandle graduated into the solidqml.Widgets module (Widgets/SplitHandle.qml) during the
// widget migration, so there are no stock components anymore. The mechanism (scan + emit) is kept
// for a possible future need; the map is empty.
export const STOCK_COMPONENTS: Record<string, string> = {};
