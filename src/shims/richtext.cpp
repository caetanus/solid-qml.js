#include "richtext.h"

#include <QFile>
#include <QImage>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QXmlStreamReader>

#include <private/qzipreader_p.h> // Qt ships no public unzip; the reader IS how Qt reads .odt themes

void RichTextHandler::setDocument(QQuickTextDocument *doc)
{
    if (m_document == doc)
        return;
    m_document = doc;
    emit documentChanged();
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
    if (m_selectionStart != m_selectionEnd) {
        c.setPosition(m_selectionStart);
        c.setPosition(m_selectionEnd, QTextCursor::KeepAnchor);
    } else {
        c.setPosition(m_cursorPosition);
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
    c.setPosition(m_cursorPosition);
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
    c.select(QTextCursor::BlockUnderCursor);
    QTextCharFormat cf;
    cf.setFontWeight(level > 0 ? QFont::Bold : QFont::Normal);
    static const qreal sizes[] = { 0.0, 20.0, 17.0, 15.0 };
    if (level >= 1 && level <= 3)
        cf.setFontPointSize(sizes[level]);
    else
        cf.clearProperty(QTextFormat::FontPointSize);
    c.mergeCharFormat(cf);
    emit formatChanged();
}

void RichTextHandler::insertImage(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return;
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QImage img(path);
    if (img.isNull()) {
        emit ioError(QStringLiteral("could not load image ") + path);
        return;
    }
    // The resource name keys BOTH the render lookup and the ODF embed (the writer walks image
    // formats and pulls the resource into Pictures/).
    doc->addResource(QTextDocument::ImageResource, file, img);
    QTextImageFormat fmt;
    fmt.setName(file.toString());
    // Word-like cap: images wider than the page column scale down, keeping aspect.
    constexpr qreal kMaxWidth = 480.0;
    if (img.width() > kMaxWidth) {
        fmt.setWidth(kMaxWidth);
        fmt.setHeight(img.height() * kMaxWidth / img.width());
    }
    insertionCursor().insertImage(fmt);
    emit formatChanged();
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

} // namespace

void RichTextHandler::insertTable(int rows, int cols)
{
    QTextCursor c = insertionCursor();
    if (c.isNull() || rows < 1 || cols < 1)
        return;
    c.insertTable(rows, cols, wordLikeTableFormat());
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

bool RichTextHandler::saveOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    // Qt's NATIVE OpenDocument writer.
    QTextDocumentWriter writer(file.isLocalFile() ? file.toLocalFile() : file.toString());
    writer.setFormat("odf");
    const bool ok = writer.write(doc);
    if (!ok)
        emit ioError(QStringLiteral("could not write ") + file.toString());
    return ok;
}

// ─── minimal ODF reader (Qt ships none): content.xml → QTextDocument ───────────────────────────

namespace {

struct OdfStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
};

// Automatic styles from content.xml: style:style/style:text-properties attributes.
QHash<QString, OdfStyle> readAutomaticStyles(QXmlStreamReader &xml)
{
    QHash<QString, OdfStyle> styles;
    QString current;
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::EndElement && xml.name() == QLatin1String("automatic-styles"))
            break;
        if (tok != QXmlStreamReader::StartElement)
            continue;
        if (xml.name() == QLatin1String("style"))
            current = xml.attributes().value(QLatin1String("style:name")).toString();
        else if (xml.name() == QLatin1String("text-properties") && !current.isEmpty()) {
            OdfStyle s;
            const auto a = xml.attributes();
            s.bold = a.value(QLatin1String("fo:font-weight")) == QLatin1String("bold");
            s.italic = a.value(QLatin1String("fo:font-style")) == QLatin1String("italic");
            s.underline = a.value(QLatin1String("style:text-underline-style")) == QLatin1String("solid");
            s.strike = a.value(QLatin1String("style:text-line-through-style")) == QLatin1String("solid");
            styles.insert(current, s);
        }
    }
    return styles;
}

