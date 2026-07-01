import QtQuick
import QtQuick.Effects
import "qrc:/qmlcss/Contrast.js" as Contrast

Item {
    id: root

    property var style: ({})
    property string fallbackSource: ""
    property string fallbackIconName: ""
    property color color: "white"
    property bool colorize: true
    property int iconSize: Math.min(width, height)

    readonly property string cssIconValue: style && style["icon"] ? style["icon"]
        : (style && style["icon-image"] ? style["icon-image"] : "")
    readonly property string cssIconName: style && style["icon-name"] ? style["icon-name"] : ""
    readonly property bool hasCustomIcon: cssIconValue.length > 0 || cssIconName.length > 0
    readonly property bool ready: iconImage.status === Image.Ready
    readonly property string iconSource: {
        if (cssIconName.length > 0) {
            return "image://themeicon/" + cssIconName + "|" + Contrast.toHex(root.color).slice(1)
        }
        if (cssIconValue.length > 0) {
            return root.sourceFromCssUrl(cssIconValue)
        }
        if (fallbackIconName.length > 0) {
            return "image://themeicon/" + fallbackIconName + "|" + Contrast.toHex(root.color).slice(1)
        }
        return fallbackSource
    }

    function sourceFromCssUrl(value) {
        var s = value.trim()
        if (s.toLowerCase().indexOf("url(") === 0) {
            s = s.substring(4, s.length - 1).trim()
        }
        if ((s[0] === "\"" && s[s.length - 1] === "\"") || (s[0] === "'" && s[s.length - 1] === "'")) {
            s = s.substring(1, s.length - 1)
        }
        if (s.length === 0) {
            return ""
        }
        if (s.indexOf("qrc:/") === 0 || s.indexOf("file:/") === 0 || s.indexOf("image://") === 0) {
            return s
        }
        if (s[0] === "/") {
            return "file://" + s
        }
        return s
    }

    Image {
        id: iconImage
        anchors.centerIn: parent
        width: root.iconSize
        height: root.iconSize
        sourceSize.width: root.iconSize
        sourceSize.height: root.iconSize
        source: root.iconSource
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        visible: false
    }

    MultiEffect {
        anchors.fill: iconImage
        source: iconImage
        visible: iconImage.status === Image.Ready
        colorization: root.colorize ? 1.0 : 0.0
        colorizationColor: root.color
    }
}
