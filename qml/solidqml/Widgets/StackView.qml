// StackView — the <StackView> component in module solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). PHASE-1 CONTRACT: children are pages, only child[current] is visible.
// T.StackView's API is imperative (push/pop/replace) and does not fit the declarative controlled-index
// shape, so this is a plain Css.CssRect host (cssPrimitive "stack"); the transpiler bakes a per-page
// `visible` guard (`(current) === k`) onto each child — the layout engine treats an invisible child as
// out of flow, so the visible page gets the full CSS box. Imperative push/pop via refs is a later phase.
//
// The transpiler emits:  W.StackView { cssClass: […]; <pages, each with a baked visible guard> }
import QtQuick
import qmlcss 1.0 as Css

Css.CssRect {
    id: root
    cssPrimitive: "stack"
    // Pages (with their baked visible guards) flow into the default content slot.
}
