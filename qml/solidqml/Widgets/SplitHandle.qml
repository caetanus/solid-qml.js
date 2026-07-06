// SplitHandle — the SplitView drag handle in module solidqml.Widgets. The root is a plain Rectangle
// so its implicit THICKNESS survives the CSS engine's content-measure pass — a bare Css.CssRect reports
// implicitWidth 0 (it measures its empty content), so QQuickSplitView would reserve no space and the
// handle vanishes (the reported bug). The nested Css.CssRect carries the `.handle` styling; hover/press
// are read from THIS delegate root, where SplitView drives the SplitHandle attached properties
// (referencing them in the child would attach a fresh, undriven handle). `cssAncestor` re-anchors the
// CSS walk to the author's SplitView wrapper so `.my-split .handle` rules match across the reparent.
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Rectangle {
    id: root
    color: "transparent"
    property Item cssAncestor: null
    property bool horizontal: true
    property int thickness: 6
    property bool handleHovered: T.SplitHandle.hovered
    property bool handlePressed: T.SplitHandle.pressed
    implicitWidth: horizontal ? thickness : (parent ? parent.width : thickness)
    implicitHeight: horizontal ? (parent ? parent.height : thickness) : thickness
    Css.CssRect {
        anchors.fill: parent
        property Item cssAncestor: root.cssAncestor
        cssPrimitive: "div"
        cssClass: ["handle"]
        cssState: (root.handlePressed ? ["active"] : []).concat(root.handleHovered ? ["hover"] : [])
    }
}
