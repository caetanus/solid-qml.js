// RichText — the word-like editor, an opt-in module (solidqml.Widgets.RichText). No extra Qt
// dependency: the formatting/IO engine is the loader-registered RichTextHandler (QTextCursor
// over the TextArea's document; ODF save via Qt's NATIVE QTextDocumentWriter, ODF load via our
// content.xml reader — owner: "ods nativo"). The chrome is all CSS boxes: a toolbar of format
// toggles (cssState "active" mirrors the format at the cursor), open/save through the native
// platform file dialogs, and a T.TextArea in RichText mode inside the flickable body.
//
// The transpiler emits:  WRich.RichText { cssClass: […] }
import QtQuick
import QtQuick.Templates as T
import Qt.labs.platform 1.1 as Platform
import qmlcss 1.0 as Css
import solidqml.native 1.0 as Native

Css.CssFill {
    id: root
    cssPrimitive: "div"
    implicitWidth: 560
    implicitHeight: 380

    component ToolBtn : Css.CssFill {
        id: btn
        property alias label: t.text
        property bool active: false
        signal clicked()
        cssPrimitive: "div"
        cssClass: ["rt-btn"]
        cssState: (ma.containsMouse ? ["hover"] : []).concat(btn.active ? ["active"] : [])
        width: 30
        height: 26
        Item {
            anchors.fill: parent
            Css.CssText { id: t; cssPrimitive: ""; cssClass: ["rt-btn-label"]; anchors.centerIn: parent }
            MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: btn.clicked() }
        }
    }

    Native.RichTextHandler {
        id: fmt
        document: edit.textDocument
        selectionStart: edit.selectionStart
        selectionEnd: edit.selectionEnd
        cursorPosition: edit.cursorPosition
    }

    Platform.FileDialog {
        id: openDialog
        fileMode: Platform.FileDialog.OpenFile
        nameFilters: ["OpenDocument text (*.odt)"]
        onAccepted: fmt.loadOdf(file)
    }
    Platform.FileDialog {
        id: saveDialog
        fileMode: Platform.FileDialog.SaveFile
        defaultSuffix: "odt"
        nameFilters: ["OpenDocument text (*.odt)"]
        onAccepted: fmt.saveOdf(file)
    }

    Item {
        anchors.fill: parent

        Css.CssFill {
            id: toolbar
            cssPrimitive: "div"
            cssClass: ["rt-toolbar"]
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 38
            Row {
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                ToolBtn { label: "B"; active: fmt.bold; onClicked: fmt.toggleBold() }
                ToolBtn { label: "I"; active: fmt.italic; onClicked: fmt.toggleItalic() }
                ToolBtn { label: "U"; active: fmt.underline; onClicked: fmt.toggleUnderline() }
                ToolBtn { label: "S"; active: fmt.strike; onClicked: fmt.toggleStrike() }
                Item { width: 8; height: 1 }
                ToolBtn { label: "H1"; active: fmt.heading === 1; onClicked: fmt.setHeading(fmt.heading === 1 ? 0 : 1) }
                ToolBtn { label: "H2"; active: fmt.heading === 2; onClicked: fmt.setHeading(fmt.heading === 2 ? 0 : 2) }
                ToolBtn { label: "•"; active: fmt.bulletList; onClicked: fmt.toggleBulletList() }
                Item { width: 8; height: 1 }
                ToolBtn { label: "⇱"; onClicked: openDialog.open() }
                ToolBtn { label: "⇲"; onClicked: saveDialog.open() }
            }
        }

        Flickable {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: toolbar.bottom
            anchors.bottom: parent.bottom
            clip: true
            contentWidth: width
            contentHeight: edit.contentHeight + 24
            boundsBehavior: Flickable.StopAtBounds

            T.TextArea {
                id: edit
                width: body.width
                height: Math.max(body.height, contentHeight + 24)
                padding: 12
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                color: root.inheritedColor ? cssTheme.parseColor(root.inheritedColor) : "#1f2328"
                font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
                font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "14px", 14)
                text: "<h1>solid-qml</h1><p>A <b>word-like</b> editor with <i>native</i> " +
                      "<u>OpenDocument</u> round-trip.</p>"
            }
        }
    }
}
