// CodeEditor — an opt-in module (solidqml.Widgets.Code). The highlighting engine is the
// loader-registered CodeHighlighter: OUR OWN QSyntaxHighlighter rules (owner directive: no KDE
// dependency) — JavaScript/TypeScript, QML, CSS and JSON, always compiled in. Chrome: a
// line-number gutter that follows the document, mono font, no-wrap horizontal scroll.
//
// The transpiler emits:  WCode.CodeEditor { cssClass: […]; language: "JavaScript"; text: <expr> }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css
import solidqml.native 1.0 as Native

Css.CssFill {
    id: root
    property string language: "JavaScript"
    property alias text: edit.text
    readonly property bool highlighting: hl.available

    cssPrimitive: "div"
    implicitWidth: 560
    implicitHeight: 320

    Native.CodeHighlighter {
        id: hl
        document: edit.textDocument
        language: root.language
    }

    Item {
        anchors.fill: parent

        Css.CssFill {
            id: gutter
            cssPrimitive: "div"
            cssClass: ["code-gutter"]
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 46
            Column {
                x: 0
                y: 8 - body.contentY
                Repeater {
                    model: edit.lineCount
                    delegate: Css.CssText {
                        cssPrimitive: ""
                        cssClass: ["code-line-no"]
                        width: gutter.width - 10
                        height: edit.cursorRectangle.height
                        text: index + 1
                    }
                }
            }
        }

        Flickable {
            id: body
            anchors.left: gutter.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            clip: true
            contentWidth: Math.max(width, edit.contentWidth + 24)
            contentHeight: edit.contentHeight + 16
            boundsBehavior: Flickable.StopAtBounds

            T.TextArea {
                id: edit
                width: Math.max(body.width, contentWidth + 24)
                height: Math.max(body.height, contentHeight + 16)
                padding: 8
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.NoWrap
                selectByMouse: true
                font.family: "monospace"
                font.pixelSize: 13
                color: "#1f2328"
                tabStopDistance: 4 * 8
            }
        }
    }
}
