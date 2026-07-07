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
        // Basic-style implicit size: Templates leave implicit sizes to the style (us). The height
        // leg cannot use implicitContentHeight — getContentHeight() returns the EXPLICIT
        // contentHeight below once set — so the tabs' own implicit is computed here (count
        // dependency re-evaluates as tabs arrive).
        readonly property real __tabsImplicitHeight: {
            var m = 0;
            for (var i = 0; i < count; i++) {
                var it = itemAt(i);
                if (it) m = Math.max(m, it.implicitHeight);
            }
            return m;
        }
        implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, __tabsImplicitHeight + topPadding + bottomPadding)
        // Tabs stretch to a CSS-sized bar (web `align-items: stretch`): T.TabBar's updateLayout
        // sizes every tab to contentHeight, which otherwise stays at the tallest tab's implicit —
        // follow the available height once the CSS box makes the bar taller than the tabs.
        contentHeight: Math.max(__tabsImplicitHeight, availableHeight)
        background: null

        // Desktop keyboard nav (study §6): a tab BAR is one tab stop (the current tab); Left/Right (or
        // Up/Down) switch the current tab WITH WRAP and move focus to the new tab. The custom ListView
        // contentItem would otherwise eat the arrows (keyNavigationEnabled: false below), and the tab
        // stop is the checked TabButton, so the arrow key propagates up to here.
        function __step(d) {
            if (bar.count < 1) return;
            bar.currentIndex = (bar.currentIndex + d + bar.count) % bar.count;
            var it = bar.itemAt(bar.currentIndex);
            if (it) it.forceActiveFocus(Qt.TabFocusReason);
        }
        Keys.onLeftPressed: bar.__step(-1)
        Keys.onUpPressed: bar.__step(-1)
        Keys.onRightPressed: bar.__step(1)
        Keys.onDownPressed: bar.__step(1)

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
            keyNavigationEnabled: false
        }
    }
}
