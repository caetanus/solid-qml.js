// ListView — the <ListView> component in module solidqml.Widgets (owner 2026-07-07: native
// ListView/TableView/ToolBar/real MenuBar). A CssFill "div" wrapper hosts a REAL QtQuick ListView
// (delegate recycling / virtualization, so it scales — unlike our hand-rolled TreeView). Data is a
// plain array of strings or { label, … }; each row is a `.list-item` (hover + selected state), a
// click fires selected(item, index). QtQuick is aliased `QtQ` so the file's own `ListView` type
// doesn't collide with QtQuick.ListView inside it.
//
// The transpiler emits:  W.ListView { cssClass: […]; __listData: <expr>; onSelected: (item,i) => … }
import QtQuick as QtQ
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property var __listData: []
    property int currentIndex: -1
    signal selected(var item, int index)

    cssPrimitive: "div"
    implicitWidth: 260
    implicitHeight: 240

    // Fire selected() for the current row (used by arrow-key navigation, Enter, and clicks).
    function _emitCurrent() {
        if (lv.currentIndex >= 0 && root.__listData && root.__listData.length > lv.currentIndex)
            root.selected(root.__listData[lv.currentIndex], lv.currentIndex);
    }

    QtQ.Item {
        anchors.fill: parent
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
                root._emitCurrent();
            }
            QtQ.Keys.onReturnPressed: root._emitCurrent()
            QtQ.Keys.onEnterPressed: root._emitCurrent()
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
                        onClicked: { lv.forceActiveFocus(); lv.currentIndex = index; root._emitCurrent(); }
                    }
                }
            }
        }
    }
}
