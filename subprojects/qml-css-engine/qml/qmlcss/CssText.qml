import QtQuick
import "qrc:/qmlcss" as QmlCss

// CssText IS a Text — a CSS-styled label. It carries the CssQmlItem signature; the engine
// pushes the resolved rules into `style` (the reverse slot — reload- and cssClass-aware),
// and the label binds its colour / font / text-shadow to that pushed `style`. Used without
// a cssId it is just a plain Text. Override `color`/`font` to opt a specific label out.
Text {
    id: root

    // --- CssQmlItem signature (read off this object by CssTheme::loadCss) ---
    property string cssId: ""
    property var cssAlternateId: []
    property var cssClass: []
    property string cssPrimitive: "text"
    property string cssPart: ""
    // Engine writes the resolved rules here; the bindings below key off it.
    property var style: ({})

    // Fallbacks when the theme leaves the label unstyled.
    property color defaultColor: "black"
    property string defaultFontFamily: "Sans Serif"
    property real defaultFontSize: 11

    // --- CSS inheritance (mirrors CssRect) ----------------------------------------------
    // Inheritable text props come from this label's own style, else from the CSS-inheriting
    // ancestor (the containing CssRect, at parent.parent). So a bare label with no class
    // picks up the container's colour/font — no need to repeat the class on the label.
    // A directly-styled parent wins (a <button> is a CssFill carrying `color`, with the label as a
    // direct child); otherwise the CSS-inheriting container two levels up (a plain label in a CssRect).
    readonly property var _cssParent:
        (parent && parent.inheritedColor !== undefined && String(parent.inheritedColor).length > 0) ? parent
        : (parent && parent.parent && parent.parent.inheritedColor !== undefined) ? parent.parent : null
    readonly property string inheritedColor: (style && style["color"]) ? style["color"] : (_cssParent ? _cssParent.inheritedColor : "")
    readonly property string inheritedFontFamily: (style && style["font-family"]) ? style["font-family"] : (_cssParent ? _cssParent.inheritedFontFamily : "")
    readonly property string inheritedFontSize: (style && style["font-size"]) ? style["font-size"] : (_cssParent ? _cssParent.inheritedFontSize : "")
    readonly property string inheritedFontWeight: (style && style["font-weight"]) ? style["font-weight"] : (_cssParent ? _cssParent.inheritedFontWeight : "")
    readonly property string inheritedLineHeight: (style && style["line-height"]) ? style["line-height"] : (_cssParent ? _cssParent.inheritedLineHeight : "")
    readonly property string inheritedLetterSpacing: (style && style["letter-spacing"]) ? style["letter-spacing"] : (_cssParent ? _cssParent.inheritedLetterSpacing : "")
    readonly property string inheritedTextTransform: (style && style["text-transform"]) ? style["text-transform"] : (_cssParent ? _cssParent.inheritedTextTransform : "")
    readonly property string inheritedTextAlign: (style && style["text-align"]) ? style["text-align"] : (_cssParent ? _cssParent.inheritedTextAlign : "")

    color: root.inheritedColor ? cssTheme.parseColor(root.inheritedColor) : root.defaultColor
    // The leading `cssTheme.fontRevision,` makes this binding depend on it so the family
    // re-resolves once a downloaded @font-face registers (async web fonts arrive after paint).
    font.family: (cssTheme.fontRevision, cssTheme.resolveFontFamily(root.inheritedFontFamily, root.defaultFontFamily))
    // The engine resolves CSS font-size → points (px→pt, em/rem, pt/bare); no unit math here.
    font.pointSize: root.inheritedFontSize
        ? cssTheme.parseFontSize(root.inheritedFontSize, root.defaultFontSize) : root.defaultFontSize
    font.weight: cssTheme.fontWeight(root.inheritedFontWeight)
    font.letterSpacing: root.inheritedLetterSpacing ? cssTheme.parseLength(root.inheritedLetterSpacing, 0) : 0
    lineHeightMode: cssTheme.isProportionalLineHeight(root.inheritedLineHeight) ? Text.ProportionalHeight : Text.FixedHeight
    lineHeight: cssTheme.parseLineHeight(root.inheritedLineHeight, root.font.pixelSize > 0 ? root.font.pixelSize : root.font.pointSize * 96 / 72)
    // CSS text-transform via QFont capitalization (no need to mutate the text string).
    font.capitalization: root._capitalization(root.inheritedTextTransform)
    // Transpiled JSX text is always literal — never HTML to re-parse. The Text default
    // (Text.AutoText) would feed strings like "render as: <h1>" through mightBeRichText() and render
    // the `<h1>` as a heading, mangling the content. PlainText shows every character verbatim.
    textFormat: Text.PlainText
    verticalAlignment: Text.AlignVCenter
    leftPadding: cssTheme.boxSideLength(style || ({}), "padding", 3)
    rightPadding: cssTheme.boxSideLength(style || ({}), "padding", 1)
    topPadding: cssTheme.boxSideLength(style || ({}), "padding", 0)
    bottomPadding: cssTheme.boxSideLength(style || ({}), "padding", 2)

    function _capitalization(v) {
        var s = String(v).trim().toLowerCase()
        if (s === "uppercase") return Font.AllUppercase
        if (s === "lowercase") return Font.AllLowercase
        if (s === "capitalize") return Font.Capitalize
        return Font.MixedCase
    }

    // Layout participant: wrap when a container assigns a width, drop out of layout on
    // `display: none`, and report size changes up so the container re-flows. Geometry (x/y/
    // width) is assigned by the parent CssRect's relayout — this label carries no own size
    // bindings, only its implicit text size.
    // `white-space: nowrap` → no wrapping; all other values keep the default word-wrap.
    wrapMode: (style && style["white-space"] === "nowrap") ? Text.NoWrap : Text.WordWrap
    // `text-overflow: ellipsis` → elide at the right; otherwise no elide.
    elide: (style && style["text-overflow"] === "ellipsis") ? Text.ElideRight : Text.ElideNone
    // `text-decoration` → underline / line-through (strikeout). A single value may contain
    // multiple decorations (e.g. "underline line-through"), so check for presence.
    font.underline: !!(style && style["text-decoration"] && String(style["text-decoration"]).indexOf("underline") >= 0)
    font.strikeout: !!(style && style["text-decoration"] && String(style["text-decoration"]).indexOf("line-through") >= 0)
    // `word-spacing` → font.wordSpacing in px (same helper as letter-spacing).
    font.wordSpacing: (style && style["word-spacing"]) ? cssTheme.parseLength(style["word-spacing"], 0) : 0
    visible: !(style && style["display"] === "none")
    // `visibility: hidden` keeps the label in layout but paints nothing; a plain CSS `opacity` applies otherwise.
    opacity: (style && style["visibility"] === "hidden") ? 0
        : (style && style["opacity"] !== undefined ? Number(style["opacity"]) : 1)
    horizontalAlignment: root.inheritedTextAlign === "center"
        ? Text.AlignHCenter
        : (root.inheritedTextAlign === "right" ? Text.AlignRight : Text.AlignLeft)

    function _notifyParent() {
        var holder = root.parent
        if (holder && holder.parent && holder.parent.requestRelayout)
            holder.parent.requestRelayout()
    }
    onImplicitWidthChanged: root._notifyParent()
    onImplicitHeightChanged: root._notifyParent()
    onStyleChanged: root._notifyParent()
    onVisibleChanged: root._notifyParent()

    // CSS `text-shadow` → drop-shadow layer.
    readonly property var _dropShadow: (cssTheme && cssTheme.loaded && style && style["text-shadow"])
        ? cssTheme.parseBoxShadow(style["text-shadow"]) : ({})
    layer.enabled: root._dropShadow.color !== undefined
    layer.effect: QmlCss.CssDropShadow { shadow: root._dropShadow }

    function hasCssIdentity() {
        return root.cssId.length > 0
            || root.cssPart.length > 0
            || root.cssPrimitive.length > 0 // a type selector (`text {}`) keys off the primitive
            || (Array.isArray(root.cssClass) ? root.cssClass.length > 0 : String(root.cssClass).length > 0)
    }

    Component.onCompleted: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }

    onCssIdChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }

    onCssClassChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }
    // A reactive cssPrimitive (e.g. <Dynamic component={tag()}> over a text tag) must re-resolve.
    onCssPrimitiveChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }
}
