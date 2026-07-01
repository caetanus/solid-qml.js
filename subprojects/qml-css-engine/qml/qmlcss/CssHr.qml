import QtQuick

// Native <hr>: a CSS-addressable Qt Quick item that paints only its top border.
Item {
    id: root

    property string cssId: ""
    property var cssAlternateId: []
    property var cssClass: []
    property var cssState: []
    property string cssPrimitive: "hr"
    property string cssPart: ""
    property var style: ({})

    readonly property bool hasCssIdentity: root.cssId.length > 0
        || root.cssPart.length > 0
        || root.cssPrimitive.length > 0
        || (Array.isArray(root.cssClass) ? root.cssClass.length > 0 : String(root.cssClass).length > 0)
    readonly property var line: cssTheme.borderSide(style || ({}), "top", 1, "#ededed")

    visible: !(style && style["display"] === "none")
    implicitWidth: 0
    implicitHeight: Math.max(1, root.line.width)

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.max(1, root.line.width)
        color: root.line.color
        visible: root.line.visible
    }

    Component.onCompleted: {
        if (cssTheme && root.hasCssIdentity)
            cssTheme.loadCss(root)
        cssLayout.notifyParentLayout(root)
    }

    onCssIdChanged: if (cssTheme && root.hasCssIdentity) cssTheme.loadCss(root)
    onCssClassChanged: if (cssTheme && root.hasCssIdentity) cssTheme.loadCss(root)
    onCssStateChanged: if (cssTheme && root.hasCssIdentity) cssTheme.loadCss(root)
    onStyleChanged: cssLayout.notifyParentLayout(root)
    onImplicitHeightChanged: cssLayout.notifyParentLayout(root)
    onVisibleChanged: cssLayout.notifyParentLayout(root)
}
