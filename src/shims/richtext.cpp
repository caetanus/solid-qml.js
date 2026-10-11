#include "richtext.h"
#include "richtext_odf.h"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QQuickItem>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QAbstractUndoItem>
#include <QPointer>

RichTextHandler::RichTextHandler(QObject *parent) : QObject(parent)
{
    // The system clipboard, not this document: canPaste must track another app copying something
    // too (office item 2b, item 6).
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &RichTextHandler::canPasteChanged);
}

void RichTextHandler::setDocument(QQuickTextDocument *doc)
{
    if (m_document == doc)
        return;
    QObject::disconnect(m_modifiedConn);
    QObject::disconnect(m_contentsConn);
    QObject::disconnect(m_cullingConn);
    QObject::disconnect(m_headingsConn);
    m_document = doc;
    if (QTextDocument *td = doc ? doc->textDocument() : nullptr) {
        m_modifiedConn = connect(td, &QTextDocument::modificationChanged, this, &RichTextHandler::modifiedChanged);
        m_contentsConn = connect(td, &QTextDocument::contentsChanged, this, &RichTextHandler::contentsChanged);
        // Every edit can cross the threshold (a load, a paste, a select-all + delete). A load is
        // one edit block, so this runs once per load, not once per paragraph.
        m_cullingConn = connect(td, &QTextDocument::contentsChanged, this, &RichTextHandler::updateViewportCulling);
        // ★3: incremental outline tracking (position/charsRemoved/charsAdded — the SAME hook a
        // QSyntaxHighlighter uses; it fires after the content change, before layout).
        m_headingsConn = connect(td, &QTextDocument::contentsChange, this, &RichTextHandler::handleContentsChange);
    }
    updateViewportCulling();
    rebuildHeadings(); // a full, but infrequent (document-attach-time) scan establishes the baseline
    emit documentChanged();
    emit formatChanged();
    emit modifiedChanged();
}

namespace {

// A file:// URL or a plain path (what an app passes) → a filesystem path.
QString localPath(const QUrl &file)
{
    return file.isLocalFile() ? file.toLocalFile() : file.toString(QUrl::PreferLocalFile);
}

} // namespace

void RichTextHandler::updateViewportCulling()
{
    // The QQuickTextDocument's parent is the TextEdit that owns it (QQuickTextEdit creates it with
    // `this`), the same item Qt's own setText rule flips.
    auto *editor = m_document ? qobject_cast<QQuickItem *>(m_document->parent()) : nullptr;
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!editor || !doc)
        return;
    // characterCount() is O(1). Qt counts the setText() string (markup included for rich text);
    // the document's characters are what the culling saves rendering of.
    const bool large = doc->characterCount() > kLargeTextThreshold;
    if (editor->flags().testFlag(QQuickItem::ItemObservesViewport) != large)
        editor->setFlag(QQuickItem::ItemObservesViewport, large);
}

// ─── outline (office item 2b, ★3) ─────────────────────────────────────────────────────────────

void RichTextHandler::rebuildHeadings()
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    const QList<HeadingEntry> before = m_headings;
    m_headings.clear();
    if (doc) {
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            const int level = b.blockFormat().headingLevel();
            if (level > 0)
                m_headings << HeadingEntry { b, level, b.text() };
        }
    }
    bool same = before.size() == m_headings.size();
    for (int i = 0; same && i < before.size(); ++i)
        same = before[i].block == m_headings[i].block && before[i].level == m_headings[i].level
            && before[i].text == m_headings[i].text;
    if (!same)
        emit headingsChanged();
}

void RichTextHandler::handleContentsChange(int position, int charsRemoved, int charsAdded)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const int last = doc->characterCount() - 1;
    const QTextBlock from = doc->findBlock(qBound(0, position, last));
    const QTextBlock to = doc->findBlock(qBound(0, position + charsAdded, last));
    const int fromNum = from.blockNumber(), toNum = to.blockNumber();
    Q_UNUSED(charsRemoved);

    // The headings the re-scanned span gives NOW (document order, since we walk from→to forward).
    QList<HeadingEntry> rescanned;
    for (QTextBlock b = from; b.isValid(); b = b.next()) {
        const int level = b.blockFormat().headingLevel();
        if (level > 0)
            rescanned << HeadingEntry { b, level, b.text() };
        if (b == to)
            break;
    }

    // Splice: keep every OLD heading outside [from, to] and not deleted; everything inside that
    // span — deleted or not — is replaced by `rescanned`, inserted where the span's old entries
    // were (they are contiguous, since m_headings is kept in document order).
    QList<HeadingEntry> spliced;
    spliced.reserve(m_headings.size() + rescanned.size());
    int insertAt = -1;
    for (const HeadingEntry &e : std::as_const(m_headings)) {
        const bool invalid = !e.block.isValid();
        const bool inSpan = !invalid && e.block.blockNumber() >= fromNum && e.block.blockNumber() <= toNum;
        if (invalid || inSpan) {
            if (insertAt < 0)
                insertAt = spliced.size();
            continue;
        }
        spliced << e;
    }
    if (insertAt < 0)
        insertAt = spliced.size();
    for (int i = 0; i < rescanned.size(); ++i)
        spliced.insert(insertAt + i, rescanned[i]);

    bool same = spliced.size() == m_headings.size();
    for (int i = 0; same && i < spliced.size(); ++i)
        same = spliced[i].block == m_headings[i].block && spliced[i].level == m_headings[i].level
            && spliced[i].text == m_headings[i].text;
    m_headings = spliced;
    if (!same)
        emit headingsChanged();
}

