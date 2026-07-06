// Div — the block-primitive component (solidqml.Widgets). Owner directive 2026-07-05: every
// element is a component so the QML engine compiles it ONCE and reuses/AOT-caches it, instead of
// re-compiling a unique inline Css.CssRect block per <div> in every generated document (which the
// engine can neither share nor qmltc-compile to a C++ class — confirmed against the Qt Quick
// Compiler docs). One cached type serves every <div>/<section>/… (the transpiler sets cssPrimitive
// for non-div tags). cssClass/cssState/children are inherited from CssRect; interactive variants
// (onClick/draggable/onDrop) add a MouseArea/Drag/DropArea child at the instantiation site.
import QtQuick
import qmlcss 1.0 as Css

Css.CssRect {
    cssPrimitive: "div"
}
