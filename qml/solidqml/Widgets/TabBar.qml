// TabBar — the <TabBar> component in module solidqml.Widgets (owner directive 2026-07-05: one .qml
// per component). A T.TabBar with CSS support: wrapper CssFill (cssPrimitive "tabbar") + the control.
// Templates create NO contentItem (verified project-wide pitfall) → a Basic-style horizontal ListView
// over the container's contentModel is supplied here.
//
// `currentIndex` is a two-way alias to the control so the emit's controlled RestoreNone Binding
// (target __tabbarN, property "currentIndex") writes through it and the onCurrentIndexChanged handler
// (which reads __tabbarN.currentIndex) resolves across the component boundary. The author's
// <TabButton> children route into the control's contentData via the default `tabs` alias.
//
// The transpiler emits:  W.TabBar { id: __tabbarN; cssClass: […]; onCurrentIndexChanged; <TabButtons>;
//                                   Binding on currentIndex }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias currentIndex: bar.currentIndex
    default property alias tabs: bar.contentData

    cssPrimitive: "tabbar"
    implicitWidth: bar.implicitWidth
    implicitHeight: bar.implicitHeight

    T.TabBar {
        id: bar
        anchors.fill: parent
        // Basic-style implicit size: Templates leave implicit sizes to the style (us).
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
        background: null
        // Basic-style contentItem: the ListView hosts the buttons from the contentModel.
        contentItem: ListView {
            model: bar.contentModel
            currentIndex: bar.currentIndex
            spacing: bar.spacing
            orientation: ListView.Horizontal
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.AutoFlickIfNeeded
            snapMode: ListView.SnapToItem
            highlightMoveDuration: 0
        }
    }
}