QVariantList RichTextHandler::headings() const
{
    QVariantList out;
    out.reserve(m_headings.size());
    for (const HeadingEntry &e : m_headings) {
        if (!e.block.isValid())
            continue; // defensive: handleContentsChange keeps this pruned
        out << QVariantMap {
            { QStringLiteral("position"), e.block.position() },
            { QStringLiteral("level"), e.block.blockFormat().headingLevel() },
            { QStringLiteral("text"), e.block.text() },
        };
    }
    return out;
}

int RichTextHandler::currentHeadingIndex() const
{
    const int pos = insertionCursor().position();
    int idx = -1;
    for (int i = 0; i < m_headings.size(); ++i) {
        if (!m_headings[i].block.isValid())
            continue;
        if (m_headings[i].block.position() > pos)
            break;
        idx = i;
    }
    return idx;
}

void RichTextHandler::setError(const QString &message)
{
    if (!message.isEmpty())
        emit ioError(message);
    if (m_error == message)
        return;
    m_error = message;
    emit errorStringChanged();
}

bool RichTextHandler::isModified() const
{
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    return doc && doc->isModified();
}

void RichTextHandler::setModified(bool m)
{
    if (QTextDocument *doc = m_document ? m_document->textDocument() : nullptr)
        doc->setModified(m); // modificationChanged → modifiedChanged
}

QString RichTextHandler::alignment() const
{
    const QTextCursor c = insertionCursor();
    if (c.isNull())
        return QStringLiteral("left");
    const Qt::Alignment a = c.blockFormat().alignment();
    if (a & Qt::AlignHCenter) return QStringLiteral("center");
    if (a & Qt::AlignJustify) return QStringLiteral("justify");
    if (a & Qt::AlignRight) return QStringLiteral("right");
    return QStringLiteral("left");
}

void RichTextHandler::setAlignment(const QString &align)
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    QTextBlockFormat bf;
    if (align == QLatin1String("center")) bf.setAlignment(Qt::AlignHCenter);
    else if (align == QLatin1String("right")) bf.setAlignment(Qt::AlignRight);
    else if (align == QLatin1String("justify")) bf.setAlignment(Qt::AlignJustify);
    else bf.setAlignment(Qt::AlignLeft);
    c.mergeBlockFormat(bf); // every block the selection touches
    emit formatChanged();
}

void RichTextHandler::newDocument()
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    // Undo off: the reset (and styling the first paragraph) leaves no history.
    doc->setUndoRedoEnabled(false);
    doc->clear(); // also drops the image resources
    m_sheet = rt::StyleSheet::builtIns();
    QTextCursor c(doc);
    c.setBlockFormat(m_sheet.blockFormat(rt::defaultParagraphStyle()));
    c.setBlockCharFormat(m_sheet.charFormat(rt::defaultParagraphStyle(), QString()));
    // m_sheet.blockFormat/charFormat() always materialize at zoom 1 (canonical units): bring the
    // fresh document up to the zoom already in effect (office item 2b, ★2), still with undo off.
    if (!qFuzzyCompare(m_zoom, 1.0))
        rescaleRange(-1, -1, m_zoom, false);
    doc->setUndoRedoEnabled(true);
    doc->setModified(false);
    rebuildHeadings(); // cheap full scan: infrequent (a new document), not a keystroke (★3)
    // clear() signals modificationChanged(true) and then resets the flag WITHOUT a signal, so
    // setModified(false) is a no-op: tell the bindings the flag is false again.
    emit modifiedChanged();
    setError(QString());
    emit stylesChanged();
    emit formatChanged();
}

QString RichTextHandler::toHtml() const
{
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    return doc ? doc->toHtml() : QString();
}

QString RichTextHandler::plainText() const
{
    const QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    // toPlainText turns paragraph separators into "\n"; line breaks (U+2028) stay — normalize them.
    return doc ? doc->toPlainText().replace(QChar::LineSeparator, QLatin1Char('\n')) : QString();
}

void RichTextHandler::insertText(const QString &text)
{
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (m_selectionStart != m_selectionEnd) {
        const int last = c.document()->characterCount() - 1;
        c.setPosition(qBound(0, m_selectionStart, last));
        c.setPosition(qBound(0, m_selectionEnd, last), QTextCursor::KeepAnchor); // typing over a selection replaces it
        c.removeSelectedText();
    }
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < paragraphs.size(); ++i) {
        if (i > 0)
            breakParagraph(c);
        if (!paragraphs[i].isEmpty())
            c.insertText(paragraphs[i]);
    }
    c.endEditBlock();
    emit formatChanged();
}

