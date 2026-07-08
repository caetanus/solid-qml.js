#include "tabbar.h"

#include "snippetwidget.h"

namespace {

// Original TabBar.qml internals — `root` = the C++ wrapper. Templates create NO contentItem
// (verified project-wide pitfall) → a Basic-style horizontal ListView over the container's
// contentModel is supplied here.
const char *kTabBarBody = R"(import QtQuick
import QtQuick.Templates as T

T.TabBar {
    id: bar
    anchors.fill: parent
    onCurrentIndexChanged: root.currentIndex = currentIndex

    Binding on currentIndex {
        value: root.currentIndex
        restoreMode: Binding.RestoreNone
    }

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
)";

} // namespace

namespace SolidWidgets {

TabBar::TabBar(QQuickItem *parent)
    : SlotContainer(parent)
{
    setCssPrimitive(QStringLiteral("tabbar"));
}

void TabBar::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void TabBar::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    adoptControl(composeInternal(this, content(), QStringLiteral("solidwidgets-tabbar"), kTabBarBody));
    if (m_control) {
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
}

} // namespace SolidWidgets
