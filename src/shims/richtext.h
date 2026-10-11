#pragma once

#include <QObject>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QUrl>
#include <functional>
#include <optional>
#include <qqml.h>

#include "richtext_styles.h"

// Formatting/IO engine behind the <RichText> word-like widget (module solidqml.Widgets.RichText).
// QML text controls expose no character-format API, so the module binds its TextArea's document +
// selection here and the toolbar drives QTextCursor operations. Registered by the loader as
// `RichTextHandler` under the `solidqml.native` uri (loader-provided helper types) — no extra Qt
// dependency, so the module is opt-in purely to keep the core lean.
//
// OpenDocument (owner: "ods nativo"): our own reader and writer (richtext_odf.cpp) — the
// word-like subset (paragraphs, headings with outline levels, spans, lists, tables, images) and
// the named styles, round-tripped through styles.xml so LibreOffice opens them as styles.
//
// Named styles (office item 2, richtext_styles.h): every block is in a paragraph style, every
// text run optionally in a character style; what Qt renders is the style chain plus direct
// formatting. Redefining a style re-applies it everywhere (direct overrides kept) as one undo
// step; Enter starts the style's next style.
class RichTextHandler : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(RichTextHandler)
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int selectionStart READ selectionStart WRITE setSelectionStart NOTIFY selectionChanged)
    Q_PROPERTY(int selectionEnd READ selectionEnd WRITE setSelectionEnd NOTIFY selectionChanged)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY selectionChanged)
    // Toolbar state at the cursor/selection.
    Q_PROPERTY(bool bold READ bold NOTIFY formatChanged)
    Q_PROPERTY(bool italic READ italic NOTIFY formatChanged)
    Q_PROPERTY(bool underline READ underline NOTIFY formatChanged)
    Q_PROPERTY(bool strike READ strike NOTIFY formatChanged)
    Q_PROPERTY(bool bulletList READ bulletList NOTIFY formatChanged)
    // Office item 2b (item 5): a numbered (ListDecimal/alpha/roman) list, as opposed to bulletList
    // (ListDisc/Circle/Square) — the two are mutually exclusive at the caret.
    Q_PROPERTY(bool numberedList READ numberedList NOTIFY formatChanged)
    Q_PROPERTY(int heading READ heading NOTIFY formatChanged)
    // Office item 2b (★4): character format at the caret/selection, same mechanics as bold. ""/0
    // when unset — Default Paragraph Style carries no explicit font by design (the editor's own
    // font stands in; see ★2). fontSize is in points, in CANONICAL (zoom-1) units either way —
    // like a word processor's font-size box, it reads the same number at any zoom.
    Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY formatChanged)
    Q_PROPERTY(qreal fontSize READ fontSize NOTIFY formatChanged)
    // Named styles at the caret: the paragraph style, the character style ("" = none).
    Q_PROPERTY(QString currentParagraphStyle READ currentParagraphStyle NOTIFY formatChanged)
    Q_PROPERTY(QString currentCharStyle READ currentCharStyle NOTIFY formatChanged)
    // Cursor sits inside a table — gates the row/column toolbar actions.
    Q_PROPERTY(bool inTable READ inTable NOTIFY formatChanged)
    // Paragraph alignment at the cursor: "left" | "center" | "right" | "justify".
    Q_PROPERTY(QString alignment READ alignment NOTIFY formatChanged)
    // The document has unsaved edits (QTextDocument::isModified). Cleared by saveOdf/loadOdf/
    // newDocument; writable so a host can mark its own save points.
    Q_PROPERTY(bool modified READ isModified WRITE setModified NOTIFY modifiedChanged)
    // Why the last loadOdf/saveOdf failed ("" after a success).
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    // Office item 2b (★2): a REAL relayout, clamped 0.5..3.0 — every absolute length already
    // baked into the live document's formats (font sizes, margins, letter spacing, exact line
    // heights, inline images) is rescaled in place; NOT a visual transform. m_sheet (the
    // canonical, unscaled styles saveOdf/styleProperties use) is never touched by zoom.
    Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    // Office item 2b (★3): index into headings() of the heading at/before the caret, -1 if none.
    Q_PROPERTY(int currentHeadingIndex READ currentHeadingIndex NOTIFY formatChanged)
    // Office item 2b (item 6): the SYSTEM clipboard currently carries pasteable content (text or
    // HTML) — not specific to this document, so it tracks QClipboard::dataChanged() too (another
    // app, or this one, copying something).
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY canPasteChanged)
    // Office item 2b (item 7): whole-document counts, kept incrementally (see m_blockCounts) —
    // NOT recounted per keystroke. A "word" is a QTextBoundaryFinder::Word segment starting with
    // a letter or digit; "chars" is every character of the document's text EXCLUDING the
    // paragraph-separator between blocks (QTextBlock::text() already leaves it out).
    Q_PROPERTY(int wordCount READ wordCount NOTIFY contentsChanged)
    Q_PROPERTY(int charCount READ charCount NOTIFY contentsChanged)
    // The current selection, computed live (bounded by selection size, not document size).
    Q_PROPERTY(int selectionWordCount READ selectionWordCount NOTIFY selectionChanged)
    Q_PROPERTY(int selectionCharCount READ selectionCharCount NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged)