// ─── clipboard (office item 2b, item 6) ───────────────────────────────────────────────────────

void RichTextHandler::copy() const
{
    const QTextCursor c = selectionCursor();
    if (c.isNull())
        return;
    const QTextDocumentFragment sel = c.selection();
    auto *data = new QMimeData;
    data->setHtml(sel.toHtml());
    data->setText(sel.toPlainText());
    QGuiApplication::clipboard()->setMimeData(data);
}

void RichTextHandler::cut()
{
    QTextCursor c = selectionCursor();
    if (c.isNull())
        return;
    copy();
    c.removeSelectedText();
    emit formatChanged();
}

void RichTextHandler::paste()
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data || !(data->hasHtml() || data->hasText()))
        return;
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (const QTextCursor sel = selectionCursor(); !sel.isNull()) {
        c = sel;
        c.removeSelectedText();
    }
    // HTML preferred (keeps formatting); inserted at whatever size its source gives (see
    // docs/document-editor.md "Clipboard" for the zoom interaction).
    if (data->hasHtml())
        c.insertHtml(data->html());
    else
        c.insertText(data->text());
    c.endEditBlock();
    emit formatChanged();
}

bool RichTextHandler::canPaste() const
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    return data && (data->hasHtml() || data->hasText());
}

void RichTextHandler::setSelectionStart(int v)
{
    if (m_selectionStart == v)
        return;
    m_selectionStart = v;
    emit selectionChanged();
    emit formatChanged();
}

void RichTextHandler::setSelectionEnd(int v)
{
    if (m_selectionEnd == v)
        return;
    m_selectionEnd = v;
    emit selectionChanged();
    emit formatChanged();
}

void RichTextHandler::setCursorPosition(int v)
{
    if (m_cursorPosition == v)
        return;
    m_cursorPosition = v;
    emit selectionChanged();
    emit formatChanged();
}

QTextCursor RichTextHandler::cursor() const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return QTextCursor();
    QTextCursor c(doc);
    // The editor's positions reach us through bindings, a beat after the document changed under
    // them (a load, a new document): clamp so a stale value can't point past the end.
    const int last = doc->characterCount() - 1;
    if (m_selectionStart != m_selectionEnd) {
        c.setPosition(qBound(0, m_selectionStart, last));
        c.setPosition(qBound(0, m_selectionEnd, last), QTextCursor::KeepAnchor);
    } else {
        c.setPosition(qBound(0, m_cursorPosition, last));
        // Word-processor convention: with no selection, a format toggle applies to the word.
        c.select(QTextCursor::WordUnderCursor);
    }
    return c;
}

QTextCursor RichTextHandler::insertionCursor() const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return QTextCursor();
    QTextCursor c(doc);
    c.setPosition(qBound(0, m_cursorPosition, doc->characterCount() - 1));
    return c;
}

void RichTextHandler::mergeCharFormat(const QTextCharFormat &fmt)
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    c.mergeCharFormat(fmt);
    emit formatChanged();
}

bool RichTextHandler::bold() const { return cursor().charFormat().fontWeight() >= QFont::Bold; }
bool RichTextHandler::italic() const { return cursor().charFormat().fontItalic(); }
bool RichTextHandler::underline() const { return cursor().charFormat().fontUnderline(); }
bool RichTextHandler::strike() const { return cursor().charFormat().fontStrikeOut(); }

namespace {
bool isNumberedStyle(QTextListFormat::Style s)
{
    switch (s) {
    case QTextListFormat::ListDecimal: case QTextListFormat::ListLowerAlpha: case QTextListFormat::ListUpperAlpha:
    case QTextListFormat::ListLowerRoman: case QTextListFormat::ListUpperRoman:
        return true;
    default:
        return false;
    }
}
} // namespace

bool RichTextHandler::bulletList() const
{
    const QTextCursor c = cursor();
    const QTextList *l = c.isNull() ? nullptr : c.currentList();
    return l && !isNumberedStyle(l->format().style());
}

bool RichTextHandler::numberedList() const
{
    const QTextCursor c = cursor();
    const QTextList *l = c.isNull() ? nullptr : c.currentList();
    return l && isNumberedStyle(l->format().style());
}

int RichTextHandler::heading() const
{
    const QTextCursor c = cursor();
    return c.isNull() ? 0 : c.blockFormat().headingLevel();
}

QString RichTextHandler::fontFamily() const
{
    return rt::readProp(cursor().charFormat(), rt::Prop::FontFamily).toString();
}

qreal RichTextHandler::fontSize() const
{
    const QVariant v = rt::readProp(cursor().charFormat(), rt::Prop::FontSize);
    return v.isValid() ? v.toDouble() / m_zoom : 0.0; // unset: the editor's own base font stands in
}

void RichTextHandler::toggleBold()
{
    QTextCharFormat f;
    f.setFontWeight(bold() ? QFont::Normal : QFont::Bold);
    mergeCharFormat(f);
}

void RichTextHandler::toggleItalic()
{
    QTextCharFormat f;
    f.setFontItalic(!italic());
    mergeCharFormat(f);
}

