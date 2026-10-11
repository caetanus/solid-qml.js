#pragma once

#include <QObject>
#include <QQuickTextDocument>
#include <QTextCursor>
#include <QUrl>
#include <qqml.h>

// Formatting/IO engine behind the <RichText> word-like widget (module solidqml.Widgets.RichText).
// QML text controls expose no character-format API, so the module binds its TextArea's document +
// selection here and the toolbar drives QTextCursor operations. Registered by the loader as
// `RichTextHandler` under the `solidqml.native` uri (loader-provided helper types) — no extra Qt
// dependency, so the module is opt-in purely to keep the core lean.
//
// OpenDocument (owner: "ods nativo"): saving uses Qt's NATIVE writer (QTextDocumentWriter, format
// "odf" → .odt); loading parses the .odt ourselves (Qt ships no ODF reader) — content.xml via the
// private QZipReader + QXmlStreamReader, covering the word-like subset: paragraphs, headings,
// bold/italic/underline/strike spans and flat bullet lists.
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
    // Vertical space (px) under an ODF paragraph whose style chain has none (see blockFormatFor).
    // LibreOffice draws such paragraphs with no gap; a quarter of a CSS pixel is invisible, and
    // any positive value avoids the culling over-draw (measured: 0.25 px == 1 px, 0 px ~3x).
    static constexpr qreal kMinParagraphSpacing = 0.25;

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
    Q_INVOKABLE void setHeading(int level); // 0 = body text, 1..3 = H1..H3

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
    // Plain text at the caret ("\n" starts a new paragraph), in the formatting around it.
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

private:
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
    QMetaObject::Connection m_modifiedConn;
    QMetaObject::Connection m_contentsConn;
    QMetaObject::Connection m_cullingConn;
};
