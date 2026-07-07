// Hand-written QML, imported straight into TSX (`import Badge from "./Badge.qml"`).
// This is the native escape hatch: anything the transpiler doesn't cover, write in QML.
import QtQuick

Rectangle {
    id: root
    property string label: "badge"
    property int count: 0
    signal bumped()

    implicitWidth: row.implicitWidth + 28
    implicitHeight: 40
    radius: 20
    color: tap.pressed ? "#12566c" : "#176b87"

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 10
        Text {
            text: root.label
            color: "white"
            font.pixelSize: 14
            anchors.verticalCenter: parent.verticalCenter
        }
        Rectangle {
            width: 24; height: 24; radius: 12; color: "white"
            anchors.verticalCenter: parent.verticalCenter
            Text {
                anchors.centerIn: parent
                text: root.count
                color: "#176b87"
                font.pixelSize: 12
                font.bold: true
            }
        }
    }
    // A TapHandler, not a MouseArea: a MouseArea grabs wheel events and blocks the enclosing scroll
    // Flickable, so scrolling with the cursor over the badge would go dead (the escape-hatch content
    // sits low in the page, so that made the bottom unreachable). Pointer handlers don't touch wheel.
    TapHandler { id: tap; onTapped: root.bumped() }
    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
