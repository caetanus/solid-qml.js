#include "richtext.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QQuickItem>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QXmlStreamReader>

#include <private/qzipreader_p.h> // Qt ships no public unzip; the reader IS how Qt reads .odt themes

void RichTextHandler::setDocument(QQuickTextDocument *doc)
{
    if (m_document == doc)
        return;
    QObject::disconnect(m_modifiedConn);
    QObject::disconnect(m_contentsConn);
    QObject::disconnect(m_cullingConn);
    m_document = doc;
    if (QTextDocument *td = doc ? doc->textDocument() : nullptr) {
        m_modifiedConn = connect(td, &QTextDocument::modificationChanged, this, &RichTextHandler::modifiedChanged);
        m_contentsConn = connect(td, &QTextDocument::contentsChanged, this, &RichTextHandler::contentsChanged);
        // Every edit can cross the threshold (a load, a paste, a select-all + delete). A load is
        // one edit block, so this runs once per load, not once per paragraph.
        m_cullingConn = connect(td, &QTextDocument::contentsChanged, this, &RichTextHandler::updateViewportCulling);
    }
    updateViewportCulling();
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
    doc->clear(); // also drops the undo stack and the image resources
    doc->setModified(false);
    // clear() signals modificationChanged(true) and then resets the flag WITHOUT a signal, so
    // setModified(false) is a no-op: tell the bindings the flag is false again.
    emit modifiedChanged();
    setError(QString());
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
    if (m_selectionStart != m_selectionEnd) {
        const int last = c.document()->characterCount() - 1;
        c.setPosition(qBound(0, m_selectionStart, last));
        c.setPosition(qBound(0, m_selectionEnd, last), QTextCursor::KeepAnchor); // typing over a selection replaces it
    }
    c.insertText(text);
    emit formatChanged();
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

bool RichTextHandler::bulletList() const
{
    const QTextCursor c = cursor();
    return !c.isNull() && c.currentList() != nullptr;
}

int RichTextHandler::heading() const
{
    const QTextCursor c = cursor();
    return c.isNull() ? 0 : c.blockFormat().headingLevel();
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
        // Detach the block: reset its indentation back to body text.
        QTextBlockFormat bf = c.blockFormat();
        list->remove(c.block());
        bf.setIndent(0);
        c.setBlockFormat(bf);
    } else {
        c.createList(QTextListFormat::ListDisc);
    }
    emit formatChanged();
}

void RichTextHandler::setHeading(int level)
{
    QTextCursor c = cursor();
    if (c.isNull())
        return;
    QTextBlockFormat bf = c.blockFormat();
    bf.setHeadingLevel(level);
    c.setBlockFormat(bf);
    // Headings carry their size on the char format of the whole block (like Qt's own editors).
    const QTextBlock block = c.block();
    if (level > 0) {
        QTextCursor b(block);
        b.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        QTextCharFormat cf;
        cf.setFontWeight(QFont::Bold);
        static const qreal sizes[] = { 0.0, 20.0, 17.0, 15.0 };
        if (level <= 3)
            cf.setFontPointSize(sizes[level]);
        b.mergeCharFormat(cf);
        b.mergeBlockCharFormat(cf); // what is typed into an empty heading
    } else {
        // Back to body text: a merge can't REMOVE a property, so rewrite each run without the
        // heading's size and weight (italic/underline/… stay).
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            QTextCharFormat f = frag.charFormat();
            f.clearProperty(QTextFormat::FontPointSize);
            f.clearProperty(QTextFormat::FontWeight);
            QTextCursor r(c.document());
            r.setPosition(frag.position());
            r.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            r.setCharFormat(f);
        }
        QTextCharFormat bcf = QTextCursor(block).blockCharFormat();
        bcf.clearProperty(QTextFormat::FontPointSize);
        bcf.clearProperty(QTextFormat::FontWeight);
        QTextCursor(block).setBlockCharFormat(bcf);
    }
    emit formatChanged();
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
    // Word-like cap: images wider than the page column scale down, keeping aspect.
    constexpr qreal kMaxWidth = 480.0;
    if (img.width() > kMaxWidth) {
        fmt.setWidth(kMaxWidth);
        fmt.setHeight(img.height() * kMaxWidth / img.width());
    }
    insertionCursor().insertImage(fmt);
    setError(QString());
    emit formatChanged();
    return true;
}