void RichTextHandler::toggleUnderline()
{
    QTextCharFormat f;
    f.setFontUnderline(!underline());
    mergeCharFormat(f);
}

void RichTextHandler::toggleStrike()
{
    QTextCharFormat f;
    f.setFontStrikeOut(!strike());
    mergeCharFormat(f);
}

void RichTextHandler::toggleBulletList()
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        // Detach the block: reset its indentation back to body text. blockFormat() must be read
        // back AFTER remove() (not before) — remove() clears the block's own list association in
        // the document; a format snapshot taken earlier still carries the OLD one, and writing it
        // back would silently re-attach the block to the (possibly just-deleted) list.
        list->remove(c.block());
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(0);
        c.setBlockFormat(bf);
    } else {
        c.createList(QTextListFormat::ListDisc);
    }
    emit formatChanged();
}

void RichTextHandler::toggleNumberedList()
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        // See toggleBulletList(): blockFormat() must be read back AFTER remove().
        list->remove(c.block());
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(0);
        c.setBlockFormat(bf);
    } else {
        c.createList(QTextListFormat::ListDecimal);
    }
    emit formatChanged();
}

void RichTextHandler::setHeading(int level)
{
    applyParagraphStyle(level >= 1 && level <= 6 ? QStringLiteral("Heading %1").arg(level) : rt::textBodyStyle());
}

void RichTextHandler::setFontFamily(const QString &name)
{
    if (name.trimmed().isEmpty())
        return;
    QTextCharFormat f;
    f.setProperty(QTextFormat::FontFamilies, QStringList { name });
    mergeCharFormat(f);
}

void RichTextHandler::setFontSize(qreal pt)
{
    if (pt <= 0)
        return;
    QTextCharFormat f;
    f.setProperty(QTextFormat::FontPointSize, pt * m_zoom); // canonical in, baked in at the current zoom
    mergeCharFormat(f);
}

bool RichTextHandler::insertImage(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    const QString path = localPath(file);
    QImage img(path);
    if (img.isNull()) {
        setError(QStringLiteral("could not load image ") + path);
        return false;
    }
    // The resource name keys BOTH the render lookup and the ODF embed (the writer walks image
    // formats and pulls the resource into Pictures/).
    // A plain path becomes an absolute file:// URL: the lookup resolves names against the editor's
    // baseUrl, so a scheme-less key would never match again.
    const QUrl key = file.scheme().isEmpty() ? QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()) : file;
    doc->addResource(QTextDocument::ImageResource, key, img);
    QTextImageFormat fmt;
    fmt.setName(key.toString());
    // Word-like cap: images wider than the page column scale down, keeping aspect — at the zoom
    // in effect (★2), like every other length already in the document; the writer divides it back
    // out to the canonical 480px cap on save.
    const qreal maxWidth = 480.0 * m_zoom;
    if (img.width() > maxWidth) {
        fmt.setWidth(maxWidth);
        fmt.setHeight(img.height() * maxWidth / img.width());
    }
    insertionCursor().insertImage(fmt);
    setError(QString());
    emit formatChanged();
    return true;
}

void RichTextHandler::insertTable(int rows, int cols)
{
    QTextCursor c = insertionCursor();
    if (c.isNull() || rows < 1 || cols < 1)
        return;
    // Our ODF writer closes a list before a table, so a table may follow a list item directly.
    c.insertTable(rows, cols, rt::wordLikeTableFormat());
    emit formatChanged();
}

bool RichTextHandler::inTable() const
{
    const QTextCursor c = insertionCursor();
    return !c.isNull() && c.currentTable() != nullptr;
}

void RichTextHandler::addTableRow()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    t->insertRows(t->cellAt(c).row() + 1, 1);
    emit formatChanged();
}

void RichTextHandler::addTableColumn()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    t->insertColumns(t->cellAt(c).column() + 1, 1);
    emit formatChanged();
}

void RichTextHandler::removeTableRow()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    if (t->rows() <= 1)
        return; // removing the last row would leave a degenerate frame
    t->removeRows(t->cellAt(c).row(), 1);
    emit formatChanged();
}

void RichTextHandler::removeTableColumn()
{
    QTextCursor c = insertionCursor();
    QTextTable *t = c.isNull() ? nullptr : c.currentTable();
    if (!t)
        return;
    if (t->columns() <= 1)
        return;
    t->removeColumns(t->cellAt(c).column(), 1);
    emit formatChanged();
}

bool RichTextHandler::writeOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    // Our writer (not QTextDocumentWriter's "odf"): named styles in styles.xml, headings as text:h.
    // It reads back the live (possibly zoomed) formats in canonical units itself (★2) — m_sheet
    // stays the unscaled source of truth either way.
    QString why;
    if (!rt::writeOdt(localPath(file), doc, m_sheet, m_zoom, &why)) {
        setError(why);
        return false;
    }
    setError(QString());
    return true;
}

bool RichTextHandler::saveOdf(const QUrl &file)
{
    if (!writeOdf(file))
        return false;
    m_document->textDocument()->setModified(false);
    return true;
}

