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

public:
    using QObject::QObject;

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
    // it into Pictures/ on save) and capped to a word-like page width.
    Q_INVOKABLE void insertImage(const QUrl &file);

    // Tables (owner: "no richtext temos tabela também"): a bordered QTextTable at the cursor,
    // plus row/column edits relative to the current cell.
    Q_INVOKABLE void insertTable(int rows, int cols);
    Q_INVOKABLE void addTableRow();
    Q_INVOKABLE void addTableColumn();
    Q_INVOKABLE void removeTableRow();
    Q_INVOKABLE void removeTableColumn();
    bool inTable() const;

    Q_INVOKABLE bool saveOdf(const QUrl &file);
    Q_INVOKABLE bool loadOdf(const QUrl &file);

signals:
    void documentChanged();
    void selectionChanged();
    void formatChanged();
    void ioError(const QString &message);

private:
    QTextCursor cursor() const;              // covers the selection, or the word at the cursor
    QTextCursor insertionCursor() const;     // plain caret position (no word expansion) — inserts
    void mergeCharFormat(const QTextCharFormat &fmt);

    QQuickTextDocument *m_document = nullptr;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
    int m_cursorPosition = 0;
};