public:
    explicit RichTextHandler(QObject *parent = nullptr);

    // QQuickTextEdit only renders the blocks near the viewport (ItemObservesViewport) when its
    // `text` is set to more than this many characters (QQuickTextEditPrivate::largeTextSizeThreshold,
    // checked in setText only). Documents built through QTextCursor — loadOdf, typing, pasting —
    // never trip it, so the handler applies the same rule to the document's character count.
    static constexpr int kLargeTextThreshold = 10000;
    // Vertical space (px) under a paragraph whose style chain has none (rt::kMinParagraphSpacing).
    static constexpr qreal kMinParagraphSpacing = rt::kMinParagraphSpacing;

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *doc);

    int selectionStart() const { return m_selectionStart; }
    int selectionEnd() const { return m_selectionEnd; }
    int cursorPosition() const { return m_cursorPosition; }
    void setSelectionStart(int v);
    void setSelectionEnd(int v);
    void setCursorPosition(int v);

    bool bold() const;
    bool italic() const;
    bool underline() const;
    bool strike() const;
    bool bulletList() const;
    bool numberedList() const;
    int heading() const;
    QString fontFamily() const;
    qreal fontSize() const; // pt, canonical (zoom-1) units

    Q_INVOKABLE void toggleBold();
    Q_INVOKABLE void toggleItalic();
    Q_INVOKABLE void toggleUnderline();
    Q_INVOKABLE void toggleStrike();
    Q_INVOKABLE void toggleBulletList();
    // Mirrors toggleBulletList() exactly: if the caret is in ANY list, detach it (plain
    // paragraph); otherwise start a ListDecimal list. Toggling the other kind while already in a
    // list turns the list off (same as toggling the current kind twice would), not a conversion.
    Q_INVOKABLE void toggleNumberedList();
    // 0 → "Text Body", 1..6 → "Heading n" (applyParagraphStyle).
    Q_INVOKABLE void setHeading(int level);
    // Office item 2b (★4): on the selection, or on what is typed next with nothing selected —
    // same mechanics as toggleBold. `pt` is canonical (zoom-1); applied at the zoom in effect.
    Q_INVOKABLE void setFontFamily(const QString &name);
    Q_INVOKABLE void setFontSize(qreal pt);
    // The selection's runs back to exactly what their (character) style gives — named character
    // styles are KEPT, only direct character formatting (bold, font, size, …) is dropped. Unlike
    // clearDirectFormatting(), paragraph properties (alignment, margins, …) are untouched.
    Q_INVOKABLE void clearFormatting();

    // ── named styles ──────────────────────────────────────────────────────────────────────────
    // Creates or updates a style. props: { family: "paragraph"|"character" (new styles; default
    // paragraph), parent, nextStyle, and the style properties (rt::Prop — fontFamily, fontSize,
    // …) }; a property set to null is removed, one left out is kept. Every paragraph / run using
    // the style (or one inheriting from it) is re-styled, direct formatting kept — one undo step.
    // Returns "" or what is wrong (nothing changes then).
    Q_INVOKABLE QString defineStyle(const QString &name, const QVariantMap &props);
    // "paragraph" | "character": the style names, in definition order.
    Q_INVOKABLE QStringList styleNames(const QString &family) const;
    // { family, parent, nextStyle, …own properties } — or the values the whole chain gives when
    // `resolved`. {} for an unknown name.
    Q_INVOKABLE QVariantMap styleProperties(const QString &name, bool resolved = false) const;
    // On every paragraph the selection touches (the caret's paragraph without one). false for an
    // unknown / character style.
    Q_INVOKABLE bool applyParagraphStyle(const QString &name);
    // On the selection (the word at the caret without one); "" removes the character style.
    Q_INVOKABLE bool applyCharStyle(const QString &name);
    QString currentParagraphStyle() const;
    QString currentCharStyle() const;
    // The selection's runs (and its paragraphs) back to exactly what their styles give; the caret's
    // paragraph without a selection.
    Q_INVOKABLE void clearDirectFormatting();
    // What overrides the styles at `pos` (the character after it): { paragraphStyle, charStyle,
    // paragraph: {prop: value}, character: {prop: value} } — null for a property the style sets
    // and the text removes.
    Q_INVOKABLE QVariantMap directFormattingAt(int pos) const;
    // Enter: splits the paragraph at the caret (replacing a selection); at the end of a paragraph
    // the new one gets the style's next style ("Heading 1" → "Text Body").
    Q_INVOKABLE void insertParagraph();
    const rt::StyleSheet &styleSheet() const { return m_sheet; }

    // ── paragraph (office item 2b, item 5) ────────────────────────────────────────────────────
    // In a list: one level deeper/shallower (a new/different QTextList; outdent() past level 1
    // detaches, plain paragraph). Otherwise: the paragraph's own left margin, one step (at the
    // zoom in effect — a length, like setFontSize()).
    Q_INVOKABLE void indent();
    Q_INVOKABLE void outdent();
    // Splits the paragraph at the caret (insertParagraph()'s mechanics) and sets
    // QTextFormat::PageBreak_AlwaysBefore on the NEW (next) paragraph. Written to ODF as
    // fo:break-before="page" on an automatic paragraph style; read back the same way.
    Q_INVOKABLE void insertPageBreak();

    // ── outline (office item 2b, ★3) ──────────────────────────────────────────────────────────
    // [{position, level, text}] in document order (level: the paragraph's heading/outline level).
    // m_headings is kept incrementally (see handleContentsChange) — this just reads it out; cheap
    // even on a very large document.
    Q_INVOKABLE QVariantList headings() const;
    int currentHeadingIndex() const;
    // scrollToPosition(pos) is QML-level (the Flickable lives in RichText.qml) — see
    // DocumentEditor.qml, not here.

    // ── counts (office item 2b, item 7) ───────────────────────────────────────────────────────
    int wordCount() const { return int(m_totalWords); }
    int charCount() const { return int(m_totalChars); }
    int selectionWordCount() const;
    int selectionCharCount() const;
    QString selectedText() const;

    // Inline image at the cursor: loaded into a document resource (so QTextDocumentWriter embeds
    // it into Pictures/ on save) and capped to a word-like page width. `file`: URL or plain path;
    // false + errorString when the image can't be read.
    Q_INVOKABLE bool insertImage(const QUrl &file);

    // Tables (owner: "no richtext temos tabela também"): a bordered QTextTable at the cursor,
    // plus row/column edits relative to the current cell.
    Q_INVOKABLE void insertTable(int rows, int cols);
    Q_INVOKABLE void addTableRow();
    Q_INVOKABLE void addTableColumn();
    Q_INVOKABLE void removeTableRow();
    Q_INVOKABLE void removeTableColumn();
    bool inTable() const;

    QString alignment() const;
    Q_INVOKABLE void setAlignment(const QString &align); // on every paragraph of the selection

    // ── zoom (office item 2b, ★2) ─────────────────────────────────────────────────────────────
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal z);

    bool isModified() const;
    void setModified(bool m);
    QString errorString() const { return m_error; }

    // File IO. `file` is a file:// URL or a plain path. Both return false and set errorString on
    // failure; on success errorString is "" and `modified` is cleared. A failed load leaves the
    // current document untouched; a load is not an undo step.
    Q_INVOKABLE bool saveOdf(const QUrl &file);
    // The same .odt as saveOdf, for autosave / crash recovery: the document is left exactly as it
    // was — `modified` stays, the undo stack is untouched. errorString reports the result.
    Q_INVOKABLE bool saveOdfCopy(const QUrl &file);
    Q_INVOKABLE bool loadOdf(const QUrl &file);
    // An empty, unmodified document (no undo history, no image resources). `options.styles`
    // (office item 2b addendum): { name: {props as in defineStyle()}, … } defined/updated with
    // the SAME validation as defineStyle() — an error leaves the PREVIOUS document untouched and
    // returns {ok:false, error}; `options.paragraphStyle` is what the empty first paragraph
    // starts in (default: Default Paragraph Style). Still not an undo step; `modified` is still
    // cleared. The definitions are written to styles.xml on save like any other.
    Q_INVOKABLE QVariantMap newDocument(const QVariantMap &options = QVariantMap());

    // Content, for the app: the HTML Qt's editor holds, and plain text with "\n" between paragraphs.
    Q_INVOKABLE QString toHtml() const;
    Q_INVOKABLE QString plainText() const;
    // Plain text at the caret ("\n" starts a new paragraph, as Enter does), in the formatting
    // around it.
    Q_INVOKABLE void insertText(const QString &text);

    // ── clipboard (office item 2b, item 6) ────────────────────────────────────────────────────
    // HTML (rich: formatting kept) AND plain text, so both a paste back into this/another
    // DocumentEditor and a paste into a plain text field work. No-op without a selection.
    Q_INVOKABLE void copy() const;
    Q_INVOKABLE void cut();
    // Prefers HTML when the clipboard has it, else plain text; no-op (and !canPaste) otherwise.
    // Inserted as-is, at whatever size its source gives — see docs/document-editor.md "Clipboard"
    // for the (minor, cosmetic) interaction with zoom.
    Q_INVOKABLE void paste();
    bool canPaste() const;