bool RichTextHandler::saveOdfCopy(const QUrl &file)
{
    return writeOdf(file);
}

bool RichTextHandler::loadOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    QString why;
    rt::StyleSheet sheet;
    const bool ok = rt::readOdt(localPath(file), doc, &sheet, &why);
    if (!ok && sheet.isEmpty()) { // not found / not an .odt: the document was not touched
        setError(why);
        return false;
    }
    m_sheet = sheet;
    // readOdt materialized at zoom 1 (canonical units): bring the document up to the zoom already
    // in effect (office item 2b, ★2). Undo is briefly disabled around it, as readOdt's own build
    // already was, so this still reads as "the load is not an undo step".
    if (!qFuzzyCompare(m_zoom, 1.0)) {
        doc->setUndoRedoEnabled(false);
        rescaleRange(-1, -1, m_zoom, false);
        doc->setUndoRedoEnabled(true);
        doc->setModified(false);
    }
    rebuildHeadings(); // cheap full scan: infrequent (a load), not a keystroke (★3)
    emit modifiedChanged(); // see newDocument: clear() can leave the last signal saying "true"
    emit stylesChanged();
    setError(why);
    emit formatChanged();
    return ok;
}

// ─── named styles ──────────────────────────────────────────────────────────────────────────────

// A style definition change inside the document's undo history: undoing the re-styled paragraphs
// also puts the old definition back (and redo the new one).
class StyleUndo : public QAbstractUndoItem {
public:
    StyleUndo(RichTextHandler *h, const QString &name, const std::optional<rt::NamedStyle> &before,
              const rt::NamedStyle &after)
        : m_handler(h), m_name(name), m_before(before), m_after(after) {}
    void undo() override { if (m_handler) m_handler->restoreStyle(m_name, m_before); }
    void redo() override { if (m_handler) m_handler->restoreStyle(m_name, m_after); }

private:
    QPointer<RichTextHandler> m_handler;
    QString m_name;
    std::optional<rt::NamedStyle> m_before;
    rt::NamedStyle m_after;
};

void RichTextHandler::restoreStyle(const QString &name, const std::optional<rt::NamedStyle> &style)
{
    if (style)
        m_sheet.put(*style);
    else
        m_sheet.remove(name);
    // Called from inside QTextDocument::undo/redo: tell the bindings once the document is whole.
    QMetaObject::invokeMethod(this, &RichTextHandler::stylesChanged, Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, &RichTextHandler::formatChanged, Qt::QueuedConnection);
}

QString RichTextHandler::defineStyle(const QString &name, const QVariantMap &props)
{
    const rt::NamedStyle *old = m_sheet.find(name);
    rt::NamedStyle s;
    if (old) {
        s = *old;
    } else {
        s.name = name;
        const QString family = props.value(QStringLiteral("family"), QStringLiteral("paragraph")).toString();
        if (family == QLatin1String("character") || family == QLatin1String("text"))
            s.family = rt::Family::Character;
        else if (family != QLatin1String("paragraph"))
            return QStringLiteral("family: expected \"paragraph\" or \"character\"");
    }
    for (auto it = props.cbegin(); it != props.cend(); ++it) {
        const QString &key = it.key();
        const QVariant &v = it.value();
        const bool unset = !v.isValid() || v.isNull();
        if (key == QLatin1String("family")) {
            const QString f = v.toString();
            const bool character = f == QLatin1String("character") || f == QLatin1String("text");
            if (old && character != (s.family == rt::Family::Character))
                return QStringLiteral("\"%1\" is a %2 style").arg(name, s.family == rt::Family::Character
                                                                    ? QStringLiteral("character") : QStringLiteral("paragraph"));
        } else if (key == QLatin1String("parent")) {
            s.parent = unset ? QString() : v.toString();
        } else if (key == QLatin1String("nextStyle")) {
            s.next = unset ? QString() : v.toString();
        } else {
            rt::Prop p;
            if (!rt::propFromName(key, &p))
                return QStringLiteral("unknown style property \"%1\"").arg(key);
            if (s.family == rt::Family::Character && !rt::isCharProp(p))
                return QStringLiteral("%1 is a paragraph property (\"%2\" is a character style)").arg(key, name);
            if (unset) {
                s.props.remove(key);
            } else {
                QVariant canonical;
                QString why;
                if (!rt::canonicalValue(p, v, &canonical, &why))
                    return why;
                s.props.insert(key, canonical);
            }
        }
    }
    if (const QString why = m_sheet.validate(s); !why.isEmpty())
        return why;
    if (old && old->parent == s.parent && old->next == s.next && old->props == s.props)
        return QString(); // nothing changes

    const rt::StyleSheet before = m_sheet;
    m_sheet.put(s);
    if (QTextDocument *doc = m_document ? m_document->textDocument() : nullptr) {
        QTextCursor edit(doc);
        edit.beginEditBlock();
        if (doc->isUndoRedoEnabled())
            doc->appendUndoItem(new StyleUndo(this, name, old ? std::optional<rt::NamedStyle>(*old) : std::nullopt,
                                              *m_sheet.find(name)));
        const auto same = [](const QString &n) { return n; };
        restyle(-1, -1, before, same, same, false, false);
        edit.endEditBlock();
        doc->setModified(true); // a definition is part of the document even when no text uses it
    }
    emit stylesChanged();
    emit contentsChanged();
    emit formatChanged();
    return QString();
}

