// DocumentEditor — the RichText "document" variant grown up: the APP drives the document (a word
// processor's editing surface). Same engine as <RichText> (the TextArea + the loader's
// RichTextHandler); this type adds the app-facing contract:
//   - starts EMPTY (no demo text), unmodified;
//   - `toolbar` (inherited) hides the built-in toolbar so the app draws its own;
//   - `modified` (+ modifiedChanged), cleared by loadOdf / saveOdf / newDocument;
//   - contentChanged(): every edit, formatting included, debounced (no payload — read plainText()/
//     toHtml() when needed); loadOdf/newDocument count as content changes too;
//   - the format state at the caret as reactive properties (bold … alignment, canUndo/canRedo);
//   - methods: file IO returning {ok, error}, content getters, and every formatting operation.
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
    readonly property bool canUndo: _editor.canUndo
    readonly property bool canRedo: _editor.canRedo

    // Debounced edit notification (ms of quiet before it fires).
    property int contentDelay: 300
    signal contentChanged()

    Timer {
        id: debounce
        interval: root.contentDelay
        onTriggered: root.contentChanged()
    }
    Connections {
        target: root.fmt
        function onContentsChanged() { debounce.restart() }
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
}
