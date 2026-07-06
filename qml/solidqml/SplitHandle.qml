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