signals:
    void documentChanged();
    void selectionChanged();
    void formatChanged();
    void ioError(const QString &message);
    void modifiedChanged();
    void errorStringChanged();
    // Every content change, formatting included (QTextDocument::contentsChanged) — undebounced;
    // the QML widget debounces it into its contentChanged().
    void contentsChanged();
    // The style table changed: a definition, a load, a new document (or an undo of a definition).
    void stylesChanged();
    // The zoom factor changed (the live document was just rescaled to it).
    void zoomChanged();
    // The outline (headings()) actually changed — undebounced, only when it really differs from
    // before (size/position/level/text); the QML widget debounces it the same way it does
    // contentsChanged() → contentChanged().
    void headingsChanged();
    // The system clipboard's content changed (any app — see QClipboard::dataChanged()).
    void canPasteChanged();

private:
    friend class StyleUndo;
    // A definition change as an undo item: restores `style` (or removes it when absent).
    void restoreStyle(const QString &name, const std::optional<rt::NamedStyle> &style);
    // Re-styles blocks/runs of [from, to] (whole document when from < 0): paragraph style through
    // paraMap, character style through charMap, formats rebased from `oldSheet` to m_sheet;
    // `force` drops direct formatting. Character runs are clipped to [from, to) when clipChars.
    // Re-applies the CURRENT zoom (m_zoom) to whatever restyle() just wrote at zoom 1 — restyle
    // always materializes from m_sheet, which is unscaled.
    void restyle(int from, int to, const rt::StyleSheet &oldSheet, const std::function<QString(const QString &)> &paraMap,
                 const std::function<QString(const QString &)> &charMap, bool force, bool clipChars);
    // Rescales every length already baked into [from, to] (whole document when from < 0) by
    // `ratio`: font point sizes, letter spacing, margins/indent, exact (fixed) line heights and
    // inline image width/height. `clipChars` mirrors restyle()'s: false touches every run of the
    // touched block(s) (and the block's own empty-paragraph charFormat), true only the runs
    // actually inside [from, to). Must run inside the caller's own edit block (one undo step).
    void rescaleRange(int from, int to, qreal ratio, bool clipChars);
    void breakParagraph(QTextCursor &c);
    QTextCursor selectionCursor() const; // the selection exactly (no word expansion); null cursor if none
    void setError(const QString &message); // "" = success; non-empty also emits ioError
    bool writeOdf(const QUrl &file);        // saveOdf/saveOdfCopy: the write + its errorString
    void updateViewportCulling();           // kLargeTextThreshold → the editor's ItemObservesViewport

    QTextCursor cursor() const;              // covers the selection, or the word at the cursor
    QTextCursor insertionCursor() const;     // plain caret position (no word expansion) — inserts
    void mergeCharFormat(const QTextCharFormat &fmt);

    // ── outline (★3) ──────────────────────────────────────────────────────────────────────────
    // A stable QTextBlock handle (survives edits elsewhere in the document; .isValid() goes false
    // only once the block itself is removed) + its heading level, kept in document order. `text`
    // is a SNAPSHOT (QTextBlock::text() is live — comparing it against itself after the same
    // block was just edited in place would always read as "unchanged").
    struct HeadingEntry { QTextBlock block; int level; QString text; };
    QList<HeadingEntry> m_headings;

    // ── counts (item 7) ───────────────────────────────────────────────────────────────────────
    // One entry per block in the WHOLE document (not just headings), document order — lets
    // handleContentsChange() turn "whole document" counts into a running total updated by the
    // DELTA of only the blocks an edit actually touched, instead of re-summing every block.
    struct BlockCount { QTextBlock block; int chars; int words; };
    QList<BlockCount> m_blockCounts;
    qint64 m_totalChars = 0;
    qint64 m_totalWords = 0;

    // Full O(blockCount) scan (both m_headings and m_blockCounts/totals): only at document
    // attach/new/load — never per keystroke.
    void rebuildIndex();
    // QTextDocument::contentsChange(position, charsRemoved, charsAdded): re-scans ONLY the blocks
    // in [position, position+charsAdded] (post-edit) for their current heading-ness and
    // char/word counts, splicing m_headings/m_blockCounts at the same spot and adjusting
    // m_totalChars/m_totalWords by the delta — O(affected blocks + heading count), not
    // O(document). Emits headingsChanged() when the outline actually differs.
    void handleContentsChange(int position, int charsRemoved, int charsAdded);

    QQuickTextDocument *m_document = nullptr;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
    int m_cursorPosition = 0;
    QString m_error;
    rt::StyleSheet m_sheet = rt::StyleSheet::builtIns();
    QMetaObject::Connection m_modifiedConn;
    QMetaObject::Connection m_contentsConn;
    QMetaObject::Connection m_cullingConn;
    QMetaObject::Connection m_headingsConn;
    qreal m_zoom = 1.0; // the live document's CURRENT zoom; m_sheet itself is never scaled
};
