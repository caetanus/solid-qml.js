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
    color: mouse.pressed ? "#12566c" : "#176b87"

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
    MouseArea { id: mouse; anchors.fill: parent; onClicked: root.bumped() }
}
