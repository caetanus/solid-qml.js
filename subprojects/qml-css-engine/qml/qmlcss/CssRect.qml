import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import "qrc:/qmlcss"

// CssRect — a CSS-styled rectangle whose fill is rendered by a Shape (a Rectangle is just
// a closed rounded-rect path, so the fill gets the full Shape capability: solid colour,
// linear-gradient, border, box-shadow, inset bevel + the alpha-fix + CSS `transition`).
// It is the primary fill renderer (CssFill is a thin shim that only adds the
// background-image layer a Shape cannot fill).
//
// NOTE the root is an `Item`, not the `Shape` itself: the fill's alpha is applied via the
// Shape's `opacity` (a translucent Shape fill blends wrong — render opaque, alpha via
// opacity), and `opacity` is INHERITED by children. If the Shape were the root, a
// translucent fill would dim/hide the element's CONTENT. So the Shape fill is an inner
// child and content/bevel are its SIBLINGS — the fill alpha never touches them.
//
// It also carries the CssQmlItem signature: an element sets its identity (cssId, optional
// waybar alias cssAlternateId, state cssClass, cssPrimitive, cssPart) and the engine
// PUSHES the resolved rules into `style` via cssTheme.loadCss(this) — registered on
// completion, re-applied on theme reload and cssClass change (the reverse slot). Used as a
// plain renderer (explicit `style`, no cssId), it simply skips registration.
Item {
    id: root

    // --- CssQmlItem signature (read off this object by CssTheme::loadCss) ---
    property string cssId: ""
    property var cssAlternateId: []
    property var cssClass: []
    // Runtime state classes merged with cssClass by the engine, so pseudo-class rules
    // (`.counter:hover`) resolve. Driven by the HoverHandler below.
    property var cssState: hoverHandler.hovered ? ["hover"] : []
    property string cssPrimitive: "rect"
    // A named part of a module (`#tray.item`, `#cpu.graph`): when set, the engine resolves
    // ONLY that part (resolvePart, excluding the bare `#id` base) so a sub-element doesn't
    // inherit the container's own background.
    property string cssPart: ""
    // Engine writes the resolved rules here; everything below keys off it.
    property var style: ({})

    // --- CSS inheritance ----------------------------------------------------------------
    // Inherited text properties (color/font/…) flow down the element tree like in CSS: an
    // element exposes its own value when set, else the value from its CSS-inheriting ancestor
    // (the containing CssRect — children sit in its contentHolder, so that's parent.parent).
    // Raw CSS strings are passed down; CssText resolves them. Reactive: an ancestor change
    // re-propagates through these bindings. So a child label needs no class to pick up the
    // container's colour/font.
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

    // --- CSS layout (the engine's box model: display/flex/gap/align/padding) -------------
    // A CssRect is both a painted box AND a layout container for its content children, just
    // like a CSS box. These read the resolved `style`; relayout() consumes them.
    // The actual box-model layout (flex/grid/calc/…) runs in C++ (CssLayoutEngine, exposed as
    // `cssLayout`); the QML here just TRIGGERS a relayout when geometry/style/children change.
    // `display: none` removes the box from layout (and from view).
    visible: !(style && style["display"] === "none")
    // `visibility: hidden` keeps the box in layout (space preserved, unlike display:none) but paints
    // nothing (opacity 0) and takes no input (enabled:false disables any child MouseArea). A plain
    // `opacity` from CSS applies when not hidden.
    readonly property bool _cssHidden: style && style["visibility"] === "hidden"
    opacity: root._cssHidden ? 0
        : (style && style["opacity"] !== undefined ? Number(style["opacity"]) : 1)
    enabled: !root._cssHidden
    // `z-index` → QML z stacking order.
    z: (style && style["z-index"] !== undefined) ? Number(style["z-index"]) : 0
    // `overflow: hidden` / `overflow: clip` → clip children to item bounds.
    clip: !!(style && (style["overflow"] === "hidden" || style["overflow"] === "clip"))

    // --- CSS transform + animation ------------------------------------------------------
    // Static `transform` (rotate/scale/translate) and `animation: <name> ...` driven by the
    // matching `@keyframes`. The animation interpolates the keyframes' transform across a
    // looping tick; while it runs it owns rotation/scale/translate, else the static
    // transform does. Rotation/scale pivot on the element centre (CSS default).
    transformOrigin: Item.Center
    readonly property var _staticTransform: (cssTheme && cssTheme.loaded && style && style["transform"])
        ? cssTheme.parseTransform(style["transform"]) : ({})
    readonly property var _animation: (cssTheme && cssTheme.loaded && style && style["animation"])
        ? cssTheme.parseAnimation(style["animation"]) : ({})
    readonly property var _animFrames: (cssTheme && cssTheme.loaded && _animation.name)
        ? cssTheme.keyframes(_animation.name) : []
    readonly property var _animStops: (typeof cssLayout !== "undefined" && cssLayout) ? cssLayout.buildAnimStops(_animFrames) : []
    readonly property bool _animActive: _animStops.length >= 2

    property real _animRotate: 0
    property real _animScale: 1
    property real _animTx: 0
    property real _animTy: 0
    property real animTick: 0

    rotation: root._animActive ? root._animRotate
        : (root._staticTransform.rotate !== undefined ? root._staticTransform.rotate : 0)
    scale: root._animActive ? root._animScale
        : (root._staticTransform.scale !== undefined ? root._staticTransform.scale : 1)
    transform: Translate {
        x: root._animActive ? root._animTx
            : (root._staticTransform.translateX !== undefined ? root._staticTransform.translateX : 0)
        y: root._animActive ? root._animTy
            : (root._staticTransform.translateY !== undefined ? root._staticTransform.translateY : 0)
    }

    NumberAnimation on animTick {
        from: 0.0
        to: 1.0
        duration: Math.max(1, (root._animation.duration !== undefined ? root._animation.duration : 1000))
        loops: (root._animation.iterations === undefined || root._animation.iterations < 0)
            ? Animation.Infinite : Math.max(1, root._animation.iterations)
        easing.type: root._animation.easing !== undefined ? root._animation.easing : Easing.Linear
        running: root._animActive
    }
    onAnimTickChanged: if (typeof cssLayout !== "undefined" && cssLayout) cssLayout.applyAnim(root, root._animStops, root.animTick)

    property real radius: 0
    property color defaultColor: "transparent"
    property color defaultBorderColor: "transparent"
    property real defaultBorderWidth: 0

    // Standard CSS `transition` (shorthand) → synced colour+opacity fade. The opaque fill
    // COLOUR and the fill Shape's OPACITY animate with the same duration/easing.
    readonly property var _transition: (cssTheme && cssTheme.loaded && root.style && root.style["transition"])
        ? cssTheme.parseTransition(root.style["transition"]) : ({})
    readonly property int transitionMs: root._transition.duration !== undefined ? root._transition.duration : 0
    readonly property int transitionEasingType: root._transition.easing !== undefined ? root._transition.easing : Easing.InOutQuad

    readonly property string backgroundValue: (style && style["background"]) ? style["background"] : ""
    // A url() background is an image (handled by the CssFill shim, not by a Shape fill);
    // exclude it here so it isn't parsed as a colour.
    readonly property string bgValue: root.isCssUrl(backgroundValue) ? "" : backgroundValue
    readonly property bool bgIsGradient: bgValue.indexOf("gradient") >= 0
    // CSS `background` parsed into stacked layers (index 0 = topmost).
    readonly property var bgLayers: (cssTheme && cssTheme.loaded && bgIsGradient)
        ? cssTheme.parseGradientLayers(bgValue) : []
    // Fast path: a lone linear-gradient is drawn by the built-in fill Shape below. Anything
    // richer (a radial gradient, or several stacked layers) is drawn by the CssFillLayer
    // stack — so the single-gradient code stays untouched for its common case.
    readonly property bool _singleLinear: bgLayers.length === 1 && bgLayers[0].type === "linear"
    readonly property var gradient: root._singleLinear ? bgLayers[0] : ({})
    readonly property bool hasGradient: gradient && gradient.stops !== undefined
    readonly property bool useLayeredFill: bgLayers.length >= 1 && !root._singleLinear
    // CSS paints the FIRST listed layer on top, so reverse for bottom-to-top stacking.
    readonly property var orderedLayers: {
        if (!root.useLayeredFill) return []
        var out = []
        for (var i = bgLayers.length - 1; i >= 0; --i) out.push(bgLayers[i])
        return out
    }
    // Peak alpha across the gradient stops — the gradient is drawn with OPAQUE stops and its
    // alpha applied via the fill Shape's `opacity` (translucent stops blend wrong).
    readonly property real gradientPeakAlpha: {
        if (!hasGradient || !gradient.stops)
            return 1.0
        var m = 0.0
        for (var i = 0; i < gradient.stops.length; ++i)
            m = Math.max(m, gradient.stops[i].color.a)
        return m
    }

    readonly property color solidColor: (style && style["background-color"])
        ? cssTheme.parseColor(style["background-color"])
        : ((bgValue && !bgIsGradient) ? cssTheme.parseColor(bgValue) : root.defaultColor)

    // `border` shorthand (`1px solid #...`), used when the explicit longhands are absent.
    readonly property var _borderShorthand: (cssTheme && cssTheme.loaded && style && style["border"])
        ? cssTheme.parseBorder(style["border"]) : ({})
    readonly property color borderColor: (style && style["border-color"])
        ? cssTheme.parseColor(style["border-color"])
        : (root._borderShorthand.color !== undefined ? root._borderShorthand.color : root.defaultBorderColor)
    readonly property real borderWidth: (style && style["border-width"])
        ? parseFloat(style["border-width"])
        : (root._borderShorthand.width !== undefined ? root._borderShorthand.width : root.defaultBorderWidth)
    readonly property bool hasSideBorder: cssTheme.hasSideBorder(style || ({}))
    readonly property var topBorder: cssTheme.borderSide(style || ({}), "top", 0, root.borderColor)
    readonly property var rightBorder: cssTheme.borderSide(style || ({}), "right", 0, root.borderColor)
    readonly property var bottomBorder: cssTheme.borderSide(style || ({}), "bottom", 0, root.borderColor)
    readonly property var leftBorder: cssTheme.borderSide(style || ({}), "left", 0, root.borderColor)

    readonly property var shadowList: (cssTheme && cssTheme.loaded && style && style["box-shadow"])
        ? cssTheme.parseBoxShadowList(style["box-shadow"]) : []
    // Drop shadow uses the first OUTSET shadow (inset shadows never cast a drop shadow).
    readonly property var outsetShadow: {
        for (var i = 0; i < root.shadowList.length; ++i)
            if (root.shadowList[i] && root.shadowList[i].inset !== true)
                return root.shadowList[i]
        return ({})
    }
    readonly property bool hasOutsetShadow: root.outsetShadow.color !== undefined
    // Inset shadows. TWO of them (a dark+light pair) = a recessed bevel (e.g. bliss-xp's
    // sunken panels). A SINGLE one = a directional inner edge — waybar's idiom for the
    // active-workspace underline (`box-shadow: inset 0 -3px #fff`), NOT a 4-sided bevel.
    readonly property var insetShadows: root.insetShadowsOf(root.shadowList)
    readonly property bool insetBevel: root.insetShadows.length >= 2
    readonly property var insetEdgeShadow: root.insetShadows.length === 1 ? root.insetShadows[0] : null
    readonly property bool insetEdge: root.insetEdgeShadow !== null
    readonly property color insetDarkColor: root.insetShadows.length > 0
        ? root.insetShadows[0].color : "transparent"
    readonly property color insetLightColor: root.insetShadows.length > 1
        ? root.insetShadows[1].color : Qt.rgba(1, 1, 1, 0.30)
    readonly property var borderRadii: root.parseBorderRadius(style && style["border-radius"] ? style["border-radius"] : "")

    // CSS gradient line: angle 0deg points up, increasing clockwise.
    readonly property real angleRad: ((hasGradient ? gradient.angle : 180) * Math.PI) / 180
    readonly property real dirX: Math.sin(angleRad)
    readonly property real dirY: -Math.cos(angleRad)
    readonly property real lineLen: Math.abs(width * dirX) + Math.abs(height * dirY)

    function insetShadowsOf(list) {
        var out = []
        for (var i = 0; i < list.length; ++i) {
            if (list[i] && list[i].inset === true)
                out.push(list[i])
        }
        return out
    }

    function isCssUrl(value) {
        return value && value.trim().toLowerCase().indexOf("url(") === 0
    }

    function parseCssLength(value, fallback) {
        if (value && String(value).trim().indexOf("%") > 0) {
            var pct = parseFloat(value)
            return isNaN(pct) ? fallback : Math.min(root.width, root.height) * pct / 100.0
        }
        var n = parseFloat(value)
        return isNaN(n) ? fallback : n
    }

    function parseBorderRadius(value) {
        if (!value || String(value).trim().length === 0)
            return [root.radius, root.radius, root.radius, root.radius]

        // Elliptical radii ("a / b") accepted syntactically; the horizontal side is used
        // because ShapePath corners are circular.
        var s = String(value).split("/")[0].trim()
        var parts = s.split(/\s+/)
        var values = []
        for (var i = 0; i < parts.length && i < 4; ++i)
            values.push(root.parseCssLength(parts[i], root.radius))
        if (values.length === 0)
            return [root.radius, root.radius, root.radius, root.radius]
        if (values.length === 1)
            return [values[0], values[0], values[0], values[0]]
        if (values.length === 2)
            return [values[0], values[1], values[0], values[1]]
        if (values.length === 3)
            return [values[0], values[1], values[2], values[1]]
        return [values[0], values[1], values[2], values[3]]
    }

    function clampedRadius(index) {
        return Math.max(0, Math.min(root.borderRadii[index], root.width / 2, root.height / 2))
    }

    readonly property bool partiallyRounded: root.borderRadii[0] > 0 || root.borderRadii[1] > 0
        || root.borderRadii[2] > 0 || root.borderRadii[3] > 0

    // A fully-square edge of a partially-rounded box butts against a neighbour — skip its
    // inset bevel so the two read as one recessed unit, not two cells split by a line.
    function bevelEdge(cornerA, cornerB) {
        return !(root.partiallyRounded && root.borderRadii[cornerA] <= 0 && root.borderRadii[cornerB] <= 0)
    }

    function hasCssIdentity() {
        return root.cssId.length > 0
            || root.cssPart.length > 0
            || root.cssPrimitive.length > 0 // a type selector (`button {}`) keys off the primitive
            || (Array.isArray(root.cssClass) ? root.cssClass.length > 0 : String(root.cssClass).length > 0)
    }

    // Single outset shadow, matching the old behaviour, but drawn by a separate source behind
    // the real fill so MultiEffect cannot make the element background disappear.
    Item {
        anchors.fill: parent
        visible: root.hasOutsetShadow

        Shape {
            id: shadowSource
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: root.hasGradient ? root.gradientPeakAlpha : root.solidColor.a

            ShapePath {
                strokeColor: "transparent"
                strokeWidth: 0
                fillColor: root.hasGradient
                    ? "#ffffff"
                    : Qt.rgba(root.solidColor.r, root.solidColor.g, root.solidColor.b, 1.0)
                startX: root.clampedRadius(0)
                startY: 0
                PathLine { x: Math.max(root.clampedRadius(0), root.width - root.clampedRadius(1)); y: 0 }
                PathArc { x: root.width; y: root.clampedRadius(1); radiusX: root.clampedRadius(1); radiusY: root.clampedRadius(1); direction: PathArc.Clockwise }
                PathLine { x: root.width; y: Math.max(root.clampedRadius(1), root.height - root.clampedRadius(2)) }
                PathArc { x: root.width - root.clampedRadius(2); y: root.height; radiusX: root.clampedRadius(2); radiusY: root.clampedRadius(2); direction: PathArc.Clockwise }
                PathLine { x: root.clampedRadius(3); y: root.height }
                PathArc { x: 0; y: root.height - root.clampedRadius(3); radiusX: root.clampedRadius(3); radiusY: root.clampedRadius(3); direction: PathArc.Clockwise }
                PathLine { x: 0; y: root.clampedRadius(0) }
                PathArc { x: root.clampedRadius(0); y: 0; radiusX: root.clampedRadius(0); radiusY: root.clampedRadius(0); direction: PathArc.Clockwise }
            }
        }

        MultiEffect {
            anchors.fill: shadowSource
            source: shadowSource
            shadowEnabled: true
            shadowColor: root.hasOutsetShadow
                ? Qt.rgba(root.outsetShadow.color.r, root.outsetShadow.color.g, root.outsetShadow.color.b, 1.0)
                : "transparent"
            shadowOpacity: root.hasOutsetShadow ? root.outsetShadow.color.a : 0
            shadowHorizontalOffset: root.hasOutsetShadow ? root.outsetShadow.x : 0
            shadowVerticalOffset: root.hasOutsetShadow ? root.outsetShadow.y : 0
            shadowBlur: root.hasOutsetShadow ? Math.min(1, root.outsetShadow.blur / 32) : 0
            autoPaddingEnabled: true
        }
    }

    // The fill. Its alpha rides on the Shape's `opacity` (a translucent Shape fill blends
    // wrong); because this Shape is NOT the root, that opacity dims only the fill, never the
    // content/bevel siblings below.
    Shape {
        id: fill
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        opacity: root.hasGradient ? root.gradientPeakAlpha : root.solidColor.a

        Behavior on opacity {
            enabled: root.transitionMs > 0
            NumberAnimation {
                duration: root.transitionMs
                easing.type: root.transitionEasingType
            }
        }

        ShapePath {
            // Border is drawn by its own Shape below (independent opacity); the fill paints
            // no stroke.
            strokeColor: "transparent"
            strokeWidth: 0
            fillColor: root.hasGradient
                ? "transparent"
                : Qt.rgba(root.solidColor.r, root.solidColor.g, root.solidColor.b, 1.0)
            fillGradient: root.hasGradient ? linearGradient : null

            Behavior on fillColor {
                enabled: root.transitionMs > 0
                ColorAnimation {
                    duration: root.transitionMs
                    easing.type: root.transitionEasingType
                }
            }

            // Corners are true quarter-ellipse arcs (PathArc), so `border-radius: 50%`
            // renders a real circle/ellipse — a PathQuad is a parabola and only fakes it
            // (the old "squircle"). Square corners (radius 0) collapse the arc to a point.
            startX: root.clampedRadius(0)
            startY: 0

            PathLine { x: Math.max(root.clampedRadius(0), root.width - root.clampedRadius(1)); y: 0 }
            PathArc {
                x: root.width; y: root.clampedRadius(1)
                radiusX: root.clampedRadius(1); radiusY: root.clampedRadius(1)
                direction: PathArc.Clockwise
            }
            PathLine { x: root.width; y: Math.max(root.clampedRadius(1), root.height - root.clampedRadius(2)) }
            PathArc {
                x: root.width - root.clampedRadius(2); y: root.height
                radiusX: root.clampedRadius(2); radiusY: root.clampedRadius(2)
                direction: PathArc.Clockwise
            }
            PathLine { x: root.clampedRadius(3); y: root.height }
            PathArc {
                x: 0; y: root.height - root.clampedRadius(3)
                radiusX: root.clampedRadius(3); radiusY: root.clampedRadius(3)
                direction: PathArc.Clockwise
            }
            PathLine { x: 0; y: root.clampedRadius(0) }
            PathArc {
                x: root.clampedRadius(0); y: 0
                radiusX: root.clampedRadius(0); radiusY: root.clampedRadius(0)
                direction: PathArc.Clockwise
            }
        }

        LinearGradient {
            id: linearGradient
            x1: root.width / 2 - root.dirX * root.lineLen / 2
            y1: root.height / 2 - root.dirY * root.lineLen / 2
            x2: root.width / 2 + root.dirX * root.lineLen / 2
            y2: root.height / 2 + root.dirY * root.lineLen / 2
        }
    }

    // Stacked CSS background layers (radial gradients / multi-layer backgrounds) the single
    // fill above can't express. Painted over the (transparent) base fill, bottom-to-top, and
    // below the bevel/content. Same rounded-rect geometry as the fill.
    Repeater {
        model: root.orderedLayers
        CssFillLayer {
            spec: modelData
            radii: [root.clampedRadius(0), root.clampedRadius(1), root.clampedRadius(2), root.clampedRadius(3)]
        }
    }

    // Border as its OWN Shape so its alpha (carried on opacity) is independent of the fill's.
    // The fill Shape's opacity = fill alpha; an element with a transparent background but a
    // visible border (the orbit ring: `border: 2px solid rgba(255,255,255,.16)` over no fill)
    // would otherwise vanish, since that opacity (0) would gate the stroke too.
    Shape {
        anchors.fill: parent
        visible: !root.hasSideBorder && root.borderWidth > 0 && root.borderColor.a > 0
        preferredRendererType: Shape.CurveRenderer
        opacity: root.borderColor.a
        ShapePath {
            strokeColor: Qt.rgba(root.borderColor.r, root.borderColor.g, root.borderColor.b, 1.0)
            strokeWidth: root.borderWidth
            fillColor: "transparent"
            startX: root.clampedRadius(0); startY: 0
            PathLine { x: Math.max(root.clampedRadius(0), root.width - root.clampedRadius(1)); y: 0 }
            PathArc { x: root.width; y: root.clampedRadius(1); radiusX: root.clampedRadius(1); radiusY: root.clampedRadius(1); direction: PathArc.Clockwise }
            PathLine { x: root.width; y: Math.max(root.clampedRadius(1), root.height - root.clampedRadius(2)) }
            PathArc { x: root.width - root.clampedRadius(2); y: root.height; radiusX: root.clampedRadius(2); radiusY: root.clampedRadius(2); direction: PathArc.Clockwise }
            PathLine { x: root.clampedRadius(3); y: root.height }
            PathArc { x: 0; y: root.height - root.clampedRadius(3); radiusX: root.clampedRadius(3); radiusY: root.clampedRadius(3); direction: PathArc.Clockwise }
            PathLine { x: 0; y: root.clampedRadius(0) }
            PathArc { x: root.clampedRadius(0); y: 0; radiusX: root.clampedRadius(0); radiusY: root.clampedRadius(0); direction: PathArc.Clockwise }
        }
    }

    // Per-side CSS borders (`border-top`, etc.) are Qt Quick rectangles. This keeps simple
    // separators cheap and avoids forcing a full stroked rounded path when only one edge is
    // requested (TodoMVC rows/footers, <hr>, list dividers).
    Item {
        anchors.fill: parent
        visible: root.hasSideBorder

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: root.topBorder.visible ? Math.max(1, root.topBorder.width) : 0
            color: root.topBorder.color
            visible: root.topBorder.visible
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: root.bottomBorder.visible ? Math.max(1, root.bottomBorder.width) : 0
            color: root.bottomBorder.color
            visible: root.bottomBorder.visible
        }
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: root.leftBorder.visible ? Math.max(1, root.leftBorder.width) : 0
            color: root.leftBorder.color
            visible: root.leftBorder.visible
        }
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: root.rightBorder.visible ? Math.max(1, root.rightBorder.width) : 0
            color: root.rightBorder.color
            visible: root.rightBorder.visible
        }
    }

    // Sunken bevel for `box-shadow: inset`. Sibling of the fill (so the fill's alpha does
    // not dim it). Thin edge lines inset past the rounded corners.
    Item {
        anchors.fill: parent
        visible: root.insetBevel

        Rectangle { // top — shadow
            visible: root.bevelEdge(0, 1)
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.leftMargin: root.clampedRadius(0); anchors.rightMargin: root.clampedRadius(1)
            height: 1; color: root.insetDarkColor
        }
        Rectangle { // left — shadow
            visible: root.bevelEdge(0, 3)
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: root.clampedRadius(0); anchors.bottomMargin: root.clampedRadius(3)
            width: 1; color: root.insetDarkColor
        }
        Rectangle { // bottom — highlight
            visible: root.bevelEdge(3, 2)
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: root.clampedRadius(3); anchors.rightMargin: root.clampedRadius(2)
            height: 1; color: root.insetLightColor
        }
        Rectangle { // right — highlight
            visible: root.bevelEdge(1, 2)
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: root.clampedRadius(1); anchors.bottomMargin: root.clampedRadius(2)
            width: 1; color: root.insetLightColor
        }
    }

    // A SINGLE inset box-shadow is a directional inner edge (waybar's active-workspace
    // underline `box-shadow: inset 0 -3px #fff`), not a 4-sided bevel: one band on the side
    // the offset points away from, thick = |offset|.
    Item {
        anchors.fill: parent
        visible: root.insetEdge
        Rectangle { // top/bottom band (vertical offset dominates)
            visible: root.insetEdge && root.insetEdgeShadow.y !== 0
                && Math.abs(root.insetEdgeShadow.y) >= Math.abs(root.insetEdgeShadow.x)
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: root.insetEdge && root.insetEdgeShadow.y > 0 ? parent.top : undefined
            anchors.bottom: root.insetEdge && root.insetEdgeShadow.y < 0 ? parent.bottom : undefined
            height: root.insetEdge ? Math.max(1, Math.abs(root.insetEdgeShadow.y)) : 0
            color: root.insetEdge ? root.insetEdgeShadow.color : "transparent"
        }
        Rectangle { // left/right band (horizontal offset dominates)
            visible: root.insetEdge && root.insetEdgeShadow.x !== 0
                && Math.abs(root.insetEdgeShadow.x) > Math.abs(root.insetEdgeShadow.y)
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: root.insetEdge && root.insetEdgeShadow.x > 0 ? parent.left : undefined
            anchors.right: root.insetEdge && root.insetEdgeShadow.x < 0 ? parent.right : undefined
            width: root.insetEdge ? Math.max(1, Math.abs(root.insetEdgeShadow.x)) : 0
            color: root.insetEdge ? root.insetEdgeShadow.color : "transparent"
        }
    }

    // Applet content sits above the fill — a SIBLING of the fill Shape, so the fill's
    // alpha-opacity never dims it.
    // Tracks pointer hover for the `:hover` state class (see cssState). Cheap and harmless
    // for elements without :hover rules — they just re-resolve to the same style.
    HoverHandler { id: hoverHandler }

    default property alias content: contentHolder.data
    Item {
        id: contentHolder
        anchors.fill: parent
        onChildrenChanged: root.requestRelayout()
    }

    Component { id: stopComponent; GradientStop {} }

    function rebuildStops() {
        var built = []
        // Read `gradient` DIRECTLY — NOT via the derived `hasGradient` binding. When
        // `gradient` changes, onGradientChanged runs this BEFORE the separate `hasGradient`
        // binding recomputes, so `hasGradient` is stale (false) here even though `gradient`
        // already has stops — gating on it builds 0 stops and the Shape paints black.
        var g = root.gradient
        var list = (g && g.stops !== undefined) ? g.stops : []
        for (var i = 0; i < list.length; ++i) {
            // Opaque stop; the gradient's alpha is the fill Shape's `opacity` (gradientPeakAlpha).
            var c = list[i].color
            built.push(stopComponent.createObject(linearGradient,
                { position: list[i].position, color: Qt.rgba(c.r, c.g, c.b, 1.0) }))
        }
        linearGradient.stops = built
    }

    onGradientChanged: rebuildStops()

    Component.onCompleted: {
        rebuildStops()
        // Register with the engine only when this is an identified element (not a plain
        // renderer the CssFill shim drives with an explicit style).
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
        root.requestRelayout()
    }

    // A dynamic css identity must
    // re-register so the engine resolves and pushes the new rules.
    onCssIdChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }
    onCssClassChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }
    // A reactive cssPrimitive (e.g. <Dynamic component={tag()}>) must re-resolve its type rules.
    onCssPrimitiveChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }

    // === CSS layout (C++ engine) ========================================================
    // The actual box model — flex/grid/calc, gap, justify/align, padding/margin, sizing and
    // absolute positioning — runs in C++ (CssLayoutEngine, exposed as `cssLayout`). The QML
    // here only TRIGGERS a relayout on geometry/style/children changes and reports its own
    // size changes up so the container reflows.
    onWidthChanged: root.requestRelayout()
    onHeightChanged: root.requestRelayout()
    onStyleChanged: { root.requestRelayout(); root._notifyParent() }
    onImplicitWidthChanged: root._notifyParent()
    onImplicitHeightChanged: root._notifyParent()
    onVisibleChanged: root._notifyParent()

    function requestRelayout() {
        if (typeof cssLayout !== "undefined" && cssLayout && contentHolder)
            cssLayout.requestLayout(root, contentHolder)
    }

    function _notifyParent() {
        var holder = root.parent
        if (holder && holder.parent && holder.parent.requestRelayout)
            holder.parent.requestRelayout()
    }

}
