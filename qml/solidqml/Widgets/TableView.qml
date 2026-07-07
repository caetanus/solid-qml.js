// TableView — the <TableView> component in module solidqml.Widgets (owner 2026-07-07). A CssFill
// "div" wrapper over a header row + a REAL QtQuick ListView of data rows (row virtualization), with
// DYNAMIC columns — QtQuick's TableView needs a static TableModel/TableModelColumn per column, so a
// header + ListView is what gives author-driven columns AND recycling. `columns` is an array of
// `{ key, label }` (or a plain string = key & label); `data` is an array of row objects; a click on a
// row fires selected(row, index). Cells are `.table-cell`, header cells `.table-th`, rows `.table-row`.
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

    // A column entry is { key, label } or a bare string (key == label). Normalized once here.
    readonly property var _cols: (root.__columns || []).map(function (c) {
        return (c && c.key !== undefined) ? c : ({ key: "" + c, label: "" + c });
    })

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
                            cssPrimitive: "div"
                            cssClass: ["table-th"]
                            width: root._cols.length ? root.width / root._cols.length : 0
                            height: 34
                            QtQ.Item {
                                anchors.fill: parent
                                Css.CssText {
                                    cssPrimitive: ""
                                    cssClass: ["table-th-label"]
                                    x: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "" + modelData.label
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
                boundsBehavior: QtQ.Flickable.StopAtBounds
                model: root.__rows
                delegate: Css.CssFill {
                    id: rowItem
                    property var rowData: modelData
                    property int rowIndex: index
                    cssPrimitive: "div"
                    cssClass: ["table-row"]
                    cssState: (rowMa.containsMouse ? ["hover"] : []).concat(index === root.currentIndex ? ["selected"] : [])
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
                        onClicked: {
                            root.currentIndex = rowItem.rowIndex;
                            root.selected(rowItem.rowData, rowItem.rowIndex);
                        }
                    }
                }
            }
        }
    }
}
