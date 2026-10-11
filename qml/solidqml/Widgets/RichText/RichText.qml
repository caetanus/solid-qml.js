// RichText — the word-like editor, an opt-in module (solidqml.Widgets.RichText). No extra Qt
// dependency: the formatting/IO engine is the loader-registered RichTextHandler (QTextCursor
// over the TextArea's document, named paragraph/character styles, our own ODF reader and writer —
// owner: "ods nativo"). The chrome is all CSS boxes: a toolbar of format
// toggles (cssState "active" mirrors the format at the cursor), open/save through the native
// platform file dialogs, and a TextEdit in RichText mode inside the flickable body.
//
// The transpiler emits:  WRich.RichText { cssClass: […] }
//                    or  WRich.RichText { variant: "mail"; html: <string>; onEdited: (html, text) => … }
//
// variant "mail" — a message being written: the toolbar keeps what mail carries (bold, italic,
// underline, strike, a list), no headings, tables, images or ODF files. `html` sets the content
// (a draft, a quote, a signature) — when it is what the editor itself last reported, nothing
// happens, so the caret stays put while the app echoes edits back; `edited(html, text)` reports
// each change, the plain text alongside for a text/plain part.
//
// <DocumentEditor> (DocumentEditor.qml) is this editor driven by the APP: empty start, file IO,
// `modified`, debounced contentChanged, format state and operations through a ref.
import QtQuick
import Qt.labs.platform 1.1 as Platform
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    cssPrimitive: "div"
    implicitWidth: 560
    implicitHeight: 380
    property string variant: "document"
    property string html: ""
    // The built-in toolbar (formatting needs the loader's handler either way).
    property bool toolbar: true
    // What the document variant shows when created. DocumentEditor (the app-driven document,
    // DocumentEditor.qml) starts empty instead.
    property string _initialHtml: "<h1>solid-qml</h1><p>A <b>word-like</b> editor with <i>native</i> " +
                                  "<u>OpenDocument</u> round-trip.</p>"
    // The text area, for DocumentEditor (ids don't cross the file boundary).
    readonly property alias _editor: edit
    // Insets belong to the scrolling sheet, not the observed text viewport. TextArea
    // subtracts its padding from that viewport on every frame, cutting off bottom lines.
    property real _sheetWidth: 0
    property real _sheetContentHeight: 0
    property real _textTopMargin: 12
    property real _textRightMargin: 12
    property real _textBottomMargin: 12
    property real _textLeftMargin: 12
    // The scrolling viewport around it — DocumentEditor keeps the caret visible after a zoom change.
    readonly property alias _body: body
    signal edited(string html, string text)
    readonly property bool _mail: variant === "mail"
    property string _reported: ""
    // New content from the app (a new draft): the caret at its top, the keyboard in it.
    // Its report waits a turn: echoed inside this handler, the app's binding of `html` looped.
    property bool _loading: false
    onHtmlChanged: if (html !== _reported) {
        _reported = html; _loading = true; edit.text = html; _loading = false
        edit.cursorPosition = 0; edit.forceActiveFocus(); Qt.callLater(_report)
    }
    // B / I / U / S: on the selection (the handler). Nothing selected: on what is typed next —
    // the handler's cursor is a copy of the editor's, so the toggle waits (_pending) and is applied
    // to the first characters typed, which the following ones then inherit, as in any editor.
    property var _pending: ({})
    property int _pendingAt: -1
    property bool _applying: false
    function _apply(what) {
        if (what === "bold") fmt.toggleBold()
        else if (what === "italic") fmt.toggleItalic()
        else if (what === "underline") fmt.toggleUnderline()
        else fmt.toggleStrike()
    }
    function _toggle(what) {
        if (!fmt) return
        if (edit.selectionStart !== edit.selectionEnd) return _apply(what)
        if (_pendingAt !== edit.cursorPosition) { _pending = ({}); _pendingAt = edit.cursorPosition }
        var p = _pending
        p[what] = !p[what]
        _pending = p
    }
    function _applyPending() {
        if (_pendingAt < 0 || _applying) return
        var from = _pendingAt, to = edit.cursorPosition
        var p = _pending
        _pendingAt = -1
        _pending = ({})
        if (to <= from) return
        _applying = true
        edit.select(from, to)
        for (var k in p) if (p[k]) _apply(k)
        edit.deselect()
        edit.cursorPosition = to
        _applying = false
    }
    // Cursor changes alone drive following: a wheel/track/drag never snaps back.
    function _keepCaretVisible() {
        var r = edit.positionToRectangle(edit.cursorPosition)
        var context = Math.min(r.height, Math.max(0, (body.height - r.height) / 2))
        var top = r.y + edit.y - context
        var bottom = r.y + edit.y + r.height + context
        var y = body.contentY
        if (top < y) y = top
        else if (bottom > y + body.height) y = bottom - body.height
        body.contentY = Math.max(0, Math.min(y, Math.max(0, body.contentHeight - body.height)))
        var x = r.x + edit.x
        if (x < body.contentX) body.contentX = Math.max(0, x)
        else if (x + r.width > body.contentX + body.width)
            body.contentX = Math.min(Math.max(0, body.contentWidth - body.width), x + r.width - body.width)
    }
    function _followCursor() {
        _keepCaretVisible()
        // Paste/undo can finish document layout after cursorPositionChanged. Follow once
        // more after layout, unless the user or app has moved the viewport in the meantime.
        Qt.callLater(_finishFollowing, edit.cursorPosition, body.contentX, body.contentY)
    }
    function _finishFollowing(position, x, y) {
        // Flickable can align a fractional content position to its nearest pixel at polish.
        if (edit.cursorPosition === position && Math.round(body.contentX) === Math.round(x)
            && Math.round(body.contentY) === Math.round(y))
            _keepCaretVisible()
    }
    function _navigatePage(event) {
        var end = (event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_Home || event.key === Qt.Key_End)
        var page = event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown
        if (!end && !page) return false
        var from = edit.cursorPosition
        var to
        if (end) to = event.key === Qt.Key_Home ? 0 : edit.length
        else {
            var r = edit.positionToRectangle(from)
            to = edit.positionAt(r.x, r.y + r.height / 2 + (event.key === Qt.Key_PageUp ? -body.height : body.height))
        }
        if (event.modifiers & Qt.ShiftModifier) {
            var anchor = from === edit.selectionStart ? edit.selectionEnd : edit.selectionStart
            edit.select(anchor, to)
        } else edit.cursorPosition = to
        // Also handles Home/End when already at the boundary but scrolled away.
        _keepCaretVisible()
        event.accepted = true
        return true
    }
    function _refreshScrolledViewport() { if (fmt) fmt.refreshViewport() }
    function _report() {
        if (!root._mail || root._loading) return
        root._reported = edit.text
        // Qt's plain text separates paragraphs with U+2029 (lines with U+2028): real line breaks.
        root.edited(edit.text, edit.getText(0, edit.length).replace(/[\u2028\u2029]/g, "\n"))
    }

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

    // The formatting engine is the loader's (solidqml.native): created here, not declared, so a
    // host that does not register it (an embedding app on an older library) still gets an editor —
    // typing and rich paste work, only the format buttons are hidden.
    property var fmt: null
    readonly property bool _canFormat: fmt !== null
    Component.onCompleted: {
        if (_mail) { _reported = html; edit.cursorPosition = 0; edit.forceActiveFocus() }
        try {
            fmt = Qt.createQmlObject("import solidqml.native 1.0 as Native; Native.RichTextHandler {}", root, "RichTextHandler")
            fmt.document = Qt.binding(function() { return edit.textDocument })
            fmt.selectionStart = Qt.binding(function() { return edit.selectionStart })
            fmt.selectionEnd = Qt.binding(function() { return edit.selectionEnd })
            fmt.cursorPosition = Qt.binding(function() { return edit.cursorPosition })
        } catch (e) {
            console.warn("RichText: no solidqml.native.RichTextHandler — formatting off")
        }
    }

    Platform.FileDialog {
        id: openDialog
        fileMode: Platform.FileDialog.OpenFile
        nameFilters: ["OpenDocument text (*.odt)"]
        onAccepted: if (fmt) fmt.loadOdf(file)
    }
    Platform.FileDialog {
        id: saveDialog
        fileMode: Platform.FileDialog.SaveFile
        defaultSuffix: "odt"
        nameFilters: ["OpenDocument text (*.odt)"]
        onAccepted: if (fmt) fmt.saveOdf(file)
    }
    Platform.FileDialog {
        id: imageDialog
        fileMode: Platform.FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.gif *.bmp *.webp)"]
        onAccepted: if (fmt) fmt.insertImage(file)
    }

    Item {
        anchors.fill: parent

        Css.CssFill {
            id: bar
            cssPrimitive: "div"
            cssClass: ["rt-toolbar"]
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: root._canFormat && root.toolbar ? 38 : 0
            visible: root._canFormat && root.toolbar
            Row {
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                ToolBtn { label: "B"; active: !!fmt && fmt.bold; onClicked: root._toggle("bold") }
                ToolBtn { label: "I"; active: !!fmt && fmt.italic; onClicked: root._toggle("italic") }
                ToolBtn { label: "U"; active: !!fmt && fmt.underline; onClicked: root._toggle("underline") }
                ToolBtn { label: "S"; active: !!fmt && fmt.strike; onClicked: root._toggle("strike") }
                Item { width: 8; height: 1 }
                ToolBtn { label: "H1"; visible: !root._mail; active: !!fmt && fmt.heading === 1; onClicked: if (fmt) fmt.setHeading(fmt.heading === 1 ? 0 : 1) }
                ToolBtn { label: "H2"; visible: !root._mail; active: !!fmt && fmt.heading === 2; onClicked: if (fmt) fmt.setHeading(fmt.heading === 2 ? 0 : 2) }
                ToolBtn { label: "•"; active: !!fmt && fmt.bulletList; onClicked: if (fmt) fmt.toggleBulletList() }
                Item { width: 8; height: 1 }
                ToolBtn { label: "🖼"; visible: !root._mail; onClicked: imageDialog.open() }
                ToolBtn { label: "⊞"; visible: !root._mail; onClicked: if (fmt) fmt.insertTable(3, 3) }
                // Row/column edits: only offered while the caret sits inside a table.
                ToolBtn { label: "R+"; visible: !!fmt && fmt.inTable && !root._mail; onClicked: if (fmt) fmt.addTableRow() }
                ToolBtn { label: "C+"; visible: !!fmt && fmt.inTable; onClicked: if (fmt) fmt.addTableColumn() }
                ToolBtn { label: "R−"; visible: !!fmt && fmt.inTable; onClicked: if (fmt) fmt.removeTableRow() }
                ToolBtn { label: "C−"; visible: !!fmt && fmt.inTable; onClicked: if (fmt) fmt.removeTableColumn() }
                Item { width: 8; height: 1 }
                ToolBtn { label: "⇱"; visible: !root._mail; onClicked: openDialog.open() }
                ToolBtn { label: "⇲"; visible: !root._mail; onClicked: saveDialog.open() }
            }
        }

        Flickable {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: bar.bottom
            anchors.bottom: parent.bottom
            clip: true
            contentWidth: Math.max(width, root._sheetWidth)
            contentHeight: root._sheetContentHeight > 0 ? root._sheetContentHeight : edit.y + edit.height + root._textBottomMargin
            boundsBehavior: Flickable.StopAtBounds
            property real _previousContentY: 0
            onContentYChanged: {
                // After Flickable's transform notifications: Qt's backward scan can stop
                // at a table before it reaches the visible text. Coalesced once per turn.
                if (contentY < _previousContentY) Qt.callLater(root._refreshScrolledViewport)
                _previousContentY = contentY
            }

            TextEdit {
                id: edit
                x: root._sheetWidth > 0 ? Math.max(0, (body.width - root._sheetWidth) / 2) : root._textLeftMargin
                y: root._textTopMargin
                width: root._sheetWidth > 0 ? root._sheetWidth : Math.max(0, body.width - root._textLeftMargin - root._textRightMargin)
                height: Math.max(body.height - root._textTopMargin - root._textBottomMargin, root._sheetContentHeight, contentHeight)
                padding: 0
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                color: root.inheritedColor ? cssTheme.parseColor(root.inheritedColor) : "#1f2328"
                font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
                font.pixelSize: root._mail ? 14 : cssTheme.parseFontSize(root.inheritedFontSize || "14px", 14)
                text: root._mail ? root.html : root._initialHtml
                onCursorPositionChanged: root._followCursor()
                onTextChanged: { if (root._pendingAt >= 0) Qt.callLater(root._applyPending); root._report() }
                // Enter: a new paragraph in the style's next style ("Heading 1" → "Text Body");
                // Shift+Enter stays a line break. Ctrl+B / Ctrl+I / Ctrl+U, as in every editor.
                Keys.onPressed: (event) => {
                    if (root._navigatePage(event)) return
                    if (!root.fmt) return
                    if (!root._mail && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        && !(event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                        root.fmt.insertParagraph()
                        event.accepted = true
                        return
                    }
                    // Shift+Insert: the traditional Ctrl-less paste shortcut — route it like
                    // Ctrl+V below so it goes through the handler's paste() (office item 2b, ★2
                    // follow-up: zoom rescaling on entry) instead of the editor's own built-in,
                    // unscaled paste.
                    if (event.key === Qt.Key_Insert && (event.modifiers & Qt.ShiftModifier)
                        && !(event.modifiers & Qt.ControlModifier)) {
                        root.fmt.paste()
                        event.accepted = true
                        return
                    }
                    if (!(event.modifiers & Qt.ControlModifier)) return
                    if (event.key === Qt.Key_B) { root._toggle("bold"); event.accepted = true }
                    else if (event.key === Qt.Key_I) { root._toggle("italic"); event.accepted = true }
                    else if (event.key === Qt.Key_U) { root._toggle("underline"); event.accepted = true }
                    // Ctrl+C/X/V: routed to the handler, NOT the editor's own built-in copy/cut/
                    // paste, so the clipboard-boundary zoom rescaling at the C++ choke point
                    // (RichTextHandler::copy/cut/paste) always runs — see
                    // docs/document-editor.md "Clipboard".
                    else if (event.key === Qt.Key_C) { root.fmt.copy(); event.accepted = true }
                    else if (event.key === Qt.Key_X) { root.fmt.cut(); event.accepted = true }
                    else if (event.key === Qt.Key_V) { root.fmt.paste(); event.accepted = true }
                }
            }
        }
    }
}
