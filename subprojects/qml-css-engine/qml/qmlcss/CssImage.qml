import QtQuick
import QtQuick.Effects
import "qrc:/qmlcss" as QmlCss

// CssImage — an <img>. Renders a source image with CSS `object-fit` → Image.fillMode, and
// (unlike Item.clip, which is always rectangular) clips to a ROUNDED rect when the resolved
// `border-radius` is non-zero, so `.avatar { border-radius: 50% }` yields a real circle. The
// round clip is a MultiEffect mask over the layered Image (the Qt6 idiom for a shaped image).
//
// Carries the CssQmlItem signature: identity in (cssId/cssClass/cssPrimitive/cssPart), resolved
// rules pushed back into `style` by cssTheme.loadCss(this) on completion / class change.
Item {
    id: root

    // --- CssQmlItem signature (read off this object by CssTheme::loadCss) ---
    property string cssId: ""
    property var cssAlternateId: []
    property var cssClass: []
    property string cssPrimitive: "img"
    property string cssPart: ""
    property var style: ({})

    // The image URL.
    property alias source: img.source

    // `object-fit` → Image.fillMode.
    readonly property string _objectFit: (style && style["object-fit"]) ? String(style["object-fit"]) : "cover"
    readonly property int _fillMode: {
        switch (root._objectFit) {
        case "contain": return Image.PreserveAspectFit
        case "fill": return Image.Stretch
        case "none": return Image.Pad
        case "scale-down": return Image.PreserveAspectFit
        default: return Image.PreserveAspectCrop // cover
        }
    }

    // Resolved corner radius; a round clip only kicks in when it's meaningful.
    readonly property real _radius: (typeof cssTheme !== "undefined" && cssTheme && style && style["border-radius"])
        ? cssTheme.parseLength(style["border-radius"], 0) : 0
    readonly property bool _rounded: _radius > 0.5

    visible: !(style && style["display"] === "none")

    Image {
        id: img
        anchors.fill: parent
        fillMode: root._fillMode
        asynchronous: true
        cache: true
        // When rounded, the image is drawn by the MultiEffect through the mask; otherwise draw direct.
        visible: !root._rounded
        layer.enabled: root._rounded
    }

    MultiEffect {
        anchors.fill: img
        source: img
        visible: root._rounded
        maskEnabled: true
        maskSource: mask
    }

    Item {
        id: mask
        anchors.fill: img
        layer.enabled: true
        visible: false
        Rectangle {
            anchors.fill: parent
            radius: root._radius
        }
    }

    readonly property bool _hasCssIdentity: root.cssId.length > 0
        || root.cssPart.length > 0
        || root.cssPrimitive.length > 0
        || (Array.isArray(root.cssClass) ? root.cssClass.length > 0 : String(root.cssClass).length > 0)

    Component.onCompleted: if (cssTheme && root._hasCssIdentity) cssTheme.loadCss(root)
    onCssClassChanged: if (cssTheme && root._hasCssIdentity) cssTheme.loadCss(root)
}
