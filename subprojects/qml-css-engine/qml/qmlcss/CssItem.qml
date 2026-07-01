import QtQuick

// Drop CssEnable into ANY builtin to make it CSS-styled — the styling analogue of
// MouseArea: a non-visual child that augments its parent. It carries the CssQmlItem
// signature, registers ITSELF with the engine (cssTheme.loadCss), and applies the
// resolved rules to its `parent`. The engine re-pushes on theme reload and cssClass
// change (the reverse slot), so the imperative apply never goes stale.
//
//   Text { text: "hi"; CssEnable { cssId: "foo" } }
//
// Boundary: CssEnable only sets what's imperatively settable on the parent (colour,
// font, radius, border, solid background). Declarative-only things — a Text's
// `text-shadow` layer, or a real gradient / inset-bevel fill — need a dedicated type:
// CssRect (a Shape) for rich fills, CssText for a shadowed label. Whatever prop CssEnable
// drives, do NOT also bind it on the parent — CssEnable owns it.
Item {
    id: root
    visible: false
    width: 0
    height: 0

    // --- CssQmlItem signature (read off this object by CssTheme::loadCss) ---
    property string cssId: ""
    property var cssAlternateId: []
    property var cssClass: []
    // What the parent is, so the right CSS→QML mapping is used. Inferred from the parent
    // when left empty.
    property string cssPrimitive: ""
    property string cssPart: ""
    // Engine writes the resolved rules here; _apply() pushes them onto the parent.
    property var style: ({})

    readonly property string _primitive: cssPrimitive.length > 0 ? cssPrimitive : root._inferPrimitive()

    function _inferPrimitive() {
        var p = root.parent
        if (!p)
            return "item"
        if (p.text !== undefined && p.font !== undefined)
            return "text"
        if (p.radius !== undefined && p.color !== undefined)
            return "rect"
        return "item"
    }

    onStyleChanged: root._apply()

    function hasCssIdentity() {
        return root.cssId.length > 0
            || root.cssPart.length > 0
            || (Array.isArray(root.cssClass) ? root.cssClass.length > 0 : String(root.cssClass).length > 0)
    }

    function _apply() {
        var p = root.parent
        if (!p || !cssTheme)
            return
        var s = root.style || ({})
        var prim = root._primitive

        if (prim === "text") {
            if (s["color"] !== undefined && p.color !== undefined)
                p.color = cssTheme.parseColor(s["color"])
            // `font` is a value type: mutate a copy and assign it back, or the write is lost.
            if (p.font !== undefined && (s["font-family"] !== undefined || s["font-size"] !== undefined)) {
                var f = p.font
                if (s["font-family"] !== undefined)
                    f.family = s["font-family"]
                if (s["font-size"] !== undefined)
                    f.pointSize = cssTheme.parseFontSize(s["font-size"], f.pointSize)
                p.font = f
            }
        } else if (prim === "rect") {
            if (s["background-color"] !== undefined && p.color !== undefined)
                p.color = cssTheme.parseColor(s["background-color"])
            if (s["border-radius"] !== undefined && p.radius !== undefined)
                p.radius = cssTheme.parseLength(s["border-radius"], p.radius)
            // Rectangle.border is a QObject (QQuickPen), so its sub-props write directly.
            if (p.border !== undefined) {
                if (s["border-color"] !== undefined)
                    p.border.color = cssTheme.parseColor(s["border-color"])
                if (s["border-width"] !== undefined)
                    p.border.width = cssTheme.parseLength(s["border-width"], p.border.width)
            }
        }
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
}