QStringList RichTextHandler::styleNames(const QString &family) const
{
    if (family == QLatin1String("character") || family == QLatin1String("text"))
        return m_sheet.names(rt::Family::Character);
    if (family == QLatin1String("paragraph"))
        return m_sheet.names(rt::Family::Paragraph);
    return {};
}

QVariantMap RichTextHandler::styleProperties(const QString &name, bool resolved) const
{
    const rt::NamedStyle *s = m_sheet.find(name);
    if (!s)
        return {};
    QVariantMap out = resolved ? m_sheet.resolved(name) : s->props;
    const bool paragraph = s->family == rt::Family::Paragraph;
    out.insert(QStringLiteral("family"), paragraph ? QStringLiteral("paragraph") : QStringLiteral("character"));
    out.insert(QStringLiteral("parent"), s->parent);
    if (paragraph)
        out.insert(QStringLiteral("nextStyle"), s->next.isEmpty() ? s->name : s->next);
    return out;
}

void RichTextHandler::restyle(int from, int to, const rt::StyleSheet &oldSheet,
                              const std::function<QString(const QString &)> &paraMap,
                              const std::function<QString(const QString &)> &charMap, bool force, bool clipChars)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const int last = doc->characterCount() - 1;
    QTextBlock b = from < 0 ? doc->begin() : doc->findBlock(qBound(0, from, last));
    const QTextBlock endBlock = from < 0 ? QTextBlock() : doc->findBlock(qBound(0, to, last));
    QTextCursor c(doc);
    struct Change { int pos; int len; QTextCharFormat format; };
    QList<Change> changes;
    for (; b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        const QString oldPara = rt::paragraphStyleOf(bf);
        const QString newPara = paraMap(oldPara);
        const QTextBlockFormat oldBase = oldSheet.blockFormat(oldPara);
        const QTextBlockFormat newBase = m_sheet.blockFormat(newPara);
        if (force || oldBase != newBase) {
            QTextBlockFormat nbf = bf;
            rt::rebase(nbf, oldBase, newBase, true, force);
            nbf.setProperty(rt::kParagraphStyleProperty, newPara);
            if (nbf != bf) {
                c.setPosition(b.position());
                c.setBlockFormat(nbf);
            }
        }
        const auto restyleChars = [&](const QTextCharFormat &f, QTextCharFormat *out) {
            const QString oldCs = rt::charStyleOf(f);
            const QString newCs = charMap(oldCs);
            const QTextCharFormat oldB = oldSheet.charFormat(oldPara, oldCs);
            const QTextCharFormat newB = m_sheet.charFormat(newPara, newCs);
            if (!force && oldB == newB)
                return false;
            *out = f;
            rt::rebase(*out, oldB, newB, false, force);
            if (newCs.isEmpty()) out->clearProperty(rt::kCharStyleProperty);
            else out->setProperty(rt::kCharStyleProperty, newCs);
            return *out != f;
        };
        if (!clipChars) { // what is typed into the paragraph when it is empty
            const QTextCharFormat bcf = b.charFormat();
            QTextCharFormat nbcf;
            if (restyleChars(bcf, &nbcf)) {
                c.setPosition(b.position());
                c.setBlockCharFormat(nbcf);
            }
        }
        changes.clear();
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.charFormat().isImageFormat())
                continue;
            int s = frag.position(), e = frag.position() + frag.length();
            if (clipChars) {
                s = qMax(s, from);
                e = qMin(e, to);
                if (s >= e)
                    continue;
            }
            QTextCharFormat nf;
            if (restyleChars(frag.charFormat(), &nf))
                changes << Change { s, e - s, nf };
        }
        for (const Change &ch : std::as_const(changes)) {
            c.setPosition(ch.pos);
            c.setPosition(ch.pos + ch.len, QTextCursor::KeepAnchor);
            c.setCharFormat(ch.format);
        }
        if (from >= 0 && b == endBlock)
            break;
    }
    // rebase() above always takes raw values from m_sheet (canonical, zoom-1 units): bring what
    // was just touched back up to the zoom in effect (office item 2b, ★2). Every restyle() caller
    // already runs inside its own edit block, so this stays part of the same undo step. Mirrors
    // clipChars exactly: a whole-block operation (false) rescales every run of the touched
    // block(s); a selection-scoped one (true) only the runs inside [from, to] — the others were
    // never touched by rebase() above and are already at the right zoom.
    if (!qFuzzyCompare(m_zoom, 1.0))
        rescaleRange(from, to, m_zoom, clipChars);
}

