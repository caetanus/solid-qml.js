// ToolBar — the <ToolBar> component in module solidqml.Widgets (owner directive 2026-07-05: one .qml
// per component; the transpiler INSTANTIATES these, it no longer hand-emits the chrome). Semantic
// native chrome: the CSS engine owns paint AND layout; T.ToolBar supplies the semantic role only.
//
// Children land in the WRAPPER's default content slot (its contentHolder), NOT the control's
// contentItem: the layout engine only flows a Css container's direct contentHolder children
// (csslayout.cpp layout() iterates content->childItems()), so hosting them inside the control's
// contentItem Item would orphan them from the author's flex/grid rules. The T.ToolBar is a plain
// (non-Css) sibling in the same holder — anchored full, skipped by the flex pass. Its contentItem is
// an empty Item so the template never instantiates style-less chrome of its own.
//
// The transpiler emits:  W.ToolBar { cssClass: […]; <children> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    cssPrimitive: "toolbar"

    T.ToolBar {
        anchors.fill: parent
        background: null
        contentItem: Item { }
    }
    // The author's children flow into the default content slot after this control.
}
