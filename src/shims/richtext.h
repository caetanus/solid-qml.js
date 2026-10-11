#pragma once

#include <QObject>
#include <QQuickTextDocument>
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
    Q_PROPERTY(int heading READ heading NOTIFY formatChanged)
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

public:
    using QObject::QObject;

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
    int heading() const;

    Q_INVOKABLE void toggleBold();
    Q_INVOKABLE void toggleItalic();
    Q_INVOKABLE void toggleUnderline();
    Q_INVOKABLE void toggleStrike();
    Q_INVOKABLE void toggleBulletList();
    // 0 → "Text Body", 1..6 → "Heading n" (applyParagraphStyle).
    Q_INVOKABLE void setHeading(int level);

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
    // An empty, unmodified document (no undo history, no image resources).
    Q_INVOKABLE void newDocument();

    // Content, for the app: the HTML Qt's editor holds, and plain text with "\n" between paragraphs.
    Q_INVOKABLE QString toHtml() const;
    Q_INVOKABLE QString plainText() const;
    // Plain text at the caret ("\n" starts a new paragraph, as Enter does), in the formatting
    // around it.
    Q_INVOKABLE void insertText(const QString &text);

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

private:
    friend class StyleUndo;
    // A definition change as an undo item: restores `style` (or removes it when absent).
    void restoreStyle(const QString &name, const std::optional<rt::NamedStyle> &style);
    // Re-styles blocks/runs of [from, to] (whole document when from < 0): paragraph style through
    // paraMap, character style through charMap, formats rebased from `oldSheet` to m_sheet;
    // `force` drops direct formatting. Character runs are clipped to [from, to) when clipChars.
    void restyle(int from, int to, const rt::StyleSheet &oldSheet, const std::function<QString(const QString &)> &paraMap,
                 const std::function<QString(const QString &)> &charMap, bool force, bool clipChars);
    void breakParagraph(QTextCursor &c);
    QTextCursor selectionCursor() const; // the selection exactly (no word expansion); null cursor if none
    void setError(const QString &message); // "" = success; non-empty also emits ioError
    bool writeOdf(const QUrl &file);        // saveOdf/saveOdfCopy: the write + its errorString
    void updateViewportCulling();           // kLargeTextThreshold → the editor's ItemObservesViewport

    QTextCursor cursor() const;              // covers the selection, or the word at the cursor
    QTextCursor insertionCursor() const;     // plain caret position (no word expansion) — inserts
    void mergeCharFormat(const QTextCharFormat &fmt);

    QQuickTextDocument *m_document = nullptr;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
    int m_cursorPosition = 0;
    QString m_error;
    rt::StyleSheet m_sheet = rt::StyleSheet::builtIns();
    QMetaObject::Connection m_modifiedConn;
    QMetaObject::Connection m_contentsConn;
    QMetaObject::Connection m_cullingConn;
};
