// SwipeView — the <SwipeView> component in module solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). A T.SwipeView with CSS support: wrapper CssFill (cssPrimitive "swipeview") +
// the control. Templates create NO contentItem (verified pitfall) → a Basic-style ListView over the
// contentModel; `clip` keeps the neighbouring pages inside the box while swiping.
//
// `currentIndex` is a two-way alias so the emit's controlled RestoreNone Binding (target __swipeN,
// property "currentIndex") writes through it and the onCurrentIndexChanged handler resolves across the
// boundary. The author's pages route into the control's contentData via the default `pages` alias —
// the Container adopts them and resizes each to the view.
//
// The transpiler emits:  W.SwipeView { id: __swipeN; cssClass: […]; onCurrentIndexChanged; <pages>;
//                                      Binding on currentIndex }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias currentIndex: sw.currentIndex
    default property alias pages: sw.contentData

    cssPrimitive: "swipeview"
    implicitWidth: sw.implicitWidth
    implicitHeight: sw.implicitHeight

    T.SwipeView {
        id: sw
        anchors.fill: parent
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        background: null
        contentItem: ListView {
            model: sw.contentModel
            interactive: sw.interactive
            currentIndex: sw.currentIndex
            spacing: sw.spacing
            orientation: sw.orientation
            snapMode: ListView.SnapOneItem
            boundsBehavior: Flickable.StopAtBounds
            highlightRangeMode: ListView.StrictlyEnforceRange
            preferredHighlightBegin: 0
            preferredHighlightEnd: 0
            highlightMoveDuration: 250
            clip: true
        }
    }
}
