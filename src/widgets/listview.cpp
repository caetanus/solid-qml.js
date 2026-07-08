#include "listview.h"

#include "snippetwidget.h"

namespace {

// Original ListView.qml internals — `root` = the C++ wrapper; the anchored Item hosts the view.
// QtQuick is aliased QtQ in the original to dodge its own type name; kept for a verbatim body.
const char *kListViewBody = R"(import QtQuick as QtQ
import qmlcss 1.0 as Css

QtQ.Item {
    id: host
    anchors.fill: parent

    // Fire selected() for the current row (used by arrow-key navigation, Enter, and clicks).
    function _emitCurrent() {
        if (lv.currentIndex >= 0 && root.__listData && root.__listData.length > lv.currentIndex)
            root.selected(root.__listData[lv.currentIndex], lv.currentIndex);
    }

    QtQ.ListView {
        id: lv
        anchors.fill: parent
        clip: true
        // Keyboard: the inner ListView is the tab stop; Up/Down move currentIndex
        // (keyNavigationEnabled), selection follows focus (QtWidgets-like), Enter re-commits.
        focus: true
        activeFocusOnTab: true
        keyNavigationEnabled: true
        highlightMoveDuration: 0
        boundsBehavior: QtQ.Flickable.StopAtBounds
        model: root.__listData
        // QtQuick forces currentIndex=0 whenever the model loads; without the squelch that
        // would fire selected() at mount, with nobody interacting. Start unselected instead.
        property bool _squelch: true
        QtQ.Component.onCompleted: Qt.callLater(function () { lv.currentIndex = -1; lv._squelch = false; })
        onCurrentIndexChanged: {
            if (_squelch) return;
            root.currentIndex = currentIndex;
            host._emitCurrent();
        }
        QtQ.Keys.onReturnPressed: host._emitCurrent()
        QtQ.Keys.onEnterPressed: host._emitCurrent()
        delegate: Css.CssFill {
            cssPrimitive: "div"
            cssClass: ["list-item"]
            cssState: (rowMa.containsMouse ? ["hover"] : []).concat(index === lv.currentIndex ? ["selected"] : [])
            width: lv.width
            height: 32
            implicitHeight: 32
            QtQ.Item {
                anchors.fill: parent
                Css.CssText {
                    cssPrimitive: ""
                    cssClass: ["list-label"]
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "" + (modelData && modelData.label !== undefined ? modelData.label : modelData)
                }
                QtQ.MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { lv.forceActiveFocus(); lv.currentIndex = index; host._emitCurrent(); }
                }
            }
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

ListView::ListView(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("div"));
    setImplicitWidth(260);
    setImplicitHeight(240);
}

void ListView::setListData(const QVariant &v)
{
    if (m_listData == v)
        return;
    m_listData = v;
    emit listDataChanged();
}

void ListView::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void ListView::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    composeInternal(this, content(), QStringLiteral("solidwidgets-listview"), kListViewBody);
}

} // namespace SolidWidgets
