#include "tableview.h"

#include "snippetwidget.h"

namespace {

// Original TableView.qml internals — `root` = the C++ wrapper; the derived sort state and helper
// functions live on the snippet host.
const char *kTableViewBody = R"(import QtQuick as QtQ
import qmlcss 1.0 as Css

QtQ.Item {
    id: host
    anchors.fill: parent

    // Sort state: which column (index into _cols, -1 = unsorted) and direction. A header click
    // toggles direction on the active column or switches to a new one (ascending first).
    property int sortColumn: -1
    property bool sortAscending: true

    // A column entry is { key, label } or a bare string (key == label). Normalized once here.
    readonly property var _cols: (root.__columns || []).map(function (c) {
        return (c && c.key !== undefined) ? c : ({ key: "" + c, label: "" + c });
    })

    // The rows the ListView actually shows: __rows sorted by the active column when one is set.
    // Numbers compare numerically, everything else by localeCompare on the string form (stable-ish).
    readonly property var _rows: {
        var rows = (root.__rows || []).slice();
        if (host.sortColumn < 0 || host.sortColumn >= host._cols.length) return rows;
        var key = host._cols[host.sortColumn].key;
        var dir = host.sortAscending ? 1 : -1;
        rows.sort(function (a, b) {
            var av = a ? a[key] : undefined, bv = b ? b[key] : undefined;
            if (av === bv) return 0;
            if (av === undefined || av === null) return 1;
            if (bv === undefined || bv === null) return -1;
            if (typeof av === "number" && typeof bv === "number") return (av - bv) * dir;
            return ("" + av).localeCompare("" + bv) * dir;
        });
        return rows;
    }

    function _toggleSort(col) {
        // The model swap forces currentIndex=0 synchronously (bindings are eager), so arm the
        // squelch FIRST; then clear the selection after Qt's internal reset instead of silently
        // "selecting" whatever landed on row 0.
        lv._squelch = true;
        if (host.sortColumn === col) host.sortAscending = !host.sortAscending;
        else { host.sortColumn = col; host.sortAscending = true; }
        Qt.callLater(function () { lv.currentIndex = -1; lv._squelch = false; });
    }

    // Fire selected() for the current row (arrow-key navigation, Enter, clicks).
    function _emitCurrent() {
        if (lv.currentIndex >= 0 && host._rows.length > lv.currentIndex)
            root.selected(host._rows[lv.currentIndex], lv.currentIndex);
    }

    QtQ.Column {
        anchors.fill: parent

        // Header row.
        Css.CssFill {
            cssPrimitive: "div"
            cssClass: ["table-header"]
            width: parent.width
            height: 34
            implicitHeight: 34
            QtQ.Row {
                anchors.fill: parent
                QtQ.Repeater {
                    model: host._cols
                    delegate: Css.CssFill {
                        id: thCell
                        property int colIndex: index
                        cssPrimitive: "div"
                        cssClass: ["table-th"]
                        cssState: (thMa.containsMouse ? ["hover"] : []).concat(host.sortColumn === index ? ["sorted"] : [])
                        width: host._cols.length ? root.width / host._cols.length : 0
                        height: 34
                        QtQ.Item {
                            anchors.fill: parent
                            Css.CssText {
                                cssPrimitive: ""
                                cssClass: ["table-th-label"]
                                x: 10
                                anchors.verticalCenter: parent.verticalCenter
                                // Trailing ▲/▼ marks the sorted column and direction.
                                text: "" + modelData.label + (host.sortColumn === thCell.colIndex ? (host.sortAscending ? "  ▲" : "  ▼") : "")
                            }
                            QtQ.MouseArea {
                                id: thMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: host._toggleSort(thCell.colIndex)
                            }
                        }
                    }
                }
            }
        }

        // Data rows — a real virtualized ListView.
        QtQ.ListView {
            id: lv
            width: parent.width
            height: parent.height - 34
            clip: true
            // Keyboard: Up/Down move currentIndex (selection follows focus), Enter re-commits.
            focus: true
            activeFocusOnTab: true
            keyNavigationEnabled: true
            highlightMoveDuration: 0
            boundsBehavior: QtQ.Flickable.StopAtBounds
            model: host._rows
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
                id: rowItem
                property var rowData: modelData
                property int rowIndex: index
                cssPrimitive: "div"
                cssClass: ["table-row"]
                cssState: (rowMa.containsMouse ? ["hover"] : []).concat(index === lv.currentIndex ? ["selected"] : [])
                width: lv.width
                height: 30
                implicitHeight: 30
                QtQ.Row {
                    anchors.fill: parent
                    QtQ.Repeater {
                        model: host._cols
                        delegate: Css.CssFill {
                            cssPrimitive: "div"
                            cssClass: ["table-cell"]
                            width: host._cols.length ? rowItem.width / host._cols.length : 0
                            height: 30
                            QtQ.Item {
                                anchors.fill: parent
                                Css.CssText {
                                    cssPrimitive: ""
                                    cssClass: ["table-cell-label"]
                                    x: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "" + (rowItem.rowData ? rowItem.rowData[modelData.key] : "")
                                }
                            }
                        }
                    }
                }
                QtQ.MouseArea {
                    id: rowMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { lv.forceActiveFocus(); lv.currentIndex = rowItem.rowIndex; host._emitCurrent(); }
                }
            }
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

TableView::TableView(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("div"));
    setImplicitWidth(360);
    setImplicitHeight(260);
}

void TableView::setColumns(const QVariant &v)
{
    if (m_columns == v)
        return;
    m_columns = v;
    emit columnsChanged();
}

void TableView::setRows(const QVariant &v)
{
    if (m_rows == v)
        return;
    m_rows = v;
    emit rowsChanged();
}

void TableView::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void TableView::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    composeInternal(this, content(), QStringLiteral("solidwidgets-tableview"), kTableViewBody);
}

} // namespace SolidWidgets