QTextCharFormat formatFor(const OdfStyle &s)
{
    QTextCharFormat f;
    if (s.bold) f.setFontWeight(QFont::Bold);
    f.setFontItalic(s.italic);
    f.setFontUnderline(s.underline);
    f.setFontStrikeOut(s.strike);
    return f;
}

// ODF lengths ("3.5in", "2.4cm", "12pt", …) → CSS pixels (96 dpi, Qt's rich-text unit).
qreal odfLengthPx(QStringView v)
{
    if (v.isEmpty())
        return -1;
    int i = 0;
    while (i < v.size() && (v.at(i).isDigit() || v.at(i) == QLatin1Char('.') || v.at(i) == QLatin1Char('-')))
        ++i;
    bool ok = false;
    const qreal n = v.left(i).toDouble(&ok);
    if (!ok)
        return -1;
    const QStringView unit = v.mid(i);
    if (unit == QLatin1String("in")) return n * 96.0;
    if (unit == QLatin1String("cm")) return n * 96.0 / 2.54;
    if (unit == QLatin1String("mm")) return n * 96.0 / 25.4;
    if (unit == QLatin1String("pt")) return n * 96.0 / 72.0;
    return n; // px or unitless
}

} // namespace

bool RichTextHandler::loadOdf(const QUrl &file)
{
    QTextDocument *doc = m_document ? m_document->textDocument() : nullptr;
    if (!doc)
        return false;
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QZipReader zip(path);
    const QByteArray content = zip.fileData(QStringLiteral("content.xml"));
    if (content.isEmpty()) {
        emit ioError(QStringLiteral("not an OpenDocument file: ") + path);
        return false;
    }

    QXmlStreamReader xml(content);
    QHash<QString, OdfStyle> styles;

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
                styles = readAutomaticStyles(xml);
            } else if (name == QLatin1String("list")) {
                ++listDepth;
            } else if (name == QLatin1String("h")) {
                inParagraph = true;
                const int level = xml.attributes().value(QLatin1String("text:outline-level")).toString().toInt();
                QTextBlockFormat bf;
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
                beginBlock(QTextBlockFormat());
                const QString styleName = xml.attributes().value(QLatin1String("text:style-name")).toString();
                act().setCharFormat(formatFor(styles.value(styleName)));
            } else if (name == QLatin1String("span")) {
                const QString styleName = xml.attributes().value(QLatin1String("text:style-name")).toString();
                act().setCharFormat(formatFor(styles.value(styleName)));
            } else if (name == QLatin1String("tab")) {
                act().insertText(QStringLiteral("\t"));
            } else if (name == QLatin1String("line-break")) {
                act().insertText(QString(QChar::LineSeparator));
            } else if (name == QLatin1String("s")) {
                int n = xml.attributes().value(QLatin1String("text:c")).toString().toInt();
                act().insertText(QString(qMax(1, n), QLatin1Char(' ')));
            } else if (name == QLatin1String("table")) {
                curTable = nullptr;
                declaredCols = 0;
                tRow = -1;
            } else if (name == QLatin1String("table-column")) {
                const int rep = xml.attributes().value(QLatin1String("table:number-columns-repeated")).toString().toInt();
                declaredCols += qMax(1, rep);
            } else if (name == QLatin1String("table-row")) {
                if (!curTable)
                    curTable = c.insertTable(1, qMax(1, declaredCols), wordLikeTableFormat());
                else
                    curTable->appendRows(1);
                ++tRow;
                tCol = -1;
            } else if (name == QLatin1String("table-cell")) {
                if (curTable && tRow >= 0) {
                    ++tCol;
                    const QTextTableCell cell = curTable->cellAt(tRow, qMin(tCol, curTable->columns() - 1));
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

    if (xml.hasError()) {
        emit ioError(QStringLiteral("ODF parse error: ") + xml.errorString());
        return false;
    }
    emit formatChanged();
    return true;
}
