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
    // Office item 2b (item 5): mutually exclusive with bulletList.
    readonly property bool numberedList: !!fmt && fmt.numberedList
    readonly property int heading: fmt ? fmt.heading : 0 // 0 = body text, 1..6
    // Office item 2b (★4): character format at the caret/selection. "" / 0 when unset.
    readonly property string fontFamily: fmt ? fmt.fontFamily : ""
    readonly property real fontSize: fmt ? fmt.fontSize : 0
    readonly property string alignment: fmt ? fmt.alignment : "left" // left|center|right|justify
    readonly property bool inTable: !!fmt && fmt.inTable
    // Named styles at the caret (paragraph style; character style, "" = none).
    readonly property string currentParagraphStyle: fmt ? fmt.currentParagraphStyle : "Default Paragraph Style"
    readonly property string currentCharStyle: fmt ? fmt.currentCharStyle : ""
    readonly property bool canUndo: _editor.canUndo
    readonly property bool canRedo: _editor.canRedo
    // Office item 2b (item 6): the SYSTEM clipboard, not this document.
    readonly property bool canPaste: !!fmt && fmt.canPaste
    // Office item 2b (item 7): kept incrementally by the engine (not recounted per keystroke).
    readonly property int wordCount: fmt ? fmt.wordCount : 0
    readonly property int charCount: fmt ? fmt.charCount : 0
    readonly property int selectionWordCount: fmt ? fmt.selectionWordCount : 0
    readonly property int selectionCharCount: fmt ? fmt.selectionCharCount : 0
    readonly property string selectedText: fmt ? fmt.selectedText : ""

    // ── page look (office item 2b, ★1) ──────────────────────────────────────────────────────────
    // `pageWidth` (px): the editor's implicit width when set (0 = the plain editor's 560 default).
    property real pageHeight: 1123
    property real pageGap: 24
    property string layoutEngine: "auto"
    onLayoutEngineChanged: Qt.callLater(_configurePages)
    property bool pageView: pageHeight > 0
    property color deskColor: "#e9e8e6"
    readonly property bool _pages: pageView && pageHeight > 0
    readonly property real _pageZoom: Math.max(0.5, Math.min(3.0, zoom))
    readonly property real _pageStride: (pageHeight + Math.max(0, pageGap)) * _pageZoom
    readonly property int pageCount: fmt ? fmt.pageCount : 1
    readonly property int currentPage: fmt ? fmt.currentPage : 1
    signal pagesChanged()
    function pageAt(position) { return fmt ? fmt.pageAt(position) : 1 }
    function scrollToPage(n) {
        if (!_pages) return
        _body.contentY = Math.max(0, Math.min((Math.max(1, Math.min(pageCount, Math.floor(n))) - 1) * _pageStride,
                                             _body.contentHeight - _body.height))
    }
    _sheetWidth: _pages ? (pageWidth > 0 ? pageWidth : 794) * _pageZoom : 0
    _sheetContentHeight: _pages ? pageCount * _pageStride - Math.max(0, pageGap) * _pageZoom : 0
    function _configurePages() {
        if (!fmt || !fmt.document) return
        fmt.layoutEngine = layoutEngine
        var m = pageMargins || [12, 12, 12, 12]
        fmt.configurePages(_editor.width, _pages ? pageHeight * _pageZoom : 0,
                           Math.max(0, pageGap) * _pageZoom, m.map(v => v * _pageZoom))
    }
    on_PagesChanged: Qt.callLater(_configurePages)
    on_PageStrideChanged: Qt.callLater(_configurePages)
    onPageMarginsChanged: Qt.callLater(_configurePages)
    Connections {
        target: root._editor
        function onWidthChanged() { Qt.callLater(root._configurePages) }
    }
    Rectangle {
        parent: root._body
        anchors.fill: parent
        z: -2
        visible: root._pages
        color: root.deskColor
    }
    Item {
        parent: root._body.contentItem
        z: -1
        visible: root._pages
        // A bounded pool is moved as the viewport scrolls. No binding depends on text edits.
        readonly property int firstPage: root._pages ? Math.max(0, Math.ceil((root._body.contentY - root._body.height - root.pageHeight * root._pageZoom) / root._pageStride)) : 0
        readonly property int poolSize: root._pages ? Math.max(0, Math.min(root.pageCount - 1,
                                                    Math.floor((root._body.contentY + 2 * root._body.height) / root._pageStride)) - firstPage + 1) : 0
        Repeater {
            model: parent.poolSize
            delegate: Rectangle {
                required property int index
                objectName: "documentSheet"
                x: root._editor.x
                y: (parent.firstPage + index) * root._pageStride
                width: root._sheetWidth
                height: root.pageHeight * root._pageZoom
                color: "white"
                // Small translucent layers give a soft edge without a texture/effect per sheet.
                Rectangle { anchors.fill: parent; anchors.margins: -3; z: -2; color: "#08000000"; radius: 3 }
                Rectangle { anchors.fill: parent; anchors.margins: -1; z: -1; color: "#18000000"; radius: 1 }
            }
        }
    }
    // Explicit breaks retain their draft marker in continuous view.
    property string pageBreakLabel: "Quebra de página"
    property real pageWidth: 0
    implicitWidth: pageWidth > 0 ? pageWidth : 560
    // `pageMargins`: [top, right, bottom, left] px, or null for the plain editor's 12px uniform
    // default. Page view uses document margins; continuous view uses outer item insets.
    property var pageMargins: null
    _textTopMargin: _pages ? 0 : (pageMargins ? pageMargins[0] : 12)
    _textRightMargin: _pages ? 0 : (pageMargins ? pageMargins[1] : 12)
    _textBottomMargin: _pages ? 0 : (pageMargins ? pageMargins[2] : 12)
    _textLeftMargin: _pages ? 0 : (pageMargins ? pageMargins[3] : 12)

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
    _editor.font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "14px", 14) * _pageZoom
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

    // Debounced edit notification (ms of quiet before it fires).
    property int contentDelay: 300
    signal contentChanged()
    // The style table changed (a definition, a load, a new document, an undo of a definition).
    signal saveCopyFinished(bool ok, string error)
    signal stylesChanged()
    // Office item 2b (★3): the outline changed — debounced the same way, and only forwarded when
    // the engine says the list actually differs (RichTextHandler::headingsChanged dedupes it).
    signal headingsChanged()
    readonly property int currentHeadingIndex: fmt ? fmt.currentHeadingIndex : -1

    Timer {
        id: debounce
        interval: root.contentDelay
        onTriggered: root.contentChanged()
    }
    Timer {
        id: headingsDebounce
        interval: root.contentDelay
        onTriggered: root.headingsChanged()
    }
    Connections {
        target: root.fmt
        function onContentsChanged() { debounce.restart() }
        function onSaveCopyFinished(ok, error) { root.saveCopyFinished(ok, error) }
        function onStylesChanged() { root.stylesChanged() }
        function onHeadingsChanged() { headingsDebounce.restart() }
        function onBlocksLoaded() { root._configurePages(); root.blocksLoaded() }
        function onDocumentChanged() { root._configurePages() }
        function onPagesChanged() { root.pagesChanged() }
        function onBlockChanged(blockId) { root.blockChanged(blockId) }
        function onBlockRemoved(blockId) { root.blockRemoved(blockId) }
    }
    // The handler arrives in RichText's Component.onCompleted: start from an empty, unmodified
    // document, without reporting that as an edit.
    onFmtChanged: if (fmt) {
        fmt.zoom = _pageZoom
        fmt.pageBreakLabel = Qt.binding(function() { return root.pageBreakLabel })
        fmt.newDocument()
        debounce.stop()
    }

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
    function saveOdfCopyAsync(path) {
        if (fmt) fmt.saveOdfCopyAsync(path)
        else root.saveCopyFinished(false, _noEngine.error)
    }
    // options (office item 2b addendum): { styles: { name: {props as in defineStyle} }, paragraphStyle }
    // applied as the document's INITIAL state; {ok, error} like loadOdf — an error leaves the
    // previous document untouched.
    function newDocument(options) {
        if (!fmt) return _noEngine
        var r = fmt.newDocument(options || {})
        if (r.ok) { _editor.cursorPosition = 0; _editor.forceActiveFocus() }
        return r
    }
    function toHtml() { return fmt ? fmt.toHtml() : _editor.text }
    function plainText() { return fmt ? fmt.plainText() : "" }
    function insertText(text) { if (fmt) { fmt.insertText(text); _editor.forceActiveFocus() } }
    function setCursorPosition(position) {
        _editor.cursorPosition = Math.max(0, Math.min(_editor.length, Math.floor(position)))
        _keepCaretVisible()
    }
    function focusEditor() { _editor.forceActiveFocus() }
    function undo() { _editor.undo() }
    function redo() { _editor.redo() }
    // ── clipboard (office item 2b, item 6) ──────────────────────────────────────────────────────
    function cut() { if (fmt) fmt.cut(); _editor.forceActiveFocus() }
    function copy() { if (fmt) fmt.copy() }
    function paste() { if (fmt) fmt.paste(); _editor.forceActiveFocus() }

    // ── formatting (the built-in toolbar's operations, for the app's own toolbar/shortcuts) ──────
    // B/I/U/S with nothing selected apply to what is typed next, as the toolbar does.
    function toggleBold() { _toggle("bold"); _editor.forceActiveFocus() }
    function toggleItalic() { _toggle("italic"); _editor.forceActiveFocus() }
    function toggleUnderline() { _toggle("underline"); _editor.forceActiveFocus() }
    function toggleStrike() { _toggle("strike"); _editor.forceActiveFocus() }
    function toggleBulletList() { if (fmt) fmt.toggleBulletList(); _editor.forceActiveFocus() }
    function toggleNumberedList() { if (fmt) fmt.toggleNumberedList(); _editor.forceActiveFocus() }
    function setHeading(level) { if (fmt) fmt.setHeading(level); _editor.forceActiveFocus() }
    // ── paragraph (office item 2b, item 5) ──────────────────────────────────────────────────────
    function indent() { if (fmt) fmt.indent(); _editor.forceActiveFocus() }
    function outdent() { if (fmt) fmt.outdent(); _editor.forceActiveFocus() }
    function insertPageBreak() { if (fmt) fmt.insertPageBreak(); _editor.forceActiveFocus() }

    // ── character format (office item 2b, ★4) ────────────────────────────────────────────────────
    // The handler owns the live cursor's pending font and reports it immediately.
    function setFontFamily(name) { if (fmt) fmt.setFontFamily(name); _editor.forceActiveFocus() }
    function setFontSize(pt) { if (fmt) fmt.setFontSize(pt); _editor.forceActiveFocus() }
    // Drops direct CHARACTER formatting in the selection (bold/font/size/…); keeps character
    // styles. Reuses the engine's clearFormatting() — paragraph properties are untouched, unlike
    // clearDirectFormatting().
    function clearFormatting() { if (fmt) fmt.clearFormatting(); _editor.forceActiveFocus() }
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

    // ── outline (office item 2b, ★3) ────────────────────────────────────────────────────────────
    // [{position, level, text}] in document order; kept incrementally by the engine (cheap even
    // on a very large document — see RichTextHandler::handleContentsChange).
    function headings() { return fmt ? fmt.headings() : [] }
    // Scrolls so `pos` sits near the top of the viewport.
    function scrollToPosition(pos) {
        var r = _editor.positionToRectangle(pos)
        r.y += _editor.y
        _body.contentY = Math.max(0, Math.min(r.y, Math.max(0, _body.contentHeight - _body.height)))
    }

    // ── blocks & marks (office item 3; docs/document-editor.md "Blocks and marks") ─────────────
    // Every paragraph has a stable string id (kept through typing, other blocks' edits and
    // undo/redo; saved as xml:id). blockChanged/blockRemoved are coalesced by the engine and
    // arrive on the next event-loop turn; blocksLoaded after loadOdf/newDocument (the app's
    // first full pass — it replaces every block, no removals are reported for that).
    signal blocksLoaded()
    signal blockChanged(string blockId)
    signal blockRemoved(string blockId)
    // `mark`: {kind, id, blockId, start, end}; x, y in this item's coordinates; windowX/windowY
    // the same point in the window's (what <Menu ref> open(x, y) takes).
    signal markClicked(var mark, real x, real y, real windowX, real windowY)
    signal markContextMenu(var mark, real x, real y, real windowX, real windowY)
    signal markHovered(var mark) // null when the pointer leaves a mark

    function blockIds() { return fmt ? fmt.blockIds() : [] }
    function blockIdAt(pos) { return fmt ? fmt.blockIdAt(pos) : "" }
    function blockText(blockId) { return fmt ? fmt.blockText(blockId) : "" }
    // Marks are an overlay, not content (not saved, copied, undone, nor `modified`).
    function addMark(kind, blockId, start, end, id) { return fmt ? fmt.addMark(kind, blockId, start, end, id || "") : "" }
    function clearMarks(kind, blockId) { if (fmt) fmt.clearMarks(kind, blockId || "") }
    function markStyle(kind, style) {
        if (!fmt) return _noEngine
        var e = fmt.markStyle(kind, style || {})
        return { ok: e === "", error: e }
    }
    function markAt(pos) { var m = fmt ? fmt.markAt(pos) : undefined; return m ? m : null }
    // Applies a suggestion: one undo step, the block keeps its id.
    function replaceRange(blockId, start, end, text) {
        var ok = !!fmt && fmt.replaceRange(blockId, start, end, text)
        _editor.forceActiveFocus()
        return ok
    }

    // The mark under a point of the text area: the CHARACTER there (positionAt gives the nearest
    // caret boundary — the right half of a character answers the next position).
    function _markAtPoint(x, y) {
        if (!fmt || !fmt.hasMarks()) return null
        var p = _editor.positionAt(x, y)
        var at = (p > 0 && x < _editor.positionToRectangle(p).x) ? p - 1 : p
        var m = fmt.markAt(at)
        return m ? m : null
    }
    // Passive (DragThreshold): the editor still gets the press — the caret and selection behave
    // as without marks; a right-click outside any mark is left alone.
    TapHandler {
        parent: root._editor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onTapped: (eventPoint, button) => {
            var m = root._markAtPoint(eventPoint.position.x, eventPoint.position.y)
            if (!m) return
            var p = root.mapFromItem(root._editor, eventPoint.position.x, eventPoint.position.y)
            var w = root._editor.mapToItem(null, eventPoint.position.x, eventPoint.position.y)
            if (button === Qt.RightButton) root.markContextMenu(m, p.x, p.y, w.x, w.y)
            else root.markClicked(m, p.x, p.y, w.x, w.y)
        }
    }
    property var _hoveredMark: null
    function _setHoveredMark(m) {
        var h = _hoveredMark
        if (m === h || (m && h && m.id === h.id && m.blockId === h.blockId && m.start === h.start)) return
        _hoveredMark = m
        markHovered(m)
    }
    HoverHandler {
        parent: root._editor
        onPointChanged: if (hovered) root._setHoveredMark(root._markAtPoint(point.position.x, point.position.y))
        onHoveredChanged: if (!hovered) root._setHoveredMark(null)
    }
}
