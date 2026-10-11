// DocumentEditor — the RichText "document" variant grown up: the APP drives the document (a word
// processor's editing surface). Same engine as <RichText> (the TextArea + the loader's
// RichTextHandler); this type adds the app-facing contract:
//   - starts EMPTY (no demo text), unmodified;
//   - `toolbar` (inherited) hides the built-in toolbar so the app draws its own;
//   - `modified` (+ modifiedChanged), cleared by loadOdf / saveOdf / newDocument (not saveOdfCopy);
//   - contentChanged(): every edit, formatting included, debounced (no payload — read plainText()/
//     toHtml() when needed); loadOdf/newDocument count as content changes too;
//   - the format state at the caret as reactive properties (bold … alignment, the named styles,
//     canUndo/canRedo);
//   - methods: file IO returning {ok, error}, content getters, every formatting operation and the
//     named styles (define / list / apply / direct formatting); stylesChanged() when the table changes.
//
// The transpiler emits:  WRich.DocumentEditor { id: _ref_doc; toolbar: false; onContentChanged: … }
// so TSX `doc.saveOdf(path)` / `doc.bold` resolve here. See docs/document-editor.md.
import QtQuick

RichText {
    id: root
    variant: "document"
    _initialHtml: ""

    // ── state the app reads (reactive) ──────────────────────────────────────────────────────────
    readonly property bool modified: !!fmt && fmt.modified
    readonly property bool bold: !!fmt && fmt.bold
    readonly property bool italic: !!fmt && fmt.italic
    readonly property bool underline: !!fmt && fmt.underline
    readonly property bool strike: !!fmt && fmt.strike
    readonly property bool bulletList: !!fmt && fmt.bulletList
    readonly property int heading: fmt ? fmt.heading : 0 // 0 = body text, 1..6
    readonly property string alignment: fmt ? fmt.alignment : "left" // left|center|right|justify
    readonly property bool inTable: !!fmt && fmt.inTable
    // Named styles at the caret (paragraph style; character style, "" = none).
    readonly property string currentParagraphStyle: fmt ? fmt.currentParagraphStyle : "Default Paragraph Style"
    readonly property string currentCharStyle: fmt ? fmt.currentCharStyle : ""
    readonly property bool canUndo: _editor.canUndo
    readonly property bool canRedo: _editor.canRedo

    // ── page look (office item 2b, ★1) ──────────────────────────────────────────────────────────
    // `pageWidth` (px): the editor's implicit width when set (0 = the plain editor's 560 default).
    // Continuous — no on-screen page breaks (see insertPageBreak() for the content-level marker).
    property real pageWidth: 0
    implicitWidth: pageWidth > 0 ? pageWidth : 560
    // `pageMargins`: [top, right, bottom, left] px, or null for the plain editor's 12px uniform
    // default. Applied as the TextArea's padding, so the caret and selection respect them.
    property var pageMargins: null
    _editor.topPadding: pageMargins ? pageMargins[0] : 12
    _editor.rightPadding: pageMargins ? pageMargins[1] : 12
    _editor.bottomPadding: pageMargins ? pageMargins[2] : 12
    _editor.leftPadding: pageMargins ? pageMargins[3] : 12

    // ── zoom (office item 2b, ★2) ────────────────────────────────────────────────────────────────
    // The app owns the value; clamped the same way the engine clamps it (defensive — a value
    // outside 0.5..3.0 would otherwise just silently clamp one call later). A REAL relayout (see
    // RichTextHandler::setZoom): never a scale transform, so text stays sharp and saveOdf's
    // output does not depend on it.
    property real zoom: 1.0
    // The editor's OWN base font stands in for Default Paragraph Style's (see RichText.qml /
    // docs: plain body text carries no explicit per-run font size), so zoom must scale it too —
    // everything ELSE (headings, direct formatting, named styles with their own fontSize) is
    // already baked per-run into the document and rescaled by RichTextHandler::setZoom.
    _editor.font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "14px", 14) * zoom
    onZoomChanged: {
        if (fmt) fmt.zoom = Math.max(0.5, Math.min(3.0, zoom))
        Qt.callLater(root._keepCaretVisible)
    }
    // Ctrl+wheel / Ctrl+= / Ctrl+- / Ctrl+0: the app owns `zoom`, so these only ask for a change
    // (delta +0.1/-0.1, 0 = reset) — set zoom from an onZoomRequested handler to act on it.
    signal zoomRequested(real delta)
    WheelHandler {
        target: null
        acceptedModifiers: Qt.ControlModifier
        onWheel: (event) => root.zoomRequested(event.angleDelta.y > 0 ? 0.1 : -0.1)
    }
    Shortcut { sequence: "Ctrl+="; onActivated: root.zoomRequested(0.1) }
    Shortcut { sequence: "Ctrl++"; onActivated: root.zoomRequested(0.1) }
    Shortcut { sequence: "Ctrl+-"; onActivated: root.zoomRequested(-0.1) }
    Shortcut { sequence: "Ctrl+0"; onActivated: root.zoomRequested(0) }
    function _keepCaretVisible() {
        var r = _editor.positionToRectangle(_editor.cursorPosition)
        if (r.y < _body.contentY) _body.contentY = Math.max(0, r.y)
        else if (r.y + r.height > _body.contentY + _body.height) _body.contentY = r.y + r.height - _body.height
    }

    // Debounced edit notification (ms of quiet before it fires).
    property int contentDelay: 300
    signal contentChanged()
    // The style table changed (a definition, a load, a new document, an undo of a definition).
    signal stylesChanged()

    Timer {
        id: debounce
        interval: root.contentDelay
        onTriggered: root.contentChanged()
    }
    Connections {
        target: root.fmt
        function onContentsChanged() { debounce.restart() }
        function onStylesChanged() { root.stylesChanged() }
    }
    // The handler arrives in RichText's Component.onCompleted: start from an empty, unmodified
    // document, without reporting that as an edit.
    onFmtChanged: if (fmt) { fmt.newDocument(); debounce.stop() }

    // ── file IO and content ──────────────────────────────────────────────────────────────────────
    // `path`: a plain filesystem path or a file:// URL.
    function _result(ok) { return { ok: ok, error: ok ? "" : fmt.errorString } }
    readonly property var _noEngine: ({ ok: false, error: "DocumentEditor: no solidqml.native.RichTextHandler in this host" })

    function loadOdf(path) {
        if (!fmt) return _noEngine
        var r = _result(fmt.loadOdf(path))
        if (r.ok) { _editor.cursorPosition = 0; _editor.forceActiveFocus() }
        return r
    }
    function saveOdf(path) { return fmt ? _result(fmt.saveOdf(path)) : _noEngine }
    // Autosave / crash recovery: the same file, but `modified` and the undo stack stay as they are.
    function saveOdfCopy(path) { return fmt ? _result(fmt.saveOdfCopy(path)) : _noEngine }
    function newDocument() {
        if (!fmt) return
        fmt.newDocument()
        _editor.cursorPosition = 0
        _editor.forceActiveFocus()
    }
    function toHtml() { return fmt ? fmt.toHtml() : _editor.text }
    function plainText() { return fmt ? fmt.plainText() : "" }
    function insertText(text) { if (fmt) { fmt.insertText(text); _editor.forceActiveFocus() } }
    function focusEditor() { _editor.forceActiveFocus() }
    function undo() { _editor.undo() }
    function redo() { _editor.redo() }

    // ── formatting (the built-in toolbar's operations, for the app's own toolbar/shortcuts) ──────
    // B/I/U/S with nothing selected apply to what is typed next, as the toolbar does.
    function toggleBold() { _toggle("bold"); _editor.forceActiveFocus() }
    function toggleItalic() { _toggle("italic"); _editor.forceActiveFocus() }
    function toggleUnderline() { _toggle("underline"); _editor.forceActiveFocus() }
    function toggleStrike() { _toggle("strike"); _editor.forceActiveFocus() }
    function toggleBulletList() { if (fmt) fmt.toggleBulletList(); _editor.forceActiveFocus() }
    function setHeading(level) { if (fmt) fmt.setHeading(level); _editor.forceActiveFocus() }
    function setAlignment(align) { if (fmt) fmt.setAlignment(align); _editor.forceActiveFocus() }
    function insertTable(rows, cols) { if (fmt) fmt.insertTable(rows, cols); _editor.forceActiveFocus() }
    function addTableRow() { if (fmt) fmt.addTableRow() }
    function addTableColumn() { if (fmt) fmt.addTableColumn() }
    function removeTableRow() { if (fmt) fmt.removeTableRow() }
    function removeTableColumn() { if (fmt) fmt.removeTableColumn() }
    function insertImage(path) { return fmt ? _result(fmt.insertImage(path)) : _noEngine }

    // ── named styles (LibreOffice's names: "Default Paragraph Style", "Text Body", "Heading 1"…) ──
    // defineStyle: create / update (props.family "paragraph" | "character" for a new one; a null
    // property removes it); every paragraph or run using the style follows, direct formatting kept.
    function defineStyle(name, props) {
        if (!fmt) return _noEngine
        var e = fmt.defineStyle(name, props || {})
        return { ok: e === "", error: e }
    }
    function styleNames(family) { return fmt ? fmt.styleNames(family || "paragraph") : [] }
    function styleProperties(name, resolved) { return fmt ? fmt.styleProperties(name, !!resolved) : ({}) }
    function applyParagraphStyle(name) { var ok = !!fmt && fmt.applyParagraphStyle(name); _editor.forceActiveFocus(); return ok }
    function applyCharStyle(name) { var ok = !!fmt && fmt.applyCharStyle(name || ""); _editor.forceActiveFocus(); return ok }
    function clearDirectFormatting() { if (fmt) fmt.clearDirectFormatting(); _editor.forceActiveFocus() }
    // What overrides the styles at `pos` (default: the caret).
    function directFormattingAt(pos) {
        return fmt ? fmt.directFormattingAt(pos === undefined ? _editor.cursorPosition : pos) : ({})
    }
}
