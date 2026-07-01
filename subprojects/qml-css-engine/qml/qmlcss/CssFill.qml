import QtQuick
import "qrc:/qmlcss" as QmlCss

// Thin compatibility shim over CssRect (the Shape-based fill renderer). A Shape cannot
// fill with an image, so CssFill adds the `background-image` layer BEHIND a CssRect;
// everything else — solid colour, linear-gradient, border, box-shadow, inset bevel and
// the CSS `transition` fade — is CssRect. New code should use CssRect directly; CssFill
// remains for container/popup callers that use url() image backgrounds.
Item {
    id: root

    // --- CssQmlItem signature (same contract as CssRect) -------------------------------
    property string cssId: ""
    property var cssAlternateId: []
    property var cssClass: []
    property var cssState: []
    property string cssPrimitive: "rect"
    property string cssPart: ""
    property var style: ({})

    property real radius: 0
    property color defaultColor: "transparent"
    property color defaultBorderColor: "transparent"
    property real defaultBorderWidth: 0
    // Deprecated: the transition is derived from `style` by CssRect now. Kept so existing
    // callers that still assign these don't error — they have no effect.
    property int transitionMs: 0
    property int transitionEasingType: 0

    readonly property string backgroundValue: (style && style["background"]) ? style["background"] : ""
    readonly property string imageValue: (style && style["background-image"]) ? style["background-image"]
        : (root.isCssUrl(backgroundValue) ? backgroundValue : "")
    readonly property bool bgIsImage: root.isCssUrl(imageValue)
    readonly property string bgImageSource: root.imageSource(imageValue)
    readonly property color solidColor: (style && style["background-color"])
        ? cssTheme.parseColor(style["background-color"]) : root.defaultColor

    // Inheritable text props carried for CHILDREN (a <button> is a CssFill with `color`/font, and its
    // label is a direct CssText child). Own style wins, else the CSS-inheriting container two levels
    // up — same chain as CssRect/CssText, so the label gets the button's colour AND the container's
    // font. All are exposed (not just colour) so CssText reading `_cssParent.inherited*` never hits
    // undefined.
    readonly property var _cssParent: (parent && parent.parent && parent.parent.inheritedColor !== undefined)
        ? parent.parent : null
    readonly property string inheritedColor: (style && style["color"]) ? style["color"] : (_cssParent ? _cssParent.inheritedColor : "")
    readonly property string inheritedFontFamily: (style && style["font-family"]) ? style["font-family"] : (_cssParent ? _cssParent.inheritedFontFamily : "")
    readonly property string inheritedFontSize: (style && style["font-size"]) ? style["font-size"] : (_cssParent ? _cssParent.inheritedFontSize : "")
    readonly property string inheritedFontWeight: (style && style["font-weight"]) ? style["font-weight"] : (_cssParent ? _cssParent.inheritedFontWeight : "")
    readonly property string inheritedLineHeight: (style && style["line-height"]) ? style["line-height"] : (_cssParent ? _cssParent.inheritedLineHeight : "")
    readonly property string inheritedLetterSpacing: (style && style["letter-spacing"]) ? style["letter-spacing"] : (_cssParent ? _cssParent.inheritedLetterSpacing : "")
    readonly property string inheritedTextTransform: (style && style["text-transform"]) ? style["text-transform"] : (_cssParent ? _cssParent.inheritedTextTransform : "")
    readonly property string inheritedTextAlign: (style && style["text-align"]) ? style["text-align"] : (_cssParent ? _cssParent.inheritedTextAlign : "")

    function isCssUrl(value) {
        return value && value.trim().toLowerCase().indexOf("url(") === 0
    }

    function imageSource(value) {
        if (!isCssUrl(value)) {
            return ""
        }
        var s = value.trim()
        s = s.substring(4, s.length - 1).trim()
        if ((s[0] === "\"" && s[s.length - 1] === "\"") || (s[0] === "'" && s[s.length - 1] === "'")) {
            s = s.substring(1, s.length - 1)
        }
        if (s.length === 0) {
            return ""
        }
        if (s.indexOf("qrc:/") === 0 || s.indexOf("file:/") === 0 || s.indexOf("http://") === 0 || s.indexOf("https://") === 0) {
            return s
        }
        if (s[0] === "/") {
            return "file://" + s
        }
        return s
    }

    function fillModeFor(size) {
        var s = (size || "cover").toLowerCase()
        if (s === "contain") {
            return Image.PreserveAspectFit
        }
        if (s === "stretch" || s === "100% 100%") {
            return Image.Stretch
        }
        return Image.PreserveAspectCrop
    }

    readonly property bool hasCssIdentity: root.cssId.length > 0
        || root.cssPart.length > 0
        || root.cssPrimitive.length > 0
        || (Array.isArray(root.cssClass) ? root.cssClass.length > 0 : String(root.cssClass).length > 0)

    // Image background sits BEHIND the CssRect fill.
    Rectangle {
        anchors.fill: parent
        visible: root.bgIsImage
        color: root.solidColor
    }

    Image {
        anchors.fill: parent
        visible: root.bgIsImage && root.bgImageSource.length > 0
        source: root.bgImageSource
        fillMode: root.fillModeFor(root.style ? root.style["background-size"] : "")
        horizontalAlignment: Image.AlignHCenter
        verticalAlignment: Image.AlignVCenter
        smooth: true
        asynchronous: true
        cache: true
        opacity: root.style && root.style["background-image-opacity"] !== undefined
            ? parseFloat(root.style["background-image-opacity"]) : 1.0
    }

    // The fill / border / gradient / box-shadow / bevel / transition — all CssRect. When
    // an image is present the fill is transparent (CssRect renders url() bgs transparent),
    // so the image above shows through and the border still frames it.
    QmlCss.CssRect {
        anchors.fill: parent
        // Renderer-only child: CssFill owns CSS resolution. Keep this CssRect off the
        // theme registry or loadCss() will overwrite the explicit style binding below.
        cssPrimitive: ""
        style: root.style
        radius: root.radius
        defaultColor: root.bgIsImage ? "transparent" : root.defaultColor
        defaultBorderColor: root.defaultBorderColor
        defaultBorderWidth: root.defaultBorderWidth
    }

    default property alias content: contentHolder.data
    Item {
        id: contentHolder
        anchors.fill: parent
        property var style: root.style
        // Like CssRect: when our own children change, relayout them so we content-size (implicit
        // width/height from the box model) — flex/grid children need this, not just anchored ones.
        onChildrenChanged: root.requestRelayout()
    }

    // Lay out our own children through the C++ engine (CssLayoutEngine), same contract as CssRect —
    // this is what makes a flex/grid CssFill size to its content (e.g. a <button>'s label).
    function requestRelayout() {
        if (typeof cssLayout !== "undefined" && cssLayout && contentHolder)
            cssLayout.requestLayout(root, contentHolder)
    }

    Component.onCompleted: {
        if (cssTheme && root.hasCssIdentity)
            cssTheme.loadCss(root)
        requestRelayout()
        cssLayout.notifyParentLayout(root)
    }

    onCssIdChanged: if (cssTheme && root.hasCssIdentity) cssTheme.loadCss(root)
    onCssClassChanged: if (cssTheme && root.hasCssIdentity) cssTheme.loadCss(root)
    onCssStateChanged: if (cssTheme && root.hasCssIdentity) cssTheme.loadCss(root)
    onWidthChanged: requestRelayout()
    onHeightChanged: requestRelayout()
    onStyleChanged: { requestRelayout(); cssLayout.notifyParentLayout(root) }
    onImplicitWidthChanged: cssLayout.notifyParentLayout(root)
    onImplicitHeightChanged: cssLayout.notifyParentLayout(root)
    onVisibleChanged: cssLayout.notifyParentLayout(root)
}