void RichTextHandler::rescaleRange(int from, int to, qreal ratio, bool clipChars)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc || qFuzzyCompare(ratio, 1.0))
        return;
    QTextCursor c(doc);
    const auto scaleBlock = [&](const QTextBlock &b) {
        if (from >= 0 && (b.position() + b.length() <= from || b.position() > to))
            return;
        c.setPosition(b.position());
        c.setBlockFormat(rt::scaledLengths(b.blockFormat(), ratio));
        if (!clipChars)
            c.setBlockCharFormat(rt::scaledLengths(b.charFormat(), ratio));
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            int s = frag.position(), e = s + frag.length();
            if (clipChars) {
                s = qMax(s, from);
                e = qMin(e, to);
                if (s >= e)
                    continue;
            }
            c.setPosition(s);
            c.setPosition(e, QTextCursor::KeepAnchor);
            c.setCharFormat(rt::scaledLengths(frag.charFormat(), ratio));
        }
    };
    const std::function<void(QTextFrame::iterator, const QTextFrame::iterator &)> walk =
        [&](QTextFrame::iterator it, const QTextFrame::iterator &end) {
            for (; it != end; ++it) {
                if (QTextFrame *f = it.currentFrame())
                    walk(f->begin(), f->end());
                else if (const QTextBlock b = it.currentBlock(); b.isValid())
                    scaleBlock(b);
            }
        };
    walk(doc->rootFrame()->begin(), doc->rootFrame()->end());
}

void RichTextHandler::setZoom(qreal z)
{
    z = qBound(0.5, z, 3.0);
    if (qFuzzyCompare(z, m_zoom))
        return;
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    const qreal ratio = z / m_zoom;
    if (doc) {
        QTextCursor edit(doc);
        edit.beginEditBlock(); // one undo step, like any other formatting change
        rescaleRange(-1, -1, ratio, false);
        edit.endEditBlock();
    }
    m_zoom = z;
    emit zoomChanged();
}

QTextCursor RichTextHandler::selectionCursor() const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc || m_selectionStart == m_selectionEnd)
        return QTextCursor();
    const int last = doc->characterCount() - 1;
    QTextCursor c(doc);
    c.setPosition(qBound(0, m_selectionStart, last));
    c.setPosition(qBound(0, m_selectionEnd, last), QTextCursor::KeepAnchor);
    return c;
}

bool RichTextHandler::applyParagraphStyle(const QString &name)
{
    const rt::NamedStyle *s = m_sheet.find(name);
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!s || s->family != rt::Family::Paragraph || !doc)
        return false;
    const QTextCursor sel = selectionCursor();
    const QTextCursor at = insertionCursor();
    const int from = sel.isNull() ? at.position() : sel.selectionStart();
    const int to = sel.isNull() ? at.position() : sel.selectionEnd();
    QTextCursor edit(doc);
    edit.beginEditBlock();
    restyle(from, to, m_sheet, [&](const QString &) { return name; }, [](const QString &n) { return n; }, false, false);
    edit.endEditBlock();
    emit formatChanged();
    return true;
}

bool RichTextHandler::applyCharStyle(const QString &name)
{
    const rt::NamedStyle *s = name.isEmpty() ? nullptr : m_sheet.find(name);
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if ((!name.isEmpty() && (!s || s->family != rt::Family::Character)) || !doc)
        return false;
    const QTextCursor c = cursor(); // the selection, or the word at the caret
    if (c.hasSelection()) {
        QTextCursor edit(doc);
        edit.beginEditBlock();
        restyle(c.selectionStart(), c.selectionEnd(), m_sheet, [](const QString &n) { return n; },
                [&](const QString &) { return name; }, false, true);
        edit.endEditBlock();
    }
    emit formatChanged();
    return true;
}

QString RichTextHandler::currentParagraphStyle() const
{
    const QTextCursor c = insertionCursor();
    return c.isNull() ? rt::defaultParagraphStyle() : rt::paragraphStyleOf(c.block());
}

QString RichTextHandler::currentCharStyle() const
{
    const QTextCursor c = cursor();
    return c.isNull() ? QString() : rt::charStyleOf(c.charFormat());
}

void RichTextHandler::clearDirectFormatting()
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const QTextCursor sel = selectionCursor();
    const QTextBlock block = insertionCursor().block();
    const int from = sel.isNull() ? block.position() : sel.selectionStart();
    const int to = sel.isNull() ? block.position() + block.length() - 1 : sel.selectionEnd();
    const auto same = [](const QString &n) { return n; };
    QTextCursor edit(doc);
    edit.beginEditBlock();
    restyle(from, to, m_sheet, same, same, true, !sel.isNull());
    edit.endEditBlock();
    emit formatChanged();
}

