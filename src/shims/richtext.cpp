#include "richtext.h"

#include <QFile>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentWriter>
#include <QTextList>
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

    const auto beginBlock = [&](const QTextBlockFormat &bf) {
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
                c.setCharFormat(cf);
            } else if (name == QLatin1String("p")) {
                inParagraph = true;
                beginBlock(QTextBlockFormat());
                const QString styleName = xml.attributes().value(QLatin1String("text:style-name")).toString();
                c.setCharFormat(formatFor(styles.value(styleName)));
            } else if (name == QLatin1String("span")) {
                const QString styleName = xml.attributes().value(QLatin1String("text:style-name")).toString();
                c.setCharFormat(formatFor(styles.value(styleName)));
            } else if (name == QLatin1String("tab")) {
                c.insertText(QStringLiteral("\t"));
            } else if (name == QLatin1String("line-break")) {
                c.insertText(QString(QChar::LineSeparator));
            } else if (name == QLatin1String("s")) {
                int n = xml.attributes().value(QLatin1String("text:c")).toString().toInt();
                c.insertText(QString(qMax(1, n), QLatin1Char(' ')));
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            const auto name = xml.name();
            if (name == QLatin1String("list"))
                --listDepth;
            else if (name == QLatin1String("p") || name == QLatin1String("h"))
                inParagraph = false;
            else if (name == QLatin1String("span"))
                c.setCharFormat(QTextCharFormat());
        } else if (tok == QXmlStreamReader::Characters) {
            // Whitespace between spans IS content inside a paragraph (a single significant
            // space); outside paragraphs it is XML formatting noise.
            if (inParagraph)
                c.insertText(xml.text().toString());
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
