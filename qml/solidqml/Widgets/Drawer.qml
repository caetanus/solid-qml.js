// Drawer — the <Drawer> edge-panel component in module solidqml.Widgets (owner directive 2026-07-05:
// one .qml per component). A T.Drawer with CSS support. The root is a zero-size in-tree anchor (a
// Css.CssItem carrying the author's classes) so `.my-drawer .panel` rules scope through the
// cssAncestor re-anchor below; it paints nothing and flows as an empty box.
//
// Drawer contents reparent to the window Overlay, severing the visual chain the author's CSS matches
// against — so background AND contentItem each carry `cssAncestor: root` (MANDATORY popup pitfall;
// they are sibling slots, so the two cover every descendant). No popupType Window — a drawer is an
// in-window panel.
//
// T.Drawer carries no built-in open/close animation — the STYLE must supply enter/exit transitions
// that drive `position` 0↔1 (without them `open()`/`visible:true` set `opened:true` but `position`
// stays 0, leaving the panel off-screen). The cross axis is NOT sized automatically by QQuickDrawer,
// so left/right take width = overlay.width × size (default 0.34), full height; top/bottom transposed.
//
// The controlled `open` (a two-way alias to the drawer's `visible`) drives visibility via the emit's
// RestoreNone Binding; Qt-side closes (Esc / press outside) fire the `closed` signal → the author's
// onClose keeps the signal in sync.
//
// The transpiler emits:  W.Drawer { id: __drawerN; cssClass: […]; edge; size; onClosed; <children>;
//                                   Binding on open }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssItem {
    id: root
    property int edge: Qt.LeftEdge
    property real size: 0.34
    property alias open: ctl.visible
    default property alias content: contentBox.content
    signal closed()

    readonly property bool __horizontal: edge === Qt.LeftEdge || edge === Qt.RightEdge

    cssPrimitive: "drawer"

    T.Drawer {
        id: ctl
        parent: T.Overlay.overlay
        edge: root.edge
        // Desktop semantics: no edge-swipe open (an interactive drag would fight the controlled Binding).
        dragMargin: 0
        width: parent ? (root.__horizontal ? parent.width * root.size : parent.width) : 0
        height: parent ? (root.__horizontal ? parent.height : parent.height * root.size) : 0
        enter: Transition { NumberAnimation { property: "position"; to: 1.0; duration: 220; easing.type: Easing.OutCubic } }
        exit: Transition { NumberAnimation { property: "position"; to: 0.0; duration: 180; easing.type: Easing.InCubic } }
        // Semi-transparent modal scrim (default style provides none → an opaque dim).
        T.Overlay.modal: Rectangle { color: "#66000000" }
        background: Css.CssFill {
            property Item cssAncestor: root
            cssPrimitive: "div"
            cssClass: ["panel"]
        }
        contentItem: Css.CssFill {
            id: contentBox
            property Item cssAncestor: root
            cssPrimitive: "div"
            cssClass: ["content"]
        }
        onClosed: root.closed()
    }
}
