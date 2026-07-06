// SplitView — the <SplitView> component in module solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). A T.SplitView with CSS support: wrapper CssFill (cssPrimitive "splitview") +
// the control. The panes route into the control's contentData via the default `panes` alias — the
// Container manages their geometry (each pane's implicit size comes from its own Css layout).
//
// The handle is the module-local SplitHandle component: a plain-Rectangle root keeps a real implicit
// thickness (a bare Css.CssRect measures its empty content as 0 → SplitView reserves no space →
// invisible handle). `cssAncestor: root` re-anchors the CSS walk to this wrapper so `.my-split .handle`
// rules match. Per-pane resizability comes from the `T.SplitView.fillWidth/fillHeight` hint the
// transpiler injects into each pane (a pane with no size hint lets the first pane's content eat the row).
//
// The transpiler emits:  W.SplitView { cssClass: […]; orientation: Qt.Horizontal|Qt.Vertical;
//                                      <panes, each with an injected fill hint> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property int orientation: Qt.Horizontal
    default property alias panes: split.contentData

    cssPrimitive: "splitview"

    T.SplitView {
        id: split
        anchors.fill: parent
        orientation: root.orientation
        handle: SplitHandle {
            cssAncestor: root
            horizontal: split.orientation === Qt.Horizontal
        }
    }
}