namespace {

// The table look shared by insertTable and the ODF reader.
QTextTableFormat wordLikeTableFormat()
{
    QTextTableFormat f;
    f.setBorder(1);
    f.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    f.setBorderCollapse(true);
    f.setCellPadding(4);
    f.setCellSpacing(0);
    f.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    return f;
}

// A table at `c`, never right after a list item. Qt's ODF writer keeps a list open until a block
// outside it, so a table that directly follows an item is written INSIDE <text:list>, with cells
// outside their rows — a file no reader recovers (a 300-page book's reload crashed on it). A plain
// (empty) paragraph between the list and the table closes the list. Our own ODF writer (office
// item 2, named styles) removes the need for it.
QTextTable *insertTableAt(QTextCursor &c, int rows, int cols)
{
    if (c.currentList())
        c.insertBlock(QTextBlockFormat(), c.charFormat());
    return c.insertTable(rows, cols, wordLikeTableFormat());
}

} // namespace

void RichTextHandler::insertTable(int rows, int cols)
{
    QTextCursor c = insertionCursor();
    if (c.isNull() || rows < 1 || cols < 1)
        return;
    insertTableAt(c, rows, cols);
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
    // Qt's NATIVE OpenDocument writer (it only reads the document).
    const QString path = localPath(file);
    QTextDocumentWriter writer(path);
    writer.setFormat("odf");
    if (!writer.write(doc)) {
        const QString why = writer.device() ? writer.device()->errorString() : QString();
        setError(QStringLiteral("could not write ") + path + (why.isEmpty() ? QString() : QStringLiteral(": ") + why));
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

// ─── minimal ODF reader (Qt ships none): content.xml → QTextDocument ───────────────────────────

namespace {

// ODF lengths ("3.5in", "2.4cm", "12pt", …) → CSS pixels (96 dpi, Qt's rich-text unit). NaN for
// anything that is not an absolute length (a percentage, garbage).
qreal odfLengthPx(QStringView v)
{
    if (v.isEmpty())
        return qQNaN();
    int i = 0;
    while (i < v.size() && (v.at(i).isDigit() || v.at(i) == QLatin1Char('.') || v.at(i) == QLatin1Char('-')))
        ++i;
    bool ok = false;
    const qreal n = v.left(i).toDouble(&ok);
    if (!ok)
        return qQNaN();
    const QStringView unit = v.mid(i);
    if (unit == QLatin1String("in")) return n * 96.0;
    if (unit == QLatin1String("cm")) return n * 96.0 / 2.54;
    if (unit == QLatin1String("mm")) return n * 96.0 / 25.4;
    if (unit == QLatin1String("pt")) return n * 96.0 / 72.0;
    if (unit.isEmpty() || unit == QLatin1String("px")) return n;
    return qQNaN();
}

// A paragraph property an ODF style may or may not declare: unset ones inherit from the parent
// style (style:parent-style-name), then from the paragraph default-style.
template <typename T>
struct Inherited {
    T value {};
    bool set = false;
    void operator=(T v) { value = v; set = true; }
    void inherit(const Inherited &parent) { if (!set && parent.set) *this = parent.value; }
};

struct OdfStyle {
    QString parent;               // style:parent-style-name
    // Text properties — from the automatic style itself (not inherited: a named style's font/size
    // would override the editor's font for the whole document; named styles are office item 2).
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    qreal pointSize = -1;         // fo:font-size in pt (Qt's writer keeps a heading's size there)
    // Paragraph properties — inherited along the parent chain (Text_20_body → Standard → default).
    Inherited<Qt::Alignment> align;   // fo:text-align
    Inherited<qreal> marginTop, marginBottom, marginLeft, marginRight, textIndent; // px
};

using OdfStyles = QHash<QString, OdfStyle>;

// The key the paragraph <style:default-style> is stored under (not a valid style:name).
const QString kDefaultParagraphStyle = QStringLiteral("\x01default");

// style:style / style:default-style elements up to the end of `section` (office:styles in
// styles.xml, office:automatic-styles in content.xml), with their text and paragraph properties.
void readStyles(QXmlStreamReader &xml, QLatin1String section, OdfStyles &styles)
{
    QString current;
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::EndElement && xml.name() == section)
            break;
        if (tok == QXmlStreamReader::EndElement
            && (xml.name() == QLatin1String("style") || xml.name() == QLatin1String("default-style"))) {
            current.clear();
            continue;
        }
        if (tok != QXmlStreamReader::StartElement)
            continue;
        const auto a = xml.attributes();
        if (xml.name() == QLatin1String("style")) {
            current = a.value(QLatin1String("style:name")).toString();
            if (!current.isEmpty())
                styles[current].parent = a.value(QLatin1String("style:parent-style-name")).toString();
        } else if (xml.name() == QLatin1String("default-style")) {
            if (a.value(QLatin1String("style:family")) == QLatin1String("paragraph"))
                current = kDefaultParagraphStyle;
        } else if (xml.name() == QLatin1String("text-properties") && !current.isEmpty()) {
            OdfStyle &s = styles[current];
            s.bold = a.value(QLatin1String("fo:font-weight")) == QLatin1String("bold");
            s.italic = a.value(QLatin1String("fo:font-style")) == QLatin1String("italic");
            s.underline = a.value(QLatin1String("style:text-underline-style")) == QLatin1String("solid");
            s.strike = a.value(QLatin1String("style:text-line-through-style")) == QLatin1String("solid");
            const QStringView size = a.value(QLatin1String("fo:font-size"));
            if (size.endsWith(QLatin1String("pt")))
                s.pointSize = size.chopped(2).toDouble();
        } else if (xml.name() == QLatin1String("paragraph-properties") && !current.isEmpty()) {
            OdfStyle &s = styles[current];
            const QStringView al = a.value(QLatin1String("fo:text-align"));
            if (al == QLatin1String("center")) s.align = Qt::AlignHCenter;
            else if (al == QLatin1String("end") || al == QLatin1String("right")) s.align = Qt::AlignRight;
            else if (al == QLatin1String("justify")) s.align = Qt::AlignJustify;
            else if (!al.isEmpty()) s.align = Qt::AlignLeft;
            const auto length = [&](const char *attr, Inherited<qreal> &into) {
                const qreal px = odfLengthPx(a.value(QLatin1String(attr)));
                if (!qIsNaN(px))
                    into = px;
            };
            // fo:margin is the shorthand the four sides start from.
            const qreal all = odfLengthPx(a.value(QLatin1String("fo:margin")));
            if (!qIsNaN(all)) {
                s.marginTop = all; s.marginBottom = all; s.marginLeft = all; s.marginRight = all;
            }
            length("fo:margin-top", s.marginTop);
            length("fo:margin-bottom", s.marginBottom);
            length("fo:margin-left", s.marginLeft);
            length("fo:margin-right", s.marginRight);
            length("fo:text-indent", s.textIndent);
        }
    }
}

// `name` with its paragraph properties resolved along the parent chain (memoized in `resolved`).
const OdfStyle &resolveStyle(const QString &name, const OdfStyles &styles, OdfStyles &resolved)
{
    if (auto it = resolved.constFind(name); it != resolved.cend())
        return *it;
    OdfStyle s = styles.value(name);
    QString parent = s.parent;
    for (int depth = 0; !parent.isEmpty() && depth < 32; ++depth) { // a cycle is a broken file: stop
        const auto it = styles.constFind(parent);
        if (it == styles.cend())
            break;
        s.align.inherit(it->align);
        s.marginTop.inherit(it->marginTop);
        s.marginBottom.inherit(it->marginBottom);
        s.marginLeft.inherit(it->marginLeft);
        s.marginRight.inherit(it->marginRight);
        s.textIndent.inherit(it->textIndent);
        parent = it->parent;
    }
    if (const auto it = styles.constFind(kDefaultParagraphStyle); it != styles.cend()) {
        s.align.inherit(it->align);
        s.marginTop.inherit(it->marginTop);
        s.marginBottom.inherit(it->marginBottom);
        s.marginLeft.inherit(it->marginLeft);
        s.marginRight.inherit(it->marginRight);
        s.textIndent.inherit(it->textIndent);
    }
    return *resolved.insert(name, s);
}

QTextCharFormat formatFor(const OdfStyle &s)
{
    QTextCharFormat f;
    if (s.bold) f.setFontWeight(QFont::Bold);
    f.setFontItalic(s.italic);
    f.setFontUnderline(s.underline);
    f.setFontStrikeOut(s.strike);
    if (s.pointSize > 0) f.setFontPointSize(s.pointSize);
    return f;
}

QTextBlockFormat blockFormatFor(const OdfStyle &s)
{
    QTextBlockFormat f;
    if (s.align.set && s.align.value) f.setAlignment(s.align.value);
    if (s.marginLeft.set) f.setLeftMargin(s.marginLeft.value);
    if (s.marginRight.set) f.setRightMargin(s.marginRight.value);
    if (s.textIndent.set) f.setTextIndent(s.textIndent.value);
    const qreal top = s.marginTop.set ? s.marginTop.value : 0;
    const qreal bottom = s.marginBottom.set ? s.marginBottom.value : 0;
    if (top <= 0 && bottom <= 0) {
        // No vertical spacing at all (Standard, tight lists, table cells, footnotes): give the
        // paragraph RichTextHandler::kMinParagraphSpacing below it. A run of zero-margin blocks
        // before a table makes QQuickTextEdit's viewport culling grow its rendered region over
        // most of the document on every keystroke (qquicktextedit.cpp, updatePaintNode:
        // viewport.united(renderedRegion) while table frames are pending) — ~3x over-draw.
        f.setBottomMargin(RichTextHandler::kMinParagraphSpacing);
        if (top < 0) f.setTopMargin(top);
    } else {
        f.setTopMargin(top);
        f.setBottomMargin(bottom);
    }
    return f;
}

} // namespace