void RichTextHandler::clearFormatting()
{
    // Office item 2b (★4): character properties only — restyle()'s `force` can't be split into
    // block/char halves, so this walks the runs itself and replaces each one with exactly what
    // its (kept) character style gives, leaving the block/paragraph format untouched.
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const QTextCursor sel = selectionCursor();
    const QTextBlock block = insertionCursor().block();
    const int from = sel.isNull() ? block.position() : sel.selectionStart();
    const int to = sel.isNull() ? block.position() + block.length() - 1 : sel.selectionEnd();
    const int last = doc->characterCount() - 1;
    QTextCursor edit(doc);
    edit.beginEditBlock();
    QTextCursor c(doc);
    QTextBlock b = doc->findBlock(qBound(0, from, last));
    const QTextBlock endBlock = doc->findBlock(qBound(0, to, last));
    for (; b.isValid(); b = b.next()) {
        const QString para = rt::paragraphStyleOf(b);
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.charFormat().isImageFormat())
                continue;
            const int s = qMax(frag.position(), from);
            const int e = qMin(frag.position() + frag.length(), to);
            if (s >= e)
                continue;
            const QString cs = rt::charStyleOf(frag.charFormat());
            c.setPosition(s);
            c.setPosition(e, QTextCursor::KeepAnchor);
            c.setCharFormat(m_sheet.charFormat(para, cs)); // the style chain exactly — keeps cs, drops direct overrides
        }
        if (b == endBlock)
            break;
    }
    edit.endEditBlock();
    // m_sheet.charFormat() above materializes at zoom 1 (canonical units), like restyle() does.
    if (!qFuzzyCompare(m_zoom, 1.0))
        rescaleRange(from, to, m_zoom, true);
    emit formatChanged();
}

QVariantMap RichTextHandler::directFormattingAt(int pos) const
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return {};
    pos = qBound(0, pos, doc->characterCount() - 1);
    const QTextBlock b = doc->findBlock(pos);
    const QString para = rt::paragraphStyleOf(b);
    QTextCursor c(doc);
    // charFormat() is the format of the character BEFORE the cursor: step over the one at pos.
    c.setPosition(pos < b.position() + b.length() - 1 ? pos + 1 : pos);
    const QTextCharFormat f = c.charFormat();
    const QString cs = rt::charStyleOf(f);
    return {
        { QStringLiteral("paragraphStyle"), para },
        { QStringLiteral("charStyle"), cs },
        { QStringLiteral("paragraph"), rt::directProps(b.blockFormat(), m_sheet.blockFormat(para), true) },
        { QStringLiteral("character"), rt::directProps(f, m_sheet.charFormat(para, cs), false) },
    };
}

void RichTextHandler::breakParagraph(QTextCursor &c)
{
    const QTextBlock b = c.block();
    const bool atEnd = c.atBlockEnd();
    const QString style = rt::paragraphStyleOf(b);
    QString next = style;
    if (const rt::NamedStyle *s = m_sheet.find(style); s && !s->next.isEmpty() && m_sheet.find(s->next))
        next = s->next;
    c.insertBlock(); // the new paragraph starts as a copy of this one (style, list, direct formatting)
    if (atEnd && next != style) {
        const int pos = c.position();
        restyle(pos, pos, m_sheet, [&](const QString &) { return next; }, [](const QString &n) { return n; }, false, false);
        c.setCharFormat(c.block().charFormat()); // type in the new style
    }
}

void RichTextHandler::insertParagraph()
{
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (const QTextCursor sel = selectionCursor(); !sel.isNull()) {
        c = sel;
        c.removeSelectedText();
    }
    breakParagraph(c);
    c.endEditBlock();
    emit formatChanged();
}

// ─── paragraph (office item 2b, item 5) ───────────────────────────────────────────────────────

void RichTextHandler::indent()
{
    // 24px (~0.25in), the common word-processor step; at the zoom in effect, like setFontSize().
    constexpr qreal kIndentStep = 24.0;
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        QTextListFormat nf = list->format();
        nf.setIndent(nf.indent() + 1);
        list->remove(c.block());
        c.createList(nf); // a new (or implicitly shared, by Qt) QTextList at the deeper level
    } else {
        QTextBlockFormat bf = c.blockFormat();
        bf.setLeftMargin(bf.leftMargin() + kIndentStep * m_zoom);
        c.setBlockFormat(bf);
    }
    emit formatChanged();
}

void RichTextHandler::outdent()
{
    constexpr qreal kIndentStep = 24.0;
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    if (QTextList *list = c.currentList()) {
        QTextListFormat nf = list->format();
        const int newIndent = nf.indent() - 1;
        list->remove(c.block()); // see toggleBulletList(): blockFormat() must be read back AFTER this
        if (newIndent <= 0) {
            QTextBlockFormat bf = c.blockFormat();
            bf.setIndent(0);
            c.setBlockFormat(bf);
        } else {
            nf.setIndent(newIndent);
            c.createList(nf);
        }
    } else {
        QTextBlockFormat bf = c.blockFormat();
        bf.setLeftMargin(qMax(0.0, bf.leftMargin() - kIndentStep * m_zoom));
        c.setBlockFormat(bf);
    }
    emit formatChanged();
}

void RichTextHandler::insertPageBreak()
{
    QTextCursor c = insertionCursor();
    if (c.isNull())
        return;
    c.beginEditBlock();
    if (const QTextCursor sel = selectionCursor(); !sel.isNull()) {
        c = sel;
        c.removeSelectedText();
    }
    breakParagraph(c);
    QTextBlockFormat bf = c.blockFormat();
    bf.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
    c.setBlockFormat(bf);
    c.endEditBlock();
    emit formatChanged();
}
