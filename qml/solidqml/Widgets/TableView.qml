// TableView — the <TableView> component in module solidqml.Widgets (owner 2026-07-07). A CssFill
// "div" wrapper over a header row + a REAL QtQuick ListView of data rows (row virtualization), with
// DYNAMIC columns — QtQuick's TableView needs a static TableModel/TableModelColumn per column, so a
// header + ListView is what gives author-driven columns AND recycling. `columns` is an array of
// `{ key, label }` (or a plain string = key & label); `data` is an array of row objects; a click on a
// row fires selected(row, index). Cells are `.table-cell`, header cells `.table-th`, rows `.table-row`.
// Clicking a header sorts by that column (toggling ▲/▼); Up/Down navigate rows, Enter re-commits.
// QtQuick aliased QtQ so the file's own TableView type doesn't collide with QtQuick.TableView.
//
// The transpiler emits:  W.TableView { cssClass: […]; __columns: <expr>; __rows: <expr>; onSelected }
import QtQuick as QtQ
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property var __columns: []
    property var __rows: []
    property int currentIndex: -1
    signal selected(var row, int index)

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
        if (root.sortColumn < 0 || root.sortColumn >= root._cols.length) return rows;
        var key = root._cols[root.sortColumn].key;
        var dir = root.sortAscending ? 1 : -1;
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
        if (root.sortColumn === col) root.sortAscending = !root.sortAscending;
        else { root.sortColumn = col; root.sortAscending = true; }
        Qt.callLater(function () { lv.currentIndex = -1; lv._squelch = false; });
    }

    // Fire selected() for the current row (arrow-key navigation, Enter, clicks).
    function _emitCurrent() {
        if (lv.currentIndex >= 0 && root._rows.length > lv.currentIndex)
            root.selected(root._rows[lv.currentIndex], lv.currentIndex);
    }

    cssPrimitive: "div"
    implicitWidth: 360
    implicitHeight: 260

    QtQ.Item {
        anchors.fill: parent
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
                        model: root._cols
                        delegate: Css.CssFill {
                            id: thCell
                            property int colIndex: index
                            cssPrimitive: "div"
                            cssClass: ["table-th"]
                            cssState: (thMa.containsMouse ? ["hover"] : []).concat(root.sortColumn === index ? ["sorted"] : [])
                            width: root._cols.length ? root.width / root._cols.length : 0
                            height: 34
                            QtQ.Item {
                                anchors.fill: parent
                                Css.CssText {
                                    cssPrimitive: ""
                                    cssClass: ["table-th-label"]
                                    x: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    // Trailing ▲/▼ marks the sorted column and direction.
                                    text: "" + modelData.label + (root.sortColumn === thCell.colIndex ? (root.sortAscending ? "  ▲" : "  ▼") : "")
                                }
                                QtQ.MouseArea {
                                    id: thMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root._toggleSort(thCell.colIndex)
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
                model: root._rows
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
                            model: root._cols
                            delegate: Css.CssFill {
                                cssPrimitive: "div"
                                cssClass: ["table-cell"]
                                width: root._cols.length ? rowItem.width / root._cols.length : 0
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
                        onClicked: { lv.forceActiveFocus(); lv.currentIndex = rowItem.rowIndex; root._emitCurrent(); }
                    }
                }
            }
        }
    }
}