bool RichTextHandler::loadOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    const QString path = localPath(file);
    if (!QFile::exists(path)) {
        setError(QStringLiteral("file not found: ") + path);
        return false;
    }
    QZipReader zip(path);
    const QByteArray content = zip.fileData(QStringLiteral("content.xml"));
    if (content.isEmpty()) {
        setError(QStringLiteral("not an OpenDocument file: ") + path);
        return false;
    }

    // Named paragraph styles (styles.xml: Standard, Text_20_body, Heading, …) first: the automatic
    // styles in content.xml and the paragraphs themselves inherit their spacing from them.
    OdfStyles styles;
    {
        QXmlStreamReader sx(zip.fileData(QStringLiteral("styles.xml")));
        while (!sx.atEnd()) {
            if (sx.readNext() == QXmlStreamReader::StartElement && sx.name() == QLatin1String("styles"))
                readStyles(sx, QLatin1String("styles"), styles);
        }
    }
    OdfStyles resolved;
    const auto style = [&](QStringView name) -> const OdfStyle & {
        return resolveStyle(name.toString(), styles, resolved);
    };

    QXmlStreamReader xml(content);

    // Opening a file is not an undoable edit: with undo off, clear() + the build leave no history
    // (re-enabling it starts an empty stack), so Ctrl+Z can't empty the freshly opened document.
    doc->setUndoRedoEnabled(false);
    doc->clear();
    QTextCursor c(doc);
    c.beginEditBlock();
    bool firstBlock = true;
    bool inParagraph = false;
    int listDepth = 0;

    // Table state (word-like subset: flat tables, spans/images inside cells). Column count comes
    // from the table:table-column declarations that precede the rows; the table is created on the
    // first row and grown with appendRows so the streaming parse never needs a second pass.
    QTextTable *curTable = nullptr;
    int declaredCols = 0;
    int tRow = -1;
    int tCol = -1;
    QTextCursor cellCursor;
    bool inCell = false;
    int nestedTables = 0;
    bool cellFirstBlock = false;

    // Pending inline image (draw:frame carries the size, the nested draw:image the href; the
    // insert happens when the frame closes so both are known).
    QString imgHref;
    qreal imgW = -1, imgH = -1;

    // The cursor content lands on: the current cell inside a table, the document elsewhere.
    const auto act = [&]() -> QTextCursor & { return inCell ? cellCursor : c; };

    const auto beginBlock = [&](const QTextBlockFormat &bf) {
        if (inCell) {
            // Every cell is born with one empty block — reuse it for the first paragraph.
            if (cellFirstBlock) {
                cellCursor.setBlockFormat(bf);
                cellFirstBlock = false;
            } else {
                cellCursor.insertBlock(bf, QTextCharFormat());
            }
            return;
        }
        if (firstBlock) {
            c.setBlockFormat(bf);
            firstBlock = false;
        } else {
            c.insertBlock(bf, QTextCharFormat());
        }
        if (listDepth > 0)
            c.createList(QTextListFormat::ListDisc);
    };

    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const auto name = xml.name();
            if (name == QLatin1String("automatic-styles")) {
                readStyles(xml, QLatin1String("automatic-styles"), styles);
                resolved.clear();
            } else if (name == QLatin1String("list")) {
                ++listDepth;
            } else if (name == QLatin1String("h")) {
                inParagraph = true;
                const int level = xml.attributes().value(QLatin1String("text:outline-level")).toString().toInt();
                QTextBlockFormat bf = blockFormatFor(style(xml.attributes().value(QLatin1String("text:style-name"))));
                bf.setHeadingLevel(qBound(1, level ? level : 1, 6));
                beginBlock(bf);
                QTextCharFormat cf;
                cf.setFontWeight(QFont::Bold);
                static const qreal sizes[] = { 0.0, 20.0, 17.0, 15.0 };
                if (level >= 1 && level <= 3)
                    cf.setFontPointSize(sizes[level]);
                act().setCharFormat(cf);
            } else if (name == QLatin1String("p")) {
                inParagraph = true;
                const OdfStyle &st = style(xml.attributes().value(QLatin1String("text:style-name")));
                beginBlock(blockFormatFor(st));
                act().setCharFormat(formatFor(st));
            } else if (name == QLatin1String("span")) {
                act().setCharFormat(formatFor(style(xml.attributes().value(QLatin1String("text:style-name")))));
            } else if (name == QLatin1String("tab")) {
                act().insertText(QStringLiteral("\t"));
            } else if (name == QLatin1String("line-break")) {
                act().insertText(QString(QChar::LineSeparator));
            } else if (name == QLatin1String("s")) {
                int n = xml.attributes().value(QLatin1String("text:c")).toString().toInt();
                act().insertText(QString(qMax(1, n), QLatin1Char(' ')));
            } else if (name == QLatin1String("table")) {
                // A table inside a cell is not supported: its cells' text flows into the outer cell.
                if (inCell || nestedTables > 0) {
                    ++nestedTables;
                } else {
                    curTable = nullptr;
                    declaredCols = 0;
                    tRow = -1;
                }
            } else if (nestedTables > 0 && name.startsWith(QLatin1String("table-"))) {
                // a nested table's columns/rows/cells: its paragraphs land in the outer cell
            } else if (name == QLatin1String("table-column")) {
                const int rep = xml.attributes().value(QLatin1String("table:number-columns-repeated")).toString().toInt();
                declaredCols += qMax(1, rep);
            } else if (name == QLatin1String("table-row")) {
                if (!curTable) {
                    curTable = insertTableAt(c, 1, qMax(1, declaredCols));
                    tRow = 0;
                } else {
                    curTable->appendRows(1);
                    ++tRow;
                }
                tCol = -1;
            } else if (name == QLatin1String("table-cell")) {
                // Only a cell of the current row: a mis-nested file (a cell outside any row) is
                // read as plain paragraphs rather than trusted.
                const QTextTableCell cell = curTable && tRow >= 0 && tRow < curTable->rows()
                    ? curTable->cellAt(tRow, qMin(++tCol, curTable->columns() - 1)) : QTextTableCell();
                if (cell.isValid()) {
                    cellCursor = cell.firstCursorPosition();
                    inCell = true;
                    cellFirstBlock = true;
                }
            } else if (name == QLatin1String("frame")) {
                const auto a = xml.attributes();
                imgW = odfLengthPx(a.value(QLatin1String("svg:width")));
                imgH = odfLengthPx(a.value(QLatin1String("svg:height")));
                imgHref.clear();
            } else if (name == QLatin1String("image")) {
                imgHref = xml.attributes().value(QLatin1String("xlink:href")).toString();
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            const auto name = xml.name();
            if (name == QLatin1String("list")) {
                --listDepth;
            } else if (name == QLatin1String("p") || name == QLatin1String("h")) {
                inParagraph = false;
            } else if (name == QLatin1String("span")) {
                act().setCharFormat(QTextCharFormat());
            } else if (name == QLatin1String("table") && nestedTables > 0) {
                --nestedTables;
            } else if (nestedTables > 0 && name.startsWith(QLatin1String("table-"))) {
                // a nested table's row/cell end: the outer cell stays open
            } else if (name == QLatin1String("table-cell")) {
                inCell = false;
            } else if (name == QLatin1String("table")) {
                if (curTable) {
                    // Continue after the table frame, reusing the trailing block Qt keeps there.
                    c.setPosition(curTable->lastPosition() + 1);
                    firstBlock = true;
                    curTable = nullptr;
                }
            } else if (name == QLatin1String("frame")) {
                if (!imgHref.isEmpty()) {
                    // Pull the embedded picture out of the archive into a document resource. The
                    // name must be an ABSOLUTE opaque URL: resource lookup goes through
                    // baseUrl.resolved(name), and TextEdit sets baseUrl to the QML dir — a
                    // relative "Pictures/x" key would never match again. QTextDocumentWriter
                    // re-embeds the resource on save regardless of the scheme.
                    const QImage img = QImage::fromData(zip.fileData(imgHref));
                    if (!img.isNull()) {
                        const QUrl resUrl(QStringLiteral("odf:/") + imgHref);
                        doc->addResource(QTextDocument::ImageResource, resUrl, img);
                        QTextImageFormat f;
                        f.setName(resUrl.toString());
                        if (imgW > 0) f.setWidth(imgW);
                        if (imgH > 0) f.setHeight(imgH);
                        act().insertImage(f);
                    }
                    imgHref.clear();
                }
            }
        } else if (tok == QXmlStreamReader::Characters) {
            // Whitespace between spans IS content inside a paragraph (a single significant
            // space); outside paragraphs it is XML formatting noise.
            if (inParagraph)
                act().insertText(xml.text().toString());
        }
    }
    c.endEditBlock();
    doc->setUndoRedoEnabled(true);
    doc->setModified(false);
    emit modifiedChanged(); // see newDocument: clear() can leave the last signal saying "true"

    if (xml.hasError()) {
        setError(QStringLiteral("ODF parse error: ") + xml.errorString());
        return false;
    }
    setError(QString());
    emit formatChanged();
    return true;
}
