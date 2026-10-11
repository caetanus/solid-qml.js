// RichTextHandler — the engine behind <RichText> and <DocumentEditor>: what an app that DRIVES the
// document relies on (office item 0). modified tracking (and its reset on load/save/new), the
// {ok, error} contract of the ODF IO, the debounce source (contentsChanged), the readable format
// state (alignment), and an .odt round-trip through our writer + reader. Office item 2: named
// paragraph/character styles (define, apply, re-apply, next style on Enter, direct formatting)
// and their styles.xml round-trip.
#include "shims/richtext.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTextDocumentWriter>
#include <QTextFragment>
#include <QTextList>
#include <QElapsedTimer>

#include <private/qzipreader_p.h>
#include <private/qzipwriter_p.h>

namespace {

int countTables(QTextDocument *doc, int *columns = nullptr)
{
    int n = 0;
    for (QTextFrame *f : doc->rootFrame()->childFrames()) {
        if (auto *t = qobject_cast<QTextTable *>(f)) {
            ++n;
            if (columns) *columns = t->columns();
        }
    }
    return n;
}

#define ODF_NS \
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\"" \
    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\"" \
    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\"" \
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\"" \
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""

// A minimal .odt whose content.xml body is `body`, with optional automatic styles (content.xml)
// and named styles (styles.xml office:styles).
void writeOdt(const QString &path, const QByteArray &body, const QByteArray &automaticStyles = {},
              const QByteArray &namedStyles = {})
{
    QZipWriter zip(path);
    zip.addFile(QStringLiteral("mimetype"), "application/vnd.oasis.opendocument.text");
    if (!namedStyles.isEmpty())
        zip.addFile(QStringLiteral("styles.xml"),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-styles" ODF_NS ">"
            "<office:styles>" + namedStyles + "</office:styles></office:document-styles>");
    zip.addFile(QStringLiteral("content.xml"),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-content" ODF_NS ">"
        "<office:automatic-styles>" + automaticStyles + "</office:automatic-styles>"
        "<office:body><office:text>" + body + "</office:text></office:body></office:document-content>");
    zip.close();
}

QTextBlockFormat blockFormat(QTextDocument *doc, int block)
{
    return doc->findBlockByNumber(block).blockFormat();
}

QString mapKey(const QVariantMap &m)
{
    QStringList parts;
    for (auto it = m.cbegin(); it != m.cend(); ++it)
        parts << it.key() + QLatin1Char('=') + (it.value().isNull() ? QStringLiteral("null") : it.value().toString());
    return parts.join(QLatin1Char(','));
}

// Everything the style model says about a document, block by block: paragraph style, heading
// level, list membership, direct paragraph properties, then each run's character style, direct
// properties and text. Two documents that describe the same are the same to a reader.
QStringList describe(QTextDocument *doc, const rt::StyleSheet &sheet)
{
    QStringList out;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QString ps = rt::paragraphStyleOf(b);
        QString line = ps + QStringLiteral(" h") + QString::number(b.blockFormat().headingLevel())
            + (b.textList() ? QStringLiteral(" list") : QString())
            + QStringLiteral(" {") + mapKey(rt::directProps(b.blockFormat(), sheet.blockFormat(ps), true)) + QStringLiteral("}");
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.charFormat().isImageFormat()) {
                line += QStringLiteral(" [image]");
                continue;
            }
            const QString cs = rt::charStyleOf(f.charFormat());
            line += QStringLiteral(" [") + cs + QLatin1Char('|')
                + mapKey(rt::directProps(f.charFormat(), sheet.charFormat(ps, cs), false)) + QLatin1Char('|') + f.text() + QLatin1Char(']');
        }
        out << line;
    }
    return out;
}

// RICHTEXT_TEST_OUT=<dir>: keep a copy of the files the writer tests produce (to open them in
// LibreOffice, or `soffice --headless --convert-to pdf` them).
void keepForInspection(const QString &file)
{
    const QString dir = qEnvironmentVariable("RICHTEXT_TEST_OUT");
    if (dir.isEmpty())
        return;
    const QString to = QDir(dir).filePath(QFileInfo(file).fileName());
    QFile::remove(to);
    QFile::copy(file, to);
}

QTextCharFormat charAt(QTextDocument *doc, int pos)
{
    QTextCursor c(doc);
    c.setPosition(pos + 1); // charFormat() is the character before the cursor
    return c.charFormat();
}

// One entry's bytes out of a .odt (content.xml/styles.xml), for a save output comparison that
// must ignore meta.xml's timestamp.
QByteArray zipEntry(const QString &path, const QString &entry)
{
    QZipReader zip(path);
    return zip.fileData(entry);
}

// content.xml without the blocks' xml:id (office item 3): for byte comparisons of the rest.
QByteArray withoutBlockIds(const QByteArray &xml)
{
    return QString::fromUtf8(xml).remove(QRegularExpression(QStringLiteral(" xml:id=\"[^\"]*\""))).toUtf8();
}

QVariantMap markAtMap(RichTextHandler *fmt, int pos) { return fmt->markAt(pos).toMap(); }

QRectF caretRect(QObject *edit, int pos)
{
    QRectF r;
    QMetaObject::invokeMethod(edit, "positionToRectangle", Q_RETURN_ARG(QRectF, r), Q_ARG(int, pos));
    return r;
}

} // namespace

class RichTextTest : public QObject {
    Q_OBJECT

    QQmlEngine m_engine;
    QScopedPointer<QObject> m_edit;
    RichTextHandler *m_fmt = nullptr;
    QTemporaryDir m_dir;

    QTextDocument *doc() const { return m_fmt->document()->textDocument(); }

private slots:
    void init()
    {
        QQmlComponent c(&m_engine);
        c.setData("import QtQuick\nTextEdit { textFormat: TextEdit.RichText }", QUrl());
        m_edit.reset(c.create());
        QVERIFY2(m_edit, qPrintable(c.errorString()));
        m_fmt = new RichTextHandler(m_edit.data());
        m_fmt->setDocument(m_edit->property("textDocument").value<QQuickTextDocument *>());
        QVERIFY(m_fmt->document());
        m_fmt->newDocument();
    }
    void cleanup() { m_edit.reset(); }

    void newDocumentIsEmptyAndUnmodified()
    {
        QCOMPARE(m_fmt->plainText(), QString());
        QCOMPARE(m_fmt->isModified(), false);
    }

    void editingSetsModifiedAndSignalsContent()
    {
        QSignalSpy modified(m_fmt, &RichTextHandler::modifiedChanged);
        QSignalSpy content(m_fmt, &RichTextHandler::contentsChanged);
        m_fmt->insertText(QStringLiteral("Hello"));
        QCOMPARE(m_fmt->isModified(), true);
        QCOMPARE(modified.count(), 1);
        QVERIFY(content.count() >= 1);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Hello"));
    }

    void newDocumentNotifiesUnmodified()
    {
        // QTextDocument::clear() signals modified=true, then resets the flag silently: a binding
        // on `modified` must still end on false.
        m_fmt->insertText(QStringLiteral("x"));
        QList<bool> seen;
        connect(m_fmt, &RichTextHandler::modifiedChanged, this, [&] { seen << m_fmt->isModified(); });
        m_fmt->newDocument();
        QVERIFY(!seen.isEmpty());
        QCOMPARE(seen.last(), false);
    }

    void plainTextUsesNewlines()
    {
        m_fmt->insertText(QStringLiteral("one\ntwo"));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("one\ntwo"));
        QVERIFY(m_fmt->toHtml().contains(QStringLiteral("two")));
    }

    void alignmentIsReadableAndSettable()
    {
        m_fmt->insertText(QStringLiteral("centered"));
        QCOMPARE(m_fmt->alignment(), QStringLiteral("left"));
        m_fmt->setAlignment(QStringLiteral("center"));
        QCOMPARE(m_fmt->alignment(), QStringLiteral("center"));
        m_fmt->setAlignment(QStringLiteral("justify"));
        QCOMPARE(m_fmt->alignment(), QStringLiteral("justify"));
    }

    void saveClearsModifiedAndRoundTrips()
    {
        m_fmt->insertText(QStringLiteral("Title"));
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral("\nBody text"));
        m_fmt->setCursorPosition(8);
        m_fmt->setHeading(0);
        m_fmt->setAlignment(QStringLiteral("center"));
        {
            QTextCursor body(doc());
            body.setPosition(8);
            QVERIFY(body.charFormat().fontPointSize() != 20.0); // body text drops the heading size
            QVERIFY(body.charFormat().fontWeight() < QFont::Bold);
        }
        QVERIFY(m_fmt->isModified());
        const QString path = m_dir.filePath(QStringLiteral("rt.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl::fromLocalFile(path)));
        QCOMPARE(m_fmt->errorString(), QString());
        QCOMPARE(m_fmt->isModified(), false);

        m_fmt->newDocument();
        QCOMPARE(m_fmt->plainText(), QString());
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        QCOMPARE(m_fmt->isModified(), false);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Title\nBody text"));
        m_fmt->setCursorPosition(8);
        QCOMPARE(m_fmt->alignment(), QStringLiteral("center"));
        QCOMPARE(m_fmt->heading(), 0);
        m_fmt->setCursorPosition(1);
        QVERIFY(m_fmt->bold()); // the heading's look (bold, its style's size) survives …
        QTextCursor at(doc());
        at.setPosition(2);
        QCOMPARE(at.charFormat().fontPointSize(), m_fmt->styleProperties(QStringLiteral("Heading 1")).value("fontSize").toDouble());
        // … and so does its level and its style (our writer: <text:h text:outline-level>).
        QCOMPARE(m_fmt->heading(), 1);
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Heading 1"));
        // The load is not an undo step: Ctrl+Z must not empty the freshly opened document.
        QCOMPARE(doc()->isUndoAvailable(), false);
    }

    void plainPathsWork()
    {
        // The app passes plain filesystem paths, not only file:// URLs.
        m_fmt->insertText(QStringLiteral("x"));
        const QString path = m_dir.filePath(QStringLiteral("plain path.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(path)));
        QVERIFY(m_fmt->loadOdf(QUrl(path)));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("x"));
    }

    void loadErrorsAreReportedAndKeepTheDocument()
    {
        m_fmt->insertText(QStringLiteral("keep me"));
        QVERIFY(!m_fmt->loadOdf(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("missing.odt")))));
        QVERIFY2(m_fmt->errorString().contains(QStringLiteral("not found")), qPrintable(m_fmt->errorString()));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("keep me"));

        const QString junk = m_dir.filePath(QStringLiteral("junk.odt"));
        QFile f(junk);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not a zip");
        f.close();
        QVERIFY(!m_fmt->loadOdf(QUrl::fromLocalFile(junk)));
        QVERIFY(m_fmt->errorString().contains(QStringLiteral("not an OpenDocument")));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("keep me"));
    }

    void insertImageFromPlainPathRoundTrips()
    {
        const QString png = m_dir.filePath(QStringLiteral("dot.png"));
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(png));
        QVERIFY(m_fmt->insertImage(QUrl(png)));
        QVERIFY(doc()->resource(QTextDocument::ImageResource, QUrl::fromLocalFile(png)).isValid());
        QVERIFY(!m_fmt->insertImage(QUrl(m_dir.filePath(QStringLiteral("none.png")))));
        QVERIFY(m_fmt->errorString().contains(QStringLiteral("could not load image")));

        const QString odt = m_dir.filePath(QStringLiteral("img.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(odt)));
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QVERIFY2(m_fmt->toHtml().contains(QStringLiteral("odf:/Pictures/")), qPrintable(m_fmt->toHtml()));
    }

    void tableAfterAListRoundTrips()
    {
        // A table inserted right after a list item must not end up inside the list: Qt's ODF
        // writer would nest it in <text:list> with cells outside their rows (seen with a 300-page
        // book: the reload then crashed).
        m_fmt->insertText(QStringLiteral("item"));
        m_fmt->toggleBulletList();
        m_fmt->setCursorPosition(4);
        m_fmt->insertTable(2, 3);
        QCOMPARE(countTables(doc()), 1);
        const QString odt = m_dir.filePath(QStringLiteral("list-table.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(odt)));
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        int cols = 0;
        QCOMPARE(countTables(doc(), &cols), 1);
        QCOMPARE(cols, 3);
        QVERIFY(m_fmt->plainText().startsWith(QStringLiteral("item")));
    }

    void misnestedTableLoadsWithoutCrashing()
    {
        // What Qt's writer produced for that case: cells outside their row, a row after the table.
        const QString odt = m_dir.filePath(QStringLiteral("misnested.odt"));
        writeOdt(odt,
            "<text:p>before</text:p>"
            "<table:table><table:table-column table:number-columns-repeated=\"3\"/>"
            "<table:table-row><table:table-cell/><text:p>Item</text:p></table:table-row>"
            "<table:table-cell><text:p>Valor</text:p></table:table-cell>"
            "</table:table>"
            "<table:table-row><table:table-cell><text:p>orphan</text:p></table:table-cell></table:table-row>"
            "<text:p>after</text:p>");
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        const QString text = m_fmt->plainText();
        for (const char *w : { "before", "Item", "Valor", "orphan", "after" })
            QVERIFY2(text.contains(QLatin1String(w)), qPrintable(text));
    }

    void nestedTableTextIsKept()
    {
        const QString odt = m_dir.filePath(QStringLiteral("nested.odt"));
        writeOdt(odt,
            "<table:table><table:table-column table:number-columns-repeated=\"2\"/>"
            "<table:table-row><table:table-cell>"
            "<table:table><table:table-column/><table:table-row><table:table-cell><text:p>inner</text:p>"
            "</table:table-cell></table:table-row></table:table>"
            "</table:table-cell><table:table-cell><text:p>right</text:p></table:table-cell></table:table-row>"
            "</table:table><text:p>after</text:p>");
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        int cols = 0;
        QCOMPARE(countTables(doc(), &cols), 1);
        QCOMPARE(cols, 2);
        const QString text = m_fmt->plainText();
        QVERIFY2(text.contains(QStringLiteral("inner")) && text.contains(QStringLiteral("right"))
                 && text.endsWith(QStringLiteral("after")), qPrintable(text));
    }

    void largeDocumentsCullToTheViewport()
    {
        // QQuickTextEdit renders only the blocks near the viewport (ItemObservesViewport) when its
        // text is set to > 10 000 chars — in setText only. A document built through QTextCursor
        // (loadOdf, typing) must get the same: a 300-page .odt otherwise re-renders every block
        // on each keystroke (~400 ms/key, 345 MB vs 17-32 ms/key, 83 MB).
        auto *edit = qobject_cast<QQuickItem *>(m_edit.data());
        QVERIFY(edit);
        QCOMPARE(m_fmt->document()->parent(), edit); // the item the flag belongs on
        const auto culling = [&] { return edit->flags().testFlag(QQuickItem::ItemObservesViewport); };
        QVERIFY(!culling());

        m_fmt->insertText(QString(RichTextHandler::kLargeTextThreshold + 1, QLatin1Char('a'))); // typing/pasting
        QVERIFY(culling());
        m_fmt->newDocument();
        QVERIFY(!culling());

        const QString odt = m_dir.filePath(QStringLiteral("large.odt"));
        QByteArray body;
        for (int i = 0; i < 200; ++i)
            body += "<text:p>" + QByteArray(60, 'w') + "</text:p>";
        writeOdt(odt, body);
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QVERIFY(culling());

        // Shrinking back below the threshold turns it off again, as setText does.
        QTextCursor all(doc());
        all.select(QTextCursor::Document);
        all.insertText(QStringLiteral("small"));
        QVERIFY(!culling());
    }

    void odfParagraphSpacingFollowsTheStyleChain()
    {
        const QString odt = m_dir.filePath(QStringLiteral("styles.odt"));
        writeOdt(odt,
            "<text:p text:style-name=\"Text_20_body\">body</text:p>"
            "<text:p text:style-name=\"P1\">centered</text:p>"
            "<text:p text:style-name=\"Quotations\">quote</text:p>"
            "<text:p text:style-name=\"Standard\">plain</text:p>"
            "<text:p text:style-name=\"Tight\">tight</text:p>"
            "<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\">head</text:h>",
            // content.xml: an automatic style on top of a named one (what LibreOffice writes)
            "<style:style style:name=\"P1\" style:family=\"paragraph\" style:parent-style-name=\"Text_20_body\">"
            "<style:paragraph-properties fo:text-align=\"center\"/></style:style>",
            "<style:default-style style:family=\"paragraph\"><style:paragraph-properties/></style:default-style>"
            "<style:style style:name=\"Standard\" style:family=\"paragraph\"/>"
            "<style:style style:name=\"Text_20_body\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
            "<style:paragraph-properties fo:margin-top=\"0.0598in\" fo:margin-bottom=\"0.0598in\"/></style:style>"
            "<style:style style:name=\"Quotations\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
            "<style:paragraph-properties fo:margin-left=\"0.3937in\" fo:margin-right=\"0.3937in\""
            " fo:margin-top=\"0.1in\" fo:margin-bottom=\"0.1in\" fo:text-indent=\"-0.1in\"/></style:style>"
            "<style:style style:name=\"Tight\" style:family=\"paragraph\" style:parent-style-name=\"Text_20_body\">"
            "<style:paragraph-properties fo:margin-top=\"0in\" fo:margin-bottom=\"0in\"/></style:style>"
            "<style:style style:name=\"Heading\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
            "<style:paragraph-properties fo:margin-top=\"0.1665in\" fo:margin-bottom=\"0.0835in\"/></style:style>"
            "<style:style style:name=\"Heading_20_1\" style:family=\"paragraph\" style:parent-style-name=\"Heading\"/>");
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("body\ncentered\nquote\nplain\ntight\nhead"));
        const qreal in = 96.0;
        const auto near = [](qreal a, qreal b) { return qAbs(a - b) < 0.01; };

        const QTextBlockFormat body = blockFormat(doc(), 0); // named style
        QVERIFY2(near(body.topMargin(), 0.0598 * in) && near(body.bottomMargin(), 0.0598 * in),
                 qPrintable(QString::number(body.topMargin())));
        const QTextBlockFormat centered = blockFormat(doc(), 1); // automatic style → its parent's margins
        QVERIFY(near(centered.topMargin(), 0.0598 * in) && near(centered.bottomMargin(), 0.0598 * in));
        QVERIFY(centered.alignment() & Qt::AlignHCenter);
        const QTextBlockFormat quote = blockFormat(doc(), 2);
        QVERIFY(near(quote.leftMargin(), 0.3937 * in) && near(quote.rightMargin(), 0.3937 * in));
        QVERIFY(near(quote.topMargin(), 0.1 * in) && near(quote.bottomMargin(), 0.1 * in));
        QVERIFY(near(quote.textIndent(), -0.1 * in));
        const QTextBlockFormat head = blockFormat(doc(), 5);
        QCOMPARE(head.headingLevel(), 1);
        QVERIFY(near(head.topMargin(), 0.1665 * in) && near(head.bottomMargin(), 0.0835 * in));

        // No vertical spacing anywhere in the chain (inherited or explicit 0): the small default.
        for (int b : { 3, 4 }) {
            const QTextBlockFormat f = blockFormat(doc(), b);
            QCOMPARE(f.topMargin(), 0.0);
            QCOMPARE(f.bottomMargin(), RichTextHandler::kMinParagraphSpacing);
        }

        // Our writer keeps the named styles (and the spacing they give): a reload agrees.
        const QString again = m_dir.filePath(QStringLiteral("styles-again.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(again)));
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(again)));
        QVERIFY(near(blockFormat(doc(), 2).leftMargin(), 0.3937 * in));
        QVERIFY(near(blockFormat(doc(), 2).textIndent(), -0.1 * in));
        QVERIFY(near(blockFormat(doc(), 0).topMargin(), 0.0598 * in));
        QCOMPARE(blockFormat(doc(), 3).bottomMargin(), RichTextHandler::kMinParagraphSpacing);
    }

    void saveOdfCopyLeavesTheDocumentAlone()
    {
        // Autosave / crash recovery: the file gets the edit, the document stays "unsaved".
        m_fmt->insertText(QStringLiteral("draft"));
        QVERIFY(m_fmt->isModified());
        QVERIFY(doc()->isUndoAvailable());
        QSignalSpy modified(m_fmt, &RichTextHandler::modifiedChanged);
        const QString copy = m_dir.filePath(QStringLiteral("autosave.odt"));
        QVERIFY(m_fmt->saveOdfCopy(QUrl(copy)));
        QCOMPARE(m_fmt->errorString(), QString());
        QCOMPARE(m_fmt->isModified(), true);
        QCOMPARE(modified.count(), 0);
        QVERIFY(doc()->isUndoAvailable());
        QVERIFY(!m_fmt->saveOdfCopy(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("no/such/dir/x.odt")))));
        QVERIFY(!m_fmt->errorString().isEmpty());
        QCOMPARE(m_fmt->isModified(), true);

        // saveOdf still clears it.
        QVERIFY(m_fmt->saveOdf(QUrl(m_dir.filePath(QStringLiteral("saved.odt")))));
        QCOMPARE(m_fmt->isModified(), false);

        // The copy holds the edit.
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(copy)));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("draft"));
    }

    // ── office item 2: named styles ──────────────────────────────────────────────────────────

    void builtInStylesUseLibreOfficeNames()
    {
        const QStringList para = m_fmt->styleNames(QStringLiteral("paragraph"));
        for (const char *n : { "Default Paragraph Style", "Text Body", "Heading", "Heading 1", "Heading 2", "Heading 3",
                               "Heading 4", "Heading 5", "Heading 6", "Title", "Quotations" })
            QVERIFY2(para.contains(QLatin1String(n)), n);
        const QStringList chars = m_fmt->styleNames(QStringLiteral("character"));
        QVERIFY(chars.contains(QStringLiteral("Emphasis")));
        QVERIFY(chars.contains(QStringLiteral("Strong Emphasis")));
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Default Paragraph Style"));
        QCOMPARE(m_fmt->currentCharStyle(), QString());

        const QVariantMap h1 = m_fmt->styleProperties(QStringLiteral("Heading 1"));
        QCOMPARE(h1.value("family").toString(), QStringLiteral("paragraph"));
        QCOMPARE(h1.value("parent").toString(), QStringLiteral("Heading"));
        QCOMPARE(h1.value("nextStyle").toString(), QStringLiteral("Text Body"));
        QCOMPARE(h1.value("outlineLevel").toInt(), 1);
        QVERIFY(!h1.contains(QStringLiteral("keepWithNext")));                                         // own …
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Heading 1"), true).value("keepWithNext").toBool(), true); // … resolved
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Emphasis")).value("italic").toBool(), true);
        QVERIFY(m_fmt->styleProperties(QStringLiteral("nope")).isEmpty());
    }

    void applyParagraphStyleAndSetHeading()
    {
        m_fmt->insertText(QStringLiteral("Chapter"));
        QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Heading 1")));
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Heading 1"));
        QCOMPARE(m_fmt->heading(), 1);
        QVERIFY(m_fmt->bold());
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 18.2);
        QVERIFY(blockFormat(doc(), 0).topMargin() > 10);

        m_fmt->setHeading(2); // item 0's API = the "Heading n" styles
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Heading 2"));
        QCOMPARE(m_fmt->heading(), 2);
        m_fmt->setHeading(0);
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Text Body"));
        QCOMPARE(m_fmt->heading(), 0);
        QVERIFY(!m_fmt->bold());
        QVERIFY(!charAt(doc(), 2).hasProperty(QTextFormat::FontPointSize)); // the editor's font again

        QVERIFY(!m_fmt->applyParagraphStyle(QStringLiteral("Emphasis"))); // a character style
        QVERIFY(!m_fmt->applyParagraphStyle(QStringLiteral("nope")));
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Text Body"));

        // A selection restyles every paragraph it touches, as one undo step.
        m_fmt->setCursorPosition(7);
        m_fmt->insertText(QStringLiteral("\nsecond\nthird"));
        m_fmt->setSelectionStart(2);
        m_fmt->setSelectionEnd(12);
        QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Quotations")));
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(0)), QStringLiteral("Quotations"));
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(1)), QStringLiteral("Quotations"));
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(2)), QStringLiteral("Text Body"));
        doc()->undo();
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(0)), QStringLiteral("Text Body"));
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(1)), QStringLiteral("Text Body"));
    }

    void redefiningAStyleReappliesItKeepingDirectFormatting()
    {
        m_fmt->insertText(QStringLiteral("alpha beta\nplain\ncentered"));
        for (int block : { 0, 2 }) {
            m_fmt->setCursorPosition(doc()->findBlockByNumber(block).position());
            QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Text Body")));
        }
        m_fmt->setSelectionStart(6); // "beta" bold, directly
        m_fmt->setSelectionEnd(10);
        m_fmt->toggleBold();
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        m_fmt->setCursorPosition(doc()->findBlockByNumber(2).position() + 1);
        m_fmt->setAlignment(QStringLiteral("center")); // direct paragraph formatting
        const int centered = doc()->findBlockByNumber(2).position();
        doc()->setModified(false);

        QSignalSpy styles(m_fmt, &RichTextHandler::stylesChanged);
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Text Body"),
                 { { "fontSize", 13 }, { "alignment", "justify" }, { "marginLeft", "1cm" }, { "color", "#336699" } }), QString());
        QCOMPARE(styles.count(), 1);
        QVERIFY(m_fmt->isModified());
        QCOMPARE(charAt(doc(), 1).fontPointSize(), 13.0);
        QCOMPARE(charAt(doc(), 7).fontPointSize(), 13.0);
        QCOMPARE(charAt(doc(), 7).fontWeight(), int(QFont::Bold)); // direct bold kept
        QCOMPARE(charAt(doc(), 1).foreground().color(), QColor(0x33, 0x66, 0x99));
        QVERIFY(blockFormat(doc(), 0).alignment() & Qt::AlignJustify);
        QVERIFY(qAbs(blockFormat(doc(), 0).leftMargin() - 96 / 2.54) < 0.01);
        QVERIFY(blockFormat(doc(), 2).alignment() & Qt::AlignHCenter); // direct alignment kept
        QVERIFY(qAbs(blockFormat(doc(), 2).leftMargin() - 96 / 2.54) < 0.01);
        QCOMPARE(charAt(doc(), centered + 1).fontPointSize(), 13.0);
        QVERIFY(!charAt(doc(), 12).hasProperty(QTextFormat::FontPointSize)); // "plain": Default Paragraph Style
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("marginLeft").toDouble(), 96 / 2.54);

        // A null removes a property; the paragraphs follow.
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Text Body"), { { "color", QVariant::fromValue(nullptr) } }), QString());
        QVERIFY(!charAt(doc(), 1).hasProperty(QTextFormat::ForegroundBrush));

        // The definition is part of the undo history: undo puts the old one back with the look.
        doc()->undo();
        QCOMPARE(charAt(doc(), 1).foreground().color(), QColor(0x33, 0x66, 0x99));
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("color").toString(), QStringLiteral("#336699"));
        doc()->undo();
        QVERIFY(!m_fmt->styleProperties(QStringLiteral("Text Body")).contains(QStringLiteral("fontSize")));
        QVERIFY(!charAt(doc(), 1).hasProperty(QTextFormat::FontPointSize));
        QCOMPARE(charAt(doc(), 7).fontWeight(), int(QFont::Bold));
        doc()->redo();
        QCOMPARE(charAt(doc(), 1).fontPointSize(), 13.0);
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("fontSize").toDouble(), 13.0);
    }

    void newStylesInheritAndValidate()
    {
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Note"), { { "family", "paragraph" }, { "parent", "Text Body" },
                                                              { "italic", true }, { "nextStyle", "Text Body" } }), QString());
        QVERIFY(m_fmt->styleNames(QStringLiteral("paragraph")).contains(QStringLiteral("Note")));
        m_fmt->insertText(QStringLiteral("a note"));
        QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Note")));
        QVERIFY(charAt(doc(), 1).fontItalic());
        // A change to the parent reaches the child.
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Text Body"), { { "fontSize", 15 } }), QString());
        QCOMPARE(charAt(doc(), 1).fontPointSize(), 15.0);
        QVERIFY(charAt(doc(), 1).fontItalic());

        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Marker"), { { "family", "character" }, { "fontWeight", "bold" } }), QString());
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Marker")).value("fontWeight").toInt(), 700);

        // Errors leave everything as it was.
        QVERIFY(!m_fmt->defineStyle(QStringLiteral("X"), { { "bogus", 1 } }).isEmpty());
        QVERIFY(!m_fmt->styleNames(QStringLiteral("paragraph")).contains(QStringLiteral("X")));
        QVERIFY(!m_fmt->defineStyle(QStringLiteral("Emphasis"), { { "marginTop", 3 } }).isEmpty());
        QVERIFY(!m_fmt->defineStyle(QStringLiteral("Text Body"), { { "parent", "Note" } }).isEmpty()); // a cycle
        QVERIFY(!m_fmt->defineStyle(QStringLiteral("Emphasis"), { { "family", "paragraph" } }).isEmpty());
        QVERIFY(!m_fmt->defineStyle(QStringLiteral("Y"), { { "parent", "Emphasis" } }).isEmpty()); // other family
        QVERIFY(!m_fmt->defineStyle(QStringLiteral("Y"), { { "fontSize", "big" } }).isEmpty());
        QVERIFY(!m_fmt->defineStyle(QStringLiteral("Y"), { { "nextStyle", "nope" } }).isEmpty());
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("parent").toString(), QStringLiteral("Default Paragraph Style"));
    }

    void characterStylesApplyAndRemove()
    {
        m_fmt->insertText(QStringLiteral("one two three"));
        m_fmt->setSelectionStart(4);
        m_fmt->setSelectionEnd(7);
        QVERIFY(m_fmt->applyCharStyle(QStringLiteral("Emphasis")));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        QVERIFY(charAt(doc(), 5).fontItalic());
        QVERIFY(!charAt(doc(), 1).fontItalic());
        QVERIFY(!charAt(doc(), 9).fontItalic());
        m_fmt->setCursorPosition(5);
        QCOMPARE(m_fmt->currentCharStyle(), QStringLiteral("Emphasis"));
        m_fmt->setCursorPosition(1);
        QCOMPARE(m_fmt->currentCharStyle(), QString());

        // Redefining the character style restyles its runs only.
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Emphasis"), { { "color", "#ff0000" } }), QString());
        QCOMPARE(charAt(doc(), 5).foreground().color(), QColor(Qt::red));
        QVERIFY(!charAt(doc(), 1).hasProperty(QTextFormat::ForegroundBrush));

        // With no selection: the word at the caret. "" removes the character style.
        m_fmt->setCursorPosition(10);
        QVERIFY(m_fmt->applyCharStyle(QStringLiteral("Strong Emphasis")));
        QCOMPARE(charAt(doc(), 9).fontWeight(), int(QFont::Bold));
        m_fmt->setCursorPosition(5);
        QVERIFY(m_fmt->applyCharStyle(QString()));
        QVERIFY(!charAt(doc(), 5).fontItalic());
        QCOMPARE(m_fmt->currentCharStyle(), QString());
        QVERIFY(!m_fmt->applyCharStyle(QStringLiteral("Heading 1"))); // a paragraph style
    }

    void enterStartsTheNextStyle()
    {
        m_fmt->insertText(QStringLiteral("Title"));
        QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Heading 1")));
        m_fmt->setCursorPosition(5);
        m_fmt->insertParagraph();
        QCOMPARE(doc()->blockCount(), 2);
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(1)), QStringLiteral("Text Body"));
        QCOMPARE(doc()->findBlockByNumber(1).blockFormat().headingLevel(), 0);
        m_fmt->setCursorPosition(6);
        m_fmt->insertText(QStringLiteral("body"));
        QVERIFY(charAt(doc(), 7).fontWeight() < QFont::Bold); // typed in the next style
        QVERIFY(!charAt(doc(), 7).hasProperty(QTextFormat::FontPointSize));
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Text Body"));

        // Enter is one undo step.
        doc()->undo();
        doc()->undo();
        QCOMPARE(doc()->blockCount(), 1);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Title"));

        // Splitting inside the paragraph keeps the style on both halves.
        m_fmt->setCursorPosition(2);
        m_fmt->insertParagraph();
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(0)), QStringLiteral("Heading 1"));
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(1)), QStringLiteral("Heading 1"));

        // insertText's "\n" is Enter too; a style that names itself as next continues.
        m_fmt->newDocument();
        m_fmt->insertText(QStringLiteral("x"));
        QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Heading 2")));
        m_fmt->setCursorPosition(1);
        m_fmt->insertText(QStringLiteral("\nafter"));
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(1)), QStringLiteral("Text Body"));
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Text Body"), { { "nextStyle", "Quotations" } }), QString());
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        m_fmt->insertParagraph();
        QCOMPARE(rt::paragraphStyleOf(doc()->lastBlock()), QStringLiteral("Quotations"));
    }

    void directFormattingIsEnumeratedAndCleared()
    {
        m_fmt->insertText(QStringLiteral("normal bold"));
        QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Text Body")));
        m_fmt->setSelectionStart(7);
        m_fmt->setSelectionEnd(11);
        m_fmt->toggleBold();
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        m_fmt->setCursorPosition(2);
        m_fmt->setAlignment(QStringLiteral("right"));

        QVariantMap at = m_fmt->directFormattingAt(8);
        QCOMPARE(at.value("paragraphStyle").toString(), QStringLiteral("Text Body"));
        QCOMPARE(at.value("charStyle").toString(), QString());
        QCOMPARE(at.value("character").toMap(), (QVariantMap { { "fontWeight", 700 } }));
        QCOMPARE(at.value("paragraph").toMap(), (QVariantMap { { "alignment", "right" } }));
        QCOMPARE(m_fmt->directFormattingAt(2).value("character").toMap(), QVariantMap());

        // Inside a heading, the heading's bold is the style's; un-bolding is the override.
        m_fmt->newDocument();
        m_fmt->insertText(QStringLiteral("Heading text"));
        QVERIFY(m_fmt->applyParagraphStyle(QStringLiteral("Heading 3")));
        QCOMPARE(m_fmt->directFormattingAt(1).value("character").toMap(), QVariantMap());
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(7);
        m_fmt->toggleBold();
        QCOMPARE(m_fmt->directFormattingAt(1).value("character").toMap(), (QVariantMap { { "fontWeight", 400 } }));

        // clearDirectFormatting: the selection's runs back to the style …
        m_fmt->clearDirectFormatting();
        QCOMPARE(m_fmt->directFormattingAt(1).value("character").toMap(), QVariantMap());
        QCOMPARE(charAt(doc(), 1).fontWeight(), int(QFont::Bold));
        // … and without a selection, the caret's whole paragraph (paragraph properties too).
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        m_fmt->setCursorPosition(3);
        m_fmt->setAlignment(QStringLiteral("center"));
        m_fmt->setSelectionStart(8);
        m_fmt->setSelectionEnd(12);
        m_fmt->toggleItalic();
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        QVERIFY(!m_fmt->directFormattingAt(9).value("character").toMap().isEmpty());
        m_fmt->clearDirectFormatting();
        QCOMPARE(m_fmt->directFormattingAt(9).value("character").toMap(), QVariantMap());
        QCOMPARE(m_fmt->directFormattingAt(9).value("paragraph").toMap(), QVariantMap());
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Heading 3"));
    }

    void ourWriterRoundTripsStylesAndHeadingLevels()
    {
        // Every heading level, the built-ins, a redefined and a new style, character styles,
        // direct formatting, a list, a table and an image.
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Text Body"), { { "fontSize", 13 }, { "lineHeight", 1.5 },
                                                                   { "fontFamily", "DejaVu Serif" } }), QString());
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Note"), { { "parent", "Text Body" }, { "italic", true },
                                    { "nextStyle", "Text Body" }, { "letterSpacing", 1.5 }, { "lineHeight", "18pt" },
                                    { "keepWithNext", true }, { "textIndent", "-0.5cm" } }), QString());
        QCOMPARE(m_fmt->defineStyle(QStringLiteral("Code Word"), { { "family", "character" }, { "fontFamily", "Monospace" },
                                                                   { "color", "#aa0000" } }), QString());
        QStringList lines { QStringLiteral("The Book") };
        for (int i = 1; i <= 6; ++i)
            lines << QStringLiteral("Level %1").arg(i);
        lines << QStringLiteral("Body with  two spaces,\ta tab and bold") << QStringLiteral("A note")
              << QStringLiteral("Quoted") << QStringLiteral(" leading space");
        m_fmt->insertText(lines.join(QLatin1Char('\n')));
        const auto styleBlock = [&](int block, const char *style) {
            m_fmt->setCursorPosition(doc()->findBlockByNumber(block).position());
            QVERIFY(m_fmt->applyParagraphStyle(QLatin1String(style)));
        };
        styleBlock(0, "Title");
        for (int i = 1; i <= 6; ++i)
            styleBlock(i, qPrintable(QStringLiteral("Heading %1").arg(i)));
        styleBlock(7, "Text Body");
        styleBlock(8, "Note");
        styleBlock(9, "Quotations");
        const int body = doc()->findBlockByNumber(7).position();
        const auto select = [&](int from, int to) { m_fmt->setSelectionStart(from); m_fmt->setSelectionEnd(to); };
        select(body + 33, body + 37); // "bold"
        m_fmt->toggleBold();
        select(body, body + 4); // "Body"
        QVERIFY(m_fmt->applyCharStyle(QStringLiteral("Code Word")));
        select(body + 5, body + 9); // "with": Emphasis + direct underline
        QVERIFY(m_fmt->applyCharStyle(QStringLiteral("Emphasis")));
        m_fmt->toggleUnderline();
        select(0, 0);
        m_fmt->setCursorPosition(doc()->findBlockByNumber(9).position());
        m_fmt->setAlignment(QStringLiteral("center"));
        // a list after the text, then a table, then an image
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        m_fmt->insertText(QStringLiteral("\nitem one\nitem two"));
        m_fmt->setCursorPosition(doc()->characterCount() - 2);
        m_fmt->setSelectionStart(doc()->characterCount() - 15);
        m_fmt->setSelectionEnd(doc()->characterCount() - 2);
        m_fmt->toggleBulletList();
        select(0, 0);
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        m_fmt->insertTable(2, 2);
        QTextTable *table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().value(0));
        QVERIFY(table);
        table->cellAt(1, 1).firstCursorPosition().insertText(QStringLiteral("cell"));
        const QString png = m_dir.filePath(QStringLiteral("pic.png"));
        QImage img(10, 6, QImage::Format_RGB32);
        img.fill(Qt::blue);
        QVERIFY(img.save(png));
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        QVERIFY(m_fmt->insertImage(QUrl(png)));

        const QStringList before = describe(doc(), m_fmt->styleSheet());
        const QString text = m_fmt->plainText();
        const QString odt = m_dir.filePath(QStringLiteral("styled.odt"));
        QVERIFY2(m_fmt->saveOdf(QUrl(odt)), qPrintable(m_fmt->errorString()));
        keepForInspection(odt);

        // The package: mimetype first and stored; named styles in styles.xml; text:h with levels.
        QFile raw(odt);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QByteArray head = raw.read(128);
        QVERIFY(head.startsWith("PK\x03\x04"));
        QCOMPARE(int(uchar(head[8])) | int(uchar(head[9])) << 8, 0); // compression method: stored
        QCOMPARE(head.mid(30, 8), QByteArray("mimetype"));
        QCOMPARE(head.mid(38, 39), QByteArray("application/vnd.oasis.opendocument.text"));
        QZipReader zip(odt);
        const QByteArray styles = zip.fileData(QStringLiteral("styles.xml"));
        const QByteArray rawContent = zip.fileData(QStringLiteral("content.xml"));
        const QByteArray content = withoutBlockIds(rawContent);
        QVERIFY(styles.contains("style:name=\"Heading_20_1\" style:display-name=\"Heading 1\" style:family=\"paragraph\""
                                " style:parent-style-name=\"Heading\" style:next-style-name=\"Text_20_body\" style:default-outline-level=\"1\""));
        QVERIFY(styles.contains("style:name=\"Note\""));
        QVERIFY(styles.contains("style:name=\"Code_20_Word\" style:display-name=\"Code Word\" style:family=\"text\""));
        for (int i = 1; i <= 6; ++i)
            QVERIFY2(content.contains(QStringLiteral("<text:h text:style-name=\"Heading_20_%1\" text:outline-level=\"%1\">Level %1</text:h>")
                                      .arg(i).toUtf8()), content.constData());
        QVERIFY(content.contains("<text:p text:style-name=\"Title\">The Book</text:p>"));
        QVERIFY(content.contains("style:parent-style-name=\"Quotations\"")); // the direct centering
        QVERIFY(content.contains("<text:s/>"));
        QVERIFY(content.contains("<text:tab/>"));
        QVERIFY(!content.contains("<text:list><table:table")); // the table is not inside the list
        QVERIFY(zip.fileData(QStringLiteral("META-INF/manifest.xml")).contains("Pictures/image1.png"));
        QVERIFY(!zip.fileData(QStringLiteral("meta.xml")).isEmpty());

        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QCOMPARE(m_fmt->plainText(), text);
        const QStringList after = describe(doc(), m_fmt->styleSheet());
        if (after != before) {
            for (int i = 0; i < qMax(before.size(), after.size()); ++i)
                if (before.value(i) != after.value(i))
                    qWarning() << "block" << i << "\n  before:" << before.value(i) << "\n  after: " << after.value(i);
        }
        QCOMPARE(after, before);
        for (int i = 1; i <= 6; ++i)
            QCOMPARE(doc()->findBlockByNumber(i).blockFormat().headingLevel(), i);
        QCOMPARE(countTables(doc()), 1);
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("fontSize").toDouble(), 13.0);
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("fontFamily").toString(), QStringLiteral("DejaVu Serif"));
        const QVariantMap note = m_fmt->styleProperties(QStringLiteral("Note"));
        QCOMPARE(note.value("parent").toString(), QStringLiteral("Text Body"));
        QCOMPARE(note.value("nextStyle").toString(), QStringLiteral("Text Body"));
        QCOMPARE(note.value("lineHeight").toString(), QStringLiteral("24px"));
        QCOMPARE(note.value("keepWithNext").toBool(), true);
        QVERIFY(qAbs(note.value("textIndent").toDouble() + 96 * 0.5 / 2.54) < 0.01);
        QCOMPARE(note.value("letterSpacing").toDouble(), 1.5);
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Code Word")).value("color").toString(), QStringLiteral("#aa0000"));
        QVERIFY2(m_fmt->toHtml().contains(QStringLiteral("odf:/Pictures/image1.png")), "the image came back");

        // Saving again writes the same document.
        const QString again = m_dir.filePath(QStringLiteral("styled-again.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(again)));
        QCOMPARE(QZipReader(again).fileData(QStringLiteral("content.xml")), rawContent); // block ids included
    }

    void libreOfficeStylesMapIntoTheStyleTable()
    {
        const QString odt = m_dir.filePath(QStringLiteral("lo.odt"));
        QZipWriter zip(odt);
        zip.addFile(QStringLiteral("mimetype"), "application/vnd.oasis.opendocument.text");
        zip.addFile(QStringLiteral("styles.xml"),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-styles" ODF_NS " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\">"
            "<office:font-face-decls><style:font-face style:name=\"Arial\" svg:font-family=\"Arial\"/>"
            "<style:font-face style:name=\"Times New Roman\" svg:font-family=\"'Times New Roman'\"/></office:font-face-decls>"
            "<office:styles>"
            "<style:default-style style:family=\"paragraph\"><style:text-properties style:font-name=\"Times New Roman\" fo:font-size=\"12pt\"/></style:default-style>"
            "<style:style style:name=\"Standard\" style:family=\"paragraph\" style:class=\"text\"/>"
            "<style:style style:name=\"Heading\" style:family=\"paragraph\" style:parent-style-name=\"Standard\" style:next-style-name=\"Text_20_body\">"
            "<style:paragraph-properties fo:keep-with-next=\"always\" fo:margin-top=\"0.1665in\" fo:margin-bottom=\"0.0835in\"/>"
            "<style:text-properties style:font-name=\"Arial\" fo:font-size=\"14pt\"/></style:style>"
            "<style:style style:name=\"Heading_20_1\" style:display-name=\"Heading 1\" style:family=\"paragraph\" style:parent-style-name=\"Heading\""
            " style:next-style-name=\"Text_20_body\" style:default-outline-level=\"1\"><style:text-properties fo:font-size=\"115%\" fo:font-weight=\"bold\"/></style:style>"
            "<style:style style:name=\"Text_20_body\" style:display-name=\"Text body\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
            "<style:paragraph-properties fo:margin-top=\"0.0598in\" fo:margin-bottom=\"0.0598in\" fo:line-height=\"115%\"/></style:style>"
            "<style:style style:name=\"First_20_paragraph\" style:display-name=\"First paragraph\" style:family=\"paragraph\" style:parent-style-name=\"Text_20_body\"/>"
            "<style:style style:name=\"Emphasis\" style:family=\"text\"><style:text-properties fo:font-style=\"italic\"/></style:style>"
            "</office:styles></office:document-styles>");
        zip.addFile(QStringLiteral("content.xml"),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?><office:document-content" ODF_NS ">"
            "<office:automatic-styles><style:style style:name=\"P1\" style:family=\"paragraph\" style:parent-style-name=\"Text_20_body\">"
            "<style:paragraph-properties fo:text-align=\"end\"/></style:style>"
            "<style:style style:name=\"T1\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\"/></style:style></office:automatic-styles>"
            "<office:body><office:text>"
            "<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\">Chapter</text:h>"
            "<text:p text:style-name=\"First_20_paragraph\">First <text:span text:style-name=\"Emphasis\">em <text:span text:style-name=\"T1\">both</text:span> em</text:span> plain</text:p>"
            "<text:p text:style-name=\"P1\">right</text:p>"
            "</office:text></office:body></office:document-content>");
        zip.close();

        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Chapter\nFirst em both em plain\nright"));
        // Named styles: LibreOffice's names map to ours, the others keep their display names.
        QVERIFY(m_fmt->styleNames(QStringLiteral("paragraph")).contains(QStringLiteral("First paragraph")));
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("First paragraph")).value("parent").toString(), QStringLiteral("Text Body"));
        const QVariantMap h1 = m_fmt->styleProperties(QStringLiteral("Heading 1"), true);
        QVERIFY(qAbs(h1.value("fontSize").toDouble() - 16.1) < 1e-6); // 115% of Heading's 14pt
        QCOMPARE(h1.value("fontFamily").toString(), QStringLiteral("Arial"));
        QCOMPARE(h1.value("keepWithNext").toBool(), true);
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("lineHeight").toDouble(), 1.15);
        // The document default IS Default Paragraph Style — kept, but the editor's font shows.
        const QVariantMap standard = m_fmt->styleProperties(QStringLiteral("Default Paragraph Style"));
        QCOMPARE(standard.value("fontSize").toDouble(), 12.0);
        QCOMPARE(standard.value("fontFamily").toString(), QStringLiteral("Times New Roman"));
        QVERIFY(!charAt(doc(), 9).hasProperty(QTextFormat::FontPointSize));
        QVERIFY(!charAt(doc(), 9).hasProperty(QTextFormat::FontFamilies));
        QCOMPARE(charAt(doc(), 2).fontFamilies().toStringList().value(0), QStringLiteral("Arial"));

        m_fmt->setCursorPosition(1);
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Heading 1"));
        QCOMPARE(m_fmt->heading(), 1);
        m_fmt->setCursorPosition(9);
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("First paragraph"));
        // nested spans: the automatic T1 inside Emphasis is Emphasis + direct bold
        QVariantMap both = m_fmt->directFormattingAt(18);
        QCOMPARE(both.value("charStyle").toString(), QStringLiteral("Emphasis"));
        QCOMPARE(both.value("character").toMap(), (QVariantMap { { "fontWeight", 700 } }));
        QVERIFY(charAt(doc(), 18).fontItalic());
        QVERIFY(charAt(doc(), 22).fontItalic()); // back in Emphasis after the inner span
        QCOMPARE(m_fmt->directFormattingAt(22).value("character").toMap(), QVariantMap());
        QVERIFY(!charAt(doc(), 28).fontItalic()); // "plain"
        // the automatic paragraph style: its named parent + the direct alignment
        const int right = doc()->findBlockByNumber(2).position();
        QCOMPARE(m_fmt->directFormattingAt(right).value("paragraphStyle").toString(), QStringLiteral("Text Body"));
        QCOMPARE(m_fmt->directFormattingAt(right).value("paragraph").toMap(), (QVariantMap { { "alignment", "right" } }));

        // Enter after the heading: its next style.
        m_fmt->setCursorPosition(7);
        m_fmt->insertParagraph();
        QCOMPARE(rt::paragraphStyleOf(doc()->findBlockByNumber(1)), QStringLiteral("Text Body"));
    }

    void filesFromQtsWriterStillLoad()
    {
        // .odt files saved before our writer (Qt's QTextDocumentWriter): headings as styled text:p.
        QTextDocument old;
        QTextCursor c(&old);
        QTextCharFormat big;
        big.setFontPointSize(20);
        big.setFontWeight(QFont::Bold);
        c.insertText(QStringLiteral("Old heading"), big);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        c.insertText(QStringLiteral("item"));
        c.createList(QTextListFormat::ListDisc);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        c.insertTable(1, 2);
        const QString odt = m_dir.filePath(QStringLiteral("qtwriter.odt"));
        QTextDocumentWriter w(odt, "odf");
        QVERIFY(w.write(&old));
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QVERIFY2(m_fmt->plainText().startsWith(QStringLiteral("Old heading\nitem")), qPrintable(m_fmt->plainText()));
        QCOMPARE(charAt(doc(), 1).fontPointSize(), 20.0);
        QCOMPARE(m_fmt->directFormattingAt(1).value("character").toMap().value("fontSize").toDouble(), 20.0);
        QCOMPARE(countTables(doc()), 1);
    }

    void bookRoundTripKeepsEveryCharacterAndStyle()
    {
        // A 300-page LibreOffice book (headings, footnotes, tables, lists); RICHTEXT_BOOK overrides.
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK", QStringLiteral("/home/caetano/lab/.wt/office-samples/livro-300p.odt"));
        if (!QFile::exists(book))
            QSKIP("livro-300p.odt sample not on this machine");
        QElapsedTimer t;
        t.start();
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(book)));
        const qint64 loadMs = t.restart();
        const QString text = m_fmt->plainText();
        QVERIFY(text.size() > 900000);
        const QStringList paraStyles = m_fmt->styleNames(QStringLiteral("paragraph"));
        const QStringList charStyles = m_fmt->styleNames(QStringLiteral("character"));
        QVERIFY(paraStyles.contains(QStringLiteral("First paragraph")));
        QStringList blocks;
        QList<int> levels;
        for (QTextBlock b = doc()->begin(); b.isValid(); b = b.next()) {
            blocks << rt::paragraphStyleOf(b);
            levels << b.blockFormat().headingLevel();
        }
        QVERIFY(levels.count(1) > 10 && levels.count(2) > 10);
        QVERIFY(blocks.count(QStringLiteral("Text Body")) > 1000);

        const QString out = m_dir.filePath(QStringLiteral("livro-again.odt"));
        t.restart();
        QVERIFY2(m_fmt->saveOdf(QUrl(out)), qPrintable(m_fmt->errorString()));
        const qint64 saveMs = t.restart();
        keepForInspection(out);
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(out)));
        const qint64 reloadMs = t.elapsed();
        qInfo("%s: load %lld ms, save %lld ms, reload %lld ms; %lld chars, text hash %zx, %lld blocks",
              qPrintable(QFileInfo(book).fileName()), loadMs, saveMs, reloadMs, qint64(text.size()),
              size_t(qHash(text)), qint64(blocks.size()));
        QCOMPARE(m_fmt->plainText().size(), text.size());
        QVERIFY(m_fmt->plainText() == text);
        QCOMPARE(m_fmt->styleNames(QStringLiteral("paragraph")), paraStyles);
        QCOMPARE(m_fmt->styleNames(QStringLiteral("character")), charStyles);
        QStringList blocks2;
        QList<int> levels2;
        for (QTextBlock b = doc()->begin(); b.isValid(); b = b.next()) {
            blocks2 << rt::paragraphStyleOf(b);
            levels2 << b.blockFormat().headingLevel();
        }
        QCOMPARE(blocks2, blocks);
        QCOMPARE(levels2, levels);
    }

    void nestedListsNumberingAndMergedCellsRoundTrip()
    {
        QTextCursor c(doc());
        c.insertText(QStringLiteral("one"));
        QTextListFormat outerFmt;
        outerFmt.setStyle(QTextListFormat::ListDecimal);
        outerFmt.setIndent(1);
        QTextList *outer = c.createList(outerFmt);
        c.insertBlock();
        c.insertText(QStringLiteral("two"));
        c.insertBlock();
        c.insertText(QStringLiteral("two-a"));
        QTextListFormat innerFmt;
        innerFmt.setStyle(QTextListFormat::ListCircle);
        innerFmt.setIndent(2);
        c.createList(innerFmt);
        c.insertBlock();
        c.insertText(QStringLiteral("three"));
        outer->add(c.block()); // back to the outer list: its numbering continues
        c.insertBlock(QTextBlockFormat());
        c.insertText(QStringLiteral("after"));
        QTextTable *t = c.insertTable(2, 3, rt::wordLikeTableFormat());
        t->mergeCells(0, 0, 1, 2);
        t->cellAt(0, 0).firstCursorPosition().insertText(QStringLiteral("wide"));
        t->cellAt(1, 2).firstCursorPosition().insertText(QStringLiteral("corner"));

        const QString odt = m_dir.filePath(QStringLiteral("lists.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(odt)));
        keepForInspection(odt);
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QVERIFY2(m_fmt->plainText().startsWith(QStringLiteral("one\ntwo\ntwo-a\nthree\nafter")), qPrintable(m_fmt->plainText()));
        const QTextBlock one = doc()->findBlockByNumber(0);
        const QTextBlock twoA = doc()->findBlockByNumber(2);
        const QTextBlock three = doc()->findBlockByNumber(3);
        QVERIFY(one.textList() && twoA.textList() && three.textList());
        QCOMPARE(one.textList()->format().style(), QTextListFormat::ListDecimal);
        QCOMPARE(twoA.textList()->format().style(), QTextListFormat::ListCircle);
        QCOMPARE(twoA.textList()->format().indent(), 2);
        QCOMPARE(three.textList(), one.textList());
        QCOMPARE(three.textList()->itemNumber(three), 2);
        QVERIFY(!doc()->findBlockByNumber(4).textList());
        QTextTable *back = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().value(0));
        QVERIFY(back);
        QCOMPARE(back->columns(), 3);
        QCOMPARE(back->cellAt(0, 0).columnSpan(), 2);
        QCOMPARE(back->cellAt(1, 2).firstCursorPosition().block().text(), QStringLiteral("corner"));
    }

    void saveErrorIsReported()
    {
        m_fmt->insertText(QStringLiteral("x"));
        QVERIFY(!m_fmt->saveOdf(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("no/such/dir/x.odt")))));
        QVERIFY(!m_fmt->errorString().isEmpty());
        QCOMPARE(m_fmt->isModified(), true);
    }

    // ── office item 2b, ★2: zoom ──────────────────────────────────────────────────────────────────

    void zoomDefaultsToOneAndClamps()
    {
        QCOMPARE(m_fmt->zoom(), 1.0);
        m_fmt->setZoom(5.0);
        QCOMPARE(m_fmt->zoom(), 3.0);
        m_fmt->setZoom(0.1);
        QCOMPARE(m_fmt->zoom(), 0.5);
    }

    void zoomRescalesFontSizeInPlace()
    {
        // Plain Default Paragraph Style text carries NO explicit QTextCharFormat::FontPointSize
        // by design (the editor's own font stands in for it — see docs: "Default Paragraph
        // Style's fontFamily/fontSize"), so it has nothing for the handler to rescale; that path
        // is the QML layer's _editor.font.pixelSize, scaled by `zoom` in DocumentEditor.qml. A
        // style that DOES set fontSize (a heading) is what exercises rescaleRange() here.
        m_fmt->insertText(QStringLiteral("Title"));
        m_fmt->setHeading(1);
        const qreal before = charAt(doc(), 0).fontPointSize();
        QVERIFY(before > 0);
        m_fmt->setZoom(2.0);
        QCOMPARE(charAt(doc(), 0).fontPointSize(), before * 2.0);
        m_fmt->setZoom(1.0);
        QCOMPARE(charAt(doc(), 0).fontPointSize(), before);
    }

    void zoomDoesNotChangeStyleProperties()
    {
        // The canonical style sheet is never scaled — only the live document's actual formats are.
        const qreal before = m_fmt->styleProperties(QStringLiteral("Heading 1")).value("fontSize").toDouble();
        QVERIFY(before > 0);
        m_fmt->setZoom(2.0);
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Heading 1")).value("fontSize").toDouble(), before);
    }

    void formattingAfterZoomKeepsTheZoomedSize()
    {
        // restyle() always rebases from m_sheet (canonical, zoom-1 units): a style change made
        // while zoomed must come back up to the current zoom, not silently drop to zoom 1.
        m_fmt->insertText(QStringLiteral("Title"));
        m_fmt->setZoom(2.0);
        m_fmt->setHeading(1); // applyParagraphStyle() → restyle()
        const qreal canonical = m_fmt->styleProperties(QStringLiteral("Heading 1")).value("fontSize").toDouble();
        QCOMPARE(charAt(doc(), 0).fontPointSize(), canonical * 2.0);
    }

    void zoomChangeIsUndoableNotAHistoryWipe()
    {
        m_fmt->insertText(QStringLiteral("x"));
        QVERIFY(doc()->isUndoAvailable());
        m_fmt->setZoom(2.0); // must not disable/re-enable undo like a load or newDocument does
        QVERIFY(doc()->isUndoAvailable());
    }

    void saveOdfIsIdenticalAtAnyZoom()
    {
        m_fmt->insertText(QStringLiteral("Title"));
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral("\nBody text"));
        m_fmt->setAlignment(QStringLiteral("center"));
        const QString png = m_dir.filePath(QStringLiteral("z.png"));
        QImage img(1000, 1000, QImage::Format_RGB32); // over the 480px cap: insertImage sets an
        img.fill(Qt::blue);                           // explicit width/height, so zoom has something
        QVERIFY(img.save(png));                        // to rescale (and the writer to unscale back)
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        QVERIFY(m_fmt->insertImage(QUrl(png)));

        const QString at1 = m_dir.filePath(QStringLiteral("zoom1.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(at1)));

        m_fmt->setZoom(2.0);
        const QString at2 = m_dir.filePath(QStringLiteral("zoom2.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(at2)));

        QCOMPARE(zipEntry(at2, QStringLiteral("content.xml")), zipEntry(at1, QStringLiteral("content.xml")));
        QCOMPARE(zipEntry(at2, QStringLiteral("styles.xml")), zipEntry(at1, QStringLiteral("styles.xml")));
    }

    void loadAndNewDocumentApplyTheCurrentZoom()
    {
        m_fmt->setZoom(2.0);
        m_fmt->newDocument();
        QCOMPARE(m_fmt->zoom(), 2.0);
        QCOMPARE(m_fmt->isModified(), false);
        QVERIFY(!doc()->isUndoAvailable()); // still "not an undo step" at zoom != 1

        m_fmt->insertText(QStringLiteral("x"));
        const qreal zoomedSize = charAt(doc(), 0).fontPointSize();
        const QString odt = m_dir.filePath(QStringLiteral("zoomed-load.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(odt)));

        m_fmt->setZoom(1.0);
        m_fmt->newDocument();
        m_fmt->setZoom(2.0);
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QCOMPARE(m_fmt->isModified(), false);
        QVERIFY(!doc()->isUndoAvailable());
        QCOMPARE(charAt(doc(), 0).fontPointSize(), zoomedSize);
    }

    // ── office item 2b, ★4: character format ─────────────────────────────────────────────────────

    void fontFamilyAndSizeDefaultToUnset()
    {
        m_fmt->insertText(QStringLiteral("x"));
        m_fmt->setCursorPosition(1);
        QCOMPARE(m_fmt->fontFamily(), QString()); // Default Paragraph Style sets neither (★2 docs)
        QCOMPARE(m_fmt->fontSize(), 0.0);
    }

    void setFontFamilyAndSizeApplyToSelection()
    {
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->setFontFamily(QStringLiteral("Monospace"));
        m_fmt->setFontSize(16.0);
        QCOMPARE(m_fmt->fontFamily(), QStringLiteral("Monospace"));
        QCOMPARE(m_fmt->fontSize(), 16.0);
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 16.0);
        const QStringList families = charAt(doc(), 2).property(QTextFormat::FontFamilies).toStringList();
        QCOMPARE(families, QStringList { QStringLiteral("Monospace") });
    }

    void fontSizeStaysCanonicalAcrossZoom()
    {
        // Like a word processor's font-size box: the same number at any zoom, even though the
        // live, on-screen point size (fontPointSize()) is baked in at the zoom in effect.
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->setZoom(2.0);
        m_fmt->setFontSize(14.0);
        QCOMPARE(m_fmt->fontSize(), 14.0);
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 28.0);
        m_fmt->setZoom(1.0);
        QCOMPARE(m_fmt->fontSize(), 14.0);
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 14.0);
    }

    // fontFamily()/fontSize() must report the EFFECTIVE value: direct formatting over the
    // resolved style chain, including a Default Paragraph Style the APP itself customized via
    // newDocument(options) — restyle()'s skipDocumentFont only governs what gets baked into the
    // live document (so the editor's own base font shows through when nothing was customized),
    // not what a reader should see.
    void fontFamilyAndSizeReportTheCustomizedDefaultParagraphStyle()
    {
        const QVariantMap result = m_fmt->newDocument(QVariantMap {
            { QStringLiteral("styles"), QVariantMap {
                { QStringLiteral("Default Paragraph Style"), QVariantMap {
                    { QStringLiteral("fontFamily"), QStringLiteral("Noto Serif") },
                    { QStringLiteral("fontSize"), 12.0 },
                } },
            } },
            { QStringLiteral("paragraphStyle"), QStringLiteral("Text Body") },
        });
        QVERIFY2(result.value(QStringLiteral("ok")).toBool(), qPrintable(result.value(QStringLiteral("error")).toString()));
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setCursorPosition(1);
        QCOMPARE(m_fmt->fontFamily(), QStringLiteral("Noto Serif"));
        QCOMPARE(m_fmt->fontSize(), 12.0);

        // In CANONICAL units — zoom must not change what this reports, same as a direct override.
        m_fmt->setZoom(2.0);
        QCOMPARE(m_fmt->fontSize(), 12.0);
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 0.0); // nothing baked onto the run itself (★2 docs)
    }

    // A caret in an EMPTY paragraph has no run to read: the caret's own format (the block's char
    // format) over the paragraph's style chain answers — what the next typed character will get.
    void fontFamilyAndSizeAtACaretInAnEmptyParagraph()
    {
        const QVariantMap result = m_fmt->newDocument(QVariantMap {
            { QStringLiteral("styles"), QVariantMap {
                { QStringLiteral("Default Paragraph Style"), QVariantMap {
                    { QStringLiteral("fontFamily"), QStringLiteral("Noto Serif") },
                    { QStringLiteral("fontSize"), 12.0 },
                } },
            } },
            { QStringLiteral("paragraphStyle"), QStringLiteral("Text Body") },
        });
        QVERIFY2(result.value(QStringLiteral("ok")).toBool(), qPrintable(result.value(QStringLiteral("error")).toString()));
        m_fmt->setCursorPosition(0);
        QCOMPARE(m_fmt->fontFamily(), QStringLiteral("Noto Serif"));
        QCOMPARE(m_fmt->fontSize(), 12.0);

        // A size chosen before typing (next-typed format) is what the box shows, in canonical units.
        m_fmt->setZoom(2.0);
        m_fmt->setFontSize(20.0);
        QCOMPARE(m_fmt->fontSize(), 20.0);
        QCOMPARE(m_fmt->fontFamily(), QStringLiteral("Noto Serif"));
        // ...and the next typed character really takes it (baked at the zoom in effect).
        m_fmt->insertText(QStringLiteral("x"));
        QCOMPARE(charAt(doc(), 0).fontPointSize(), 40.0);
    }

    void fontFamilyAndSizeFallBackToUnsetWhenDefaultParagraphStyleDefinesNeither()
    {
        // Unmodified built-in Default Paragraph Style: unchanged from before this fix — "" / 0,
        // the editor's own base font standing in (this handler has no way to know what that is).
        m_fmt->insertText(QStringLiteral("x"));
        m_fmt->setCursorPosition(1);
        QCOMPARE(m_fmt->fontFamily(), QString());
        QCOMPARE(m_fmt->fontSize(), 0.0);
    }

    void fontFamilyAndSizeAreBlankOverAMixedSelection()
    {
        // Word's "blank box" convention: a selection whose runs don't all agree reports "" / 0,
        // not whichever run happens to be last.
        m_fmt->insertText(QStringLiteral("AB"));
        m_fmt->setSelectionStart(1);
        m_fmt->setSelectionEnd(2); // "B"
        m_fmt->setFontSize(20.0);
        m_fmt->setFontFamily(QStringLiteral("Monospace"));

        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(2); // "AB": "A" has nothing direct, "B" has a 20pt Monospace override
        QCOMPARE(m_fmt->fontFamily(), QString());
        QCOMPARE(m_fmt->fontSize(), 0.0);

        // Each one alone is still reported correctly (sanity: the mixed check isn't just always blank).
        m_fmt->setSelectionStart(1);
        m_fmt->setSelectionEnd(2);
        QCOMPARE(m_fmt->fontFamily(), QStringLiteral("Monospace"));
        QCOMPARE(m_fmt->fontSize(), 20.0);
    }

    void clearFormattingDropsDirectButKeepsCharStyle()
    {
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        QVERIFY(m_fmt->applyCharStyle(QStringLiteral("Strong Emphasis")));
        m_fmt->setFontSize(20.0); // direct, on top of the character style
        m_fmt->setAlignment(QStringLiteral("center")); // a PARAGRAPH property — must survive
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 20.0);

        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->clearFormatting();

        m_fmt->setCursorPosition(2);
        QCOMPARE(m_fmt->currentCharStyle(), QStringLiteral("Strong Emphasis")); // kept
        QVERIFY(m_fmt->bold()); // "Strong Emphasis" is bold — still applied through the style
        QVERIFY2(!qFuzzyCompare(charAt(doc(), 2).fontPointSize(), 20.0),
                  "direct fontSize must be dropped"); // the direct override is gone
        QCOMPARE(m_fmt->alignment(), QStringLiteral("center")); // paragraph property untouched
    }

    // ── office item 2b, ★3: outline ──────────────────────────────────────────────────────────────

    void headingsEmptyOnAFreshDocument()
    {
        QCOMPARE(m_fmt->headings(), QVariantList());
        QCOMPARE(m_fmt->currentHeadingIndex(), -1);
    }

    static QVariantMap headingAt(const QVariantList &list, int i)
    {
        return list.at(i).toMap();
    }

    void headingsCollectsLevelsInDocumentOrder()
    {
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral("\nBody text"));
        m_fmt->setCursorPosition(15);
        m_fmt->insertText(QStringLiteral("\nChapter"));
        m_fmt->setCursorPosition(16);
        m_fmt->setHeading(2);

        const QVariantList h = m_fmt->headings();
        QCOMPARE(h.size(), 2);
        QCOMPARE(headingAt(h, 0).value("level").toInt(), 1);
        QCOMPARE(headingAt(h, 0).value("text").toString(), QStringLiteral("Intro"));
        QCOMPARE(headingAt(h, 1).value("level").toInt(), 2);
        QCOMPARE(headingAt(h, 1).value("text").toString(), QStringLiteral("Chapter"));
        QVERIFY(headingAt(h, 0).value("position").toInt() < headingAt(h, 1).value("position").toInt());
    }

    void headingsChangedFiresOnlyWhenTheOutlineActuallyDiffers()
    {
        QSignalSpy spy(m_fmt, &RichTextHandler::headingsChanged);
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1); // Default Paragraph Style → Heading 1: the outline gains an entry
        QVERIFY(spy.count() >= 1);

        spy.clear();
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral(", more")); // still inside the SAME heading block
        QCOMPARE(m_fmt->headings().size(), 1);
        QVERIFY2(spy.count() >= 1, "the heading's own text changed — still a real outline change");

        spy.clear();
        m_fmt->setCursorPosition(m_fmt->document()->textDocument()->characterCount() - 1);
        m_fmt->insertText(QStringLiteral("\nplain body text, no heading"));
        QCOMPARE(m_fmt->headings().size(), 1); // unchanged outline
        QCOMPARE(spy.count(), 0); // must NOT fire for an edit that doesn't touch any heading
    }

    void headingRemovedWhenItsBlockIsDeleted()
    {
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5); // setHeading() does not move the cursor itself
        m_fmt->insertText(QStringLiteral("\nBody"));
        QCOMPARE(m_fmt->headings().size(), 1);

        // Select-and-delete the heading paragraph entirely.
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(6); // "Intro\n"
        m_fmt->insertText(QString()); // typing nothing over a selection removes it
        QCOMPARE(m_fmt->headings().size(), 0);
    }

    void currentHeadingIndexTracksTheCaret()
    {
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral("\nBody\nChapter"));
        m_fmt->setCursorPosition(11);
        m_fmt->setHeading(2);

        m_fmt->setCursorPosition(0);
        QCOMPARE(m_fmt->currentHeadingIndex(), 0); // at the first heading itself
        m_fmt->setCursorPosition(8); // inside "Body" — after H1, before H2
        QCOMPARE(m_fmt->currentHeadingIndex(), 0);
        m_fmt->setCursorPosition(m_fmt->document()->textDocument()->characterCount() - 1);
        QCOMPARE(m_fmt->currentHeadingIndex(), 1); // inside/after the chapter heading
    }

    void outlineOnThe300PageBook()
    {
        // Cheap, incremental: a load does one full scan (infrequent, not a keystroke — see
        // docs/document-editor.md "Outline"); logged here alongside a handful of edits to show
        // the per-edit cost stays small on a large document, not proportional to its size.
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK", QStringLiteral("/home/caetano/lab/.wt/office-samples/livro-300p.odt"));
        if (!QFile::exists(book))
            QSKIP("livro-300p.odt sample not on this machine");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(book)));
        const QVariantList h = m_fmt->headings();
        QVERIFY(h.size() > 10);

        QElapsedTimer t;
        t.start();
        const int last = m_fmt->document()->textDocument()->characterCount() - 1;
        for (int i = 0; i < 50; ++i) {
            m_fmt->setCursorPosition(qBound(0, last / 2 + i, last));
            m_fmt->insertText(QStringLiteral("x"));
        }
        const qint64 editMs = t.elapsed();
        QCOMPARE(m_fmt->headings().size(), h.size()); // the edits touched no heading
        qInfo("outlineOnThe300PageBook: %d headings, 50 mid-document edits in %lld ms (%.2f ms/edit)",
              int(h.size()), editMs, editMs / 50.0);
    }

    // ── office item 2b, item 5: paragraph ────────────────────────────────────────────────────────

    void numberedListIsMutuallyExclusiveWithBulletList()
    {
        m_fmt->insertText(QStringLiteral("one"));
        m_fmt->setCursorPosition(1);
        QVERIFY(!m_fmt->bulletList() && !m_fmt->numberedList());

        m_fmt->toggleNumberedList();
        QVERIFY(m_fmt->numberedList());
        QVERIFY(!m_fmt->bulletList());
        QCOMPARE(doc()->findBlockByNumber(0).textList()->format().style(), QTextListFormat::ListDecimal);

        m_fmt->toggleNumberedList(); // mirrors toggleBulletList(): in a list → detach
        QVERIFY(!m_fmt->numberedList() && !m_fmt->bulletList());

        m_fmt->toggleBulletList();
        QVERIFY(m_fmt->bulletList() && !m_fmt->numberedList());
        m_fmt->toggleNumberedList(); // already in A list (bullet) → detaches, does not convert
        QVERIFY(!m_fmt->bulletList() && !m_fmt->numberedList());
    }

    void numberedListRoundTripsThroughOdf()
    {
        m_fmt->insertText(QStringLiteral("one"));
        m_fmt->setCursorPosition(1);
        m_fmt->toggleNumberedList();
        const QString odt = m_dir.filePath(QStringLiteral("numbered.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(odt)));
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QTextList *l = doc()->findBlockByNumber(0).textList();
        QVERIFY(l);
        QCOMPARE(l->format().style(), QTextListFormat::ListDecimal);
    }

    void indentOutdentStepTheParagraphMarginOutsideAList()
    {
        m_fmt->insertText(QStringLiteral("x"));
        m_fmt->setCursorPosition(1);
        const qreal before = blockFormat(doc(), 0).leftMargin();
        m_fmt->indent();
        const qreal after1 = blockFormat(doc(), 0).leftMargin();
        QVERIFY(after1 > before);
        m_fmt->indent();
        QVERIFY(blockFormat(doc(), 0).leftMargin() > after1);
        m_fmt->outdent();
        QCOMPARE(blockFormat(doc(), 0).leftMargin(), after1);
    }

    void indentStepsScaleWithZoom()
    {
        m_fmt->insertText(QStringLiteral("x"));
        m_fmt->setCursorPosition(1);
        m_fmt->setZoom(2.0);
        const qreal before = blockFormat(doc(), 0).leftMargin();
        m_fmt->indent();
        QCOMPARE(blockFormat(doc(), 0).leftMargin() - before, 48.0); // 24px step * zoom 2
    }

    void indentOutdentChangeListLevel()
    {
        m_fmt->insertText(QStringLiteral("x"));
        m_fmt->setCursorPosition(1);
        m_fmt->toggleBulletList();
        QCOMPARE(doc()->findBlockByNumber(0).textList()->format().indent(), 1);
        m_fmt->indent();
        QTextList *l1 = doc()->findBlockByNumber(0).textList();
        QVERIFY(l1);
        QCOMPARE(l1->format().indent(), 2);
        m_fmt->outdent();
        QTextList *l0 = doc()->findBlockByNumber(0).textList();
        QVERIFY(l0);
        QCOMPARE(l0->format().indent(), 1);
        m_fmt->outdent(); // past the first level: detaches
        QVERIFY(!doc()->findBlockByNumber(0).textList());
    }

    void insertPageBreakSetsThePolicyOnTheNextParagraph()
    {
        m_fmt->insertText(QStringLiteral("one"));
        m_fmt->setCursorPosition(3);
        m_fmt->insertPageBreak();
        m_fmt->setCursorPosition(m_fmt->document()->textDocument()->characterCount() - 1);
        m_fmt->insertText(QStringLiteral("two"));
        QCOMPARE(doc()->blockCount(), 2);
        QCOMPARE(blockFormat(doc(), 0).pageBreakPolicy(), QTextFormat::PageBreak_Auto);
        QCOMPARE(blockFormat(doc(), 1).pageBreakPolicy(), QTextFormat::PageBreak_AlwaysBefore);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("one\ntwo"));
    }

    void insertPageBreakRoundTripsThroughOdf()
    {
        m_fmt->insertText(QStringLiteral("one"));
        m_fmt->setCursorPosition(3);
        m_fmt->insertPageBreak();
        m_fmt->setCursorPosition(m_fmt->document()->textDocument()->characterCount() - 1);
        m_fmt->insertText(QStringLiteral("two"));
        const QString odt = m_dir.filePath(QStringLiteral("pagebreak.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(odt)));
        QVERIFY2(zipEntry(odt, QStringLiteral("content.xml")).contains("fo:break-before=\"page\""),
                 "our writer must emit fo:break-before=\"page\"");

        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QCOMPARE(doc()->blockCount(), 2);
        QCOMPARE(blockFormat(doc(), 0).pageBreakPolicy(), QTextFormat::PageBreak_Auto);
        QCOMPARE(blockFormat(doc(), 1).pageBreakPolicy(), QTextFormat::PageBreak_AlwaysBefore);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("one\ntwo"));
    }

    void pageBreakSurvivesZoom()
    {
        m_fmt->insertText(QStringLiteral("one"));
        m_fmt->setCursorPosition(3);
        m_fmt->insertPageBreak();
        m_fmt->setCursorPosition(m_fmt->document()->textDocument()->characterCount() - 1);
        m_fmt->insertText(QStringLiteral("two"));
        m_fmt->setZoom(2.0);
        QCOMPARE(blockFormat(doc(), 1).pageBreakPolicy(), QTextFormat::PageBreak_AlwaysBefore);
        const QString at1 = m_dir.filePath(QStringLiteral("pb-zoom1.odt"));
        m_fmt->setZoom(1.0);
        QVERIFY(m_fmt->saveOdf(QUrl(at1)));
        m_fmt->setZoom(2.0);
        const QString at2 = m_dir.filePath(QStringLiteral("pb-zoom2.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(at2)));
        QCOMPARE(zipEntry(at2, QStringLiteral("content.xml")), zipEntry(at1, QStringLiteral("content.xml")));
    }

    // ── office item 2b, item 6: clipboard ────────────────────────────────────────────────────────

    void copyPutsHtmlAndPlainTextOnTheClipboard()
    {
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->copy();
        const QMimeData *data = QGuiApplication::clipboard()->mimeData();
        QVERIFY(data);
        QVERIFY(data->hasText());
        QVERIFY(data->hasHtml());
        QCOMPARE(data->text(), QStringLiteral("Hello"));
        QVERIFY2(data->html().contains(QStringLiteral("Hello")), qPrintable(data->html()));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Hello")); // copy() does not touch the document
    }

    void copyWithoutASelectionIsANoOp()
    {
        m_fmt->insertText(QStringLiteral("x"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        QGuiApplication::clipboard()->clear();
        m_fmt->copy();
        // QClipboard::mimeData() is not guaranteed non-null after clear() on every platform
        // (observed null under offscreen) — go through the guarded, production canPaste() rather
        // than dereference it directly.
        QVERIFY(!m_fmt->canPaste());
    }

    void cutRemovesTheSelectionAndCopiesIt()
    {
        m_fmt->insertText(QStringLiteral("Hello world"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(6); // "Hello "
        m_fmt->cut();
        QCOMPARE(m_fmt->plainText(), QStringLiteral("world"));
        const QMimeData *data = QGuiApplication::clipboard()->mimeData();
        QVERIFY(data);
        QCOMPARE(data->text(), QStringLiteral("Hello "));
    }

    void pasteInsertsAtTheCaretAndReplacesASelection()
    {
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->copy();
        QVERIFY(m_fmt->canPaste());

        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral(" world"));
        m_fmt->setCursorPosition(6); // right after "Hello " (" world" starts with a space)
        m_fmt->paste();
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Hello Helloworld"));

        // Replaces a selection, like typed text does.
        m_fmt->setSelectionStart(11);
        m_fmt->setSelectionEnd(16); // "world"
        m_fmt->paste();
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Hello HelloHello"));
    }

    void pasteIsANoOpWhenTheClipboardHasNothingPasteable()
    {
        QGuiApplication::clipboard()->clear();
        QVERIFY(!m_fmt->canPaste());
        m_fmt->insertText(QStringLiteral("x"));
        m_fmt->setCursorPosition(1);
        m_fmt->paste();
        QCOMPARE(m_fmt->plainText(), QStringLiteral("x"));
    }

    // ── office item 2b, ★2 follow-up: zoom vs. content entering/leaving the live document ───────
    //
    // 2a87a2c made zoom a real relayout of content ALREADY in the document, but anything crossing
    // the document's boundary (clipboard paste/copy, a direct insertHtml) was left at face value:
    // a 12pt paste at zoom 2 was saved as 6pt (data loss); a copy at zoom 2 put doubled sizes on
    // the clipboard. These tests pin the fix: entry is scaled UP to the zoom in effect (like typed
    // text), exit is scaled back DOWN to canonical, so the clipboard and the saved file are always
    // zoom-independent, at any zoom, in either direction.

    void insertHtmlAtZoomRescalesLikeTypedText()
    {
        m_fmt->setZoom(2.0);
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        m_fmt->insertHtml(QStringLiteral("<p style=\"font-size:12pt;\">Direct</p>"));
        const int pos = m_fmt->plainText().indexOf(QStringLiteral("Direct"));
        QVERIFY(pos >= 0);
        QCOMPARE(charAt(doc(), pos).fontPointSize(), 24.0); // baked in at zoom 2, like setFontSize does
    }

    void insertHtmlAtZoomRescalesANewBlocksMargin()
    {
        // A block-level length (margin) declared by the pasted HTML itself, as opposed to a
        // character-level one (fontSize above) — exercises rescaleInsertedRange's block-format
        // path. Qt's own HTML importer only understands margin units it can convert to px — not
        // cm/pt/in (verified against Qt 6.11's QCss::ValueExtractor::lengthValue, which only
        // recognizes "px"/"ex"/"em" and silently reads anything else as 0) — so the probe itself
        // uses px; that is a pre-existing Qt limitation, unrelated to this fix. Pasting into the
        // lone empty paragraph of a fresh document inserts at position 0, where Qt's own
        // QTextCursor::insertFragment overwrites even that (merged-into) block's format from the
        // pasted source — see rescaleInsertedRange's doc comment.
        const QString html = QStringLiteral("<p style=\"margin-left:48px;\">Indented</p>");
        m_fmt->setCursorPosition(0);
        m_fmt->insertHtml(html);
        QCOMPARE(blockFormat(doc(), 0).leftMargin(), 48.0);

        m_fmt->newDocument();
        m_fmt->setZoom(2.0);
        m_fmt->setCursorPosition(0);
        m_fmt->insertHtml(html);
        QCOMPARE(blockFormat(doc(), 0).leftMargin(), 96.0);
    }

    void insertHtmlAtZoomDoesNotRescaleThePreexistingFirstBlock()
    {
        // A multi-paragraph paste in the middle of existing text: per
        // QTextCopyHelper::appendFragment (qtextdocumentfragment.cpp), Qt only ever overwrites
        // the format of a block the paste itself just started (every block strictly after the
        // first one touched, even when its tail end absorbs pre-existing text past the LAST
        // pasted paragraph) — the ONE block the paste merely merges inline into at its start
        // keeps that block's own, already-correctly-zoomed format untouched.
        m_fmt->insertText(QStringLiteral("HeadTail"));
        m_fmt->setCursorPosition(4);
        m_fmt->indent(); // gives the lone paragraph a non-zero, non-pasted-looking margin
        const qreal headMarginAtZoom1 = blockFormat(doc(), 0).leftMargin();
        QVERIFY(headMarginAtZoom1 > 0);

        m_fmt->setZoom(2.0);
        const qreal headMarginAtZoom2 = blockFormat(doc(), 0).leftMargin(); // setZoom already rescaled it
        QCOMPARE(headMarginAtZoom2, headMarginAtZoom1 * 2.0);

        m_fmt->setCursorPosition(4); // between "Head" and "Tail"
        m_fmt->insertHtml(QStringLiteral("<p style=\"margin-left:16px;\">Mid1</p><p style=\"margin-left:32px;\">Mid2</p>"));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("HeadMid1\nMid2Tail"));

        // Block 0 ("HeadMid1" — the block the paste merely merged INTO): untouched by the paste.
        QCOMPARE(blockFormat(doc(), 0).leftMargin(), headMarginAtZoom2);
        // Block 1 ("Mid2Tail" — entirely new, from the paste's LAST paragraph): the pasted
        // canonical margin (32px), rescaled UP to the zoom in effect, like everything else
        // entering the document.
        QCOMPARE(blockFormat(doc(), 1).leftMargin(), 64.0);
    }

    void pasteHtmlAtZoomSavesTheSameOdfAtAnyZoom()
    {
        const QString html = QStringLiteral("<p style=\"font-size:12pt;margin-left:48px;\">Pasted</p>");
        auto pasteAndSave = [&](qreal zoom, const QString &path) {
            m_fmt->newDocument();
            m_fmt->setZoom(zoom);
            m_fmt->setCursorPosition(doc()->characterCount() - 1);
            auto *data = new QMimeData;
            data->setHtml(html);
            QGuiApplication::clipboard()->setMimeData(data);
            m_fmt->paste();
            QVERIFY(m_fmt->saveOdf(QUrl::fromLocalFile(path)));
        };
        const QString at1 = m_dir.filePath(QStringLiteral("paste-zoom1.odt"));
        const QString at2 = m_dir.filePath(QStringLiteral("paste-zoom2.odt"));
        pasteAndSave(1.0, at1);
        pasteAndSave(2.0, at2);
        // (the blocks' ids differ: two documents)
        QCOMPARE(withoutBlockIds(zipEntry(at2, QStringLiteral("content.xml"))),
                 withoutBlockIds(zipEntry(at1, QStringLiteral("content.xml"))));
    }

    void pasteAtZoomRoundTripsToTheSourceSizeAtZoomOne()
    {
        m_fmt->setZoom(2.0);
        auto *data = new QMimeData;
        data->setHtml(QStringLiteral("<p style=\"font-size:12pt;\">Pasted</p>"));
        QGuiApplication::clipboard()->setMimeData(data);
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        m_fmt->paste();
        m_fmt->setZoom(1.0); // zoom back down: the whole document, including the paste, rescales
        const int pos = m_fmt->plainText().indexOf(QStringLiteral("Pasted"));
        QVERIFY(pos >= 0);
        QCOMPARE(charAt(doc(), pos).fontPointSize(), 12.0); // exactly the source size, not half of it
    }

    void pastePlainTextAtZoomTakesTheCaretsScaledFormat()
    {
        // Plain-text paste has no size of its own to (mis)scale — it must take whatever format is
        // already at the caret, same as typed text (insertText/insertionCursor), not fall back to
        // some canonical default.
        m_fmt->setZoom(2.0);
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->setFontSize(10.0); // canonical 10pt; live format baked at 20pt (zoom 2)
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 20.0);

        m_fmt->setSelectionStart(5);
        m_fmt->setSelectionEnd(5);
        m_fmt->setCursorPosition(5); // right after "Hello"
        auto *data = new QMimeData;
        data->setText(QStringLiteral(" world"));
        QGuiApplication::clipboard()->setMimeData(data);
        m_fmt->paste();
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Hello world"));
        QCOMPARE(charAt(doc(), 7).fontPointSize(), 20.0);
    }

    void copyAtZoomPutsUnscaledSizesOnTheClipboard()
    {
        m_fmt->setZoom(2.0);
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->setFontSize(12.0); // canonical; live format baked at 24pt (zoom 2)
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 24.0); // sanity: the live doc really is zoomed

        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->copy();
        const QString html = QGuiApplication::clipboard()->mimeData()->html();
        QVERIFY2(html.contains(QStringLiteral("font-size:12pt")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("font-size:24pt")), qPrintable(html));
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 24.0); // copy() must not touch the live document
    }

    void cutAtZoomAlsoPutsUnscaledSizesOnTheClipboard()
    {
        // cut() is copy() + removeSelectedText() — confirm the unscaling isn't copy()-only.
        m_fmt->setZoom(2.0);
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->setFontSize(12.0);
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->cut();
        const QString html = QGuiApplication::clipboard()->mimeData()->html();
        QVERIFY2(html.contains(QStringLiteral("font-size:12pt")), qPrintable(html));
        QVERIFY2(!html.contains(QStringLiteral("font-size:24pt")), qPrintable(html));
    }

    void copyPasteRoundTripAtDifferentZoomsStaysAtTheSourceSize()
    {
        // Copy at zoom 2, paste into a fresh zoom-1 document: must read exactly like copying at
        // zoom 1 in the first place — the clipboard content must not carry any memory of the zoom
        // it was copied FROM.
        m_fmt->setZoom(2.0);
        m_fmt->insertText(QStringLiteral("Hello"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->setFontSize(12.0);
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(5);
        m_fmt->copy();

        m_fmt->setZoom(1.0);
        m_fmt->newDocument();
        m_fmt->setCursorPosition(0);
        m_fmt->paste();
        const int pos = m_fmt->plainText().indexOf(QStringLiteral("Hello"));
        QVERIFY(pos >= 0);
        QCOMPARE(charAt(doc(), pos).fontPointSize(), 12.0);
    }

    // ── office item 2b, item 7: counts ───────────────────────────────────────────────────────────

    void countsAreZeroOnAFreshDocument()
    {
        QCOMPARE(m_fmt->wordCount(), 0);
        QCOMPARE(m_fmt->charCount(), 0);
        QCOMPARE(m_fmt->selectionWordCount(), 0);
        QCOMPARE(m_fmt->selectionCharCount(), 0);
        QCOMPARE(m_fmt->selectedText(), QString());
    }

    void wordAndCharCountRules()
    {
        // "Hello, world! 42" — words: Hello, world, 42 (punctuation/space runs don't count).
        m_fmt->insertText(QStringLiteral("Hello, world! 42"));
        QCOMPARE(m_fmt->wordCount(), 3);
        QCOMPARE(m_fmt->charCount(), 16);
    }

    // loadOdf()/newDocument() rebuild the index (rebuildIndex()) with the CORRECT totals, but the
    // ONLY contentsChanged() emissions during the rebuild come from QTextDocument::clear()/the
    // reader's own edits — all BEFORE rebuildIndex() runs — so a live QML binding on wordCount/
    // charCount (NOTIFY contentsChanged) last reads whatever the counts were mid-rebuild, not the
    // correct final ones rebuildIndex() just computed. These check the LAST value a listener
    // actually observes, not just that some signal fired (which already happens, for the wrong
    // reason) — the same "seen.last()" shape as newDocumentNotifiesUnmodified above.
    void loadOdfNotifiesWordAndCharCountWithTheFinalValue()
    {
        const QString odt = m_dir.filePath(QStringLiteral("counts.odt"));
        writeOdt(odt, "<text:p>one two three</text:p><text:h text:outline-level=\"1\">Chapter</text:h>");

        QList<int> seenWords, seenChars;
        connect(m_fmt, &RichTextHandler::contentsChanged, this, [&] {
            seenWords << m_fmt->wordCount();
            seenChars << m_fmt->charCount();
        });
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QCOMPARE(m_fmt->headings().size(), 1);
        QVERIFY2(!seenWords.isEmpty(), "wordCount/charCount's NOTIFY must fire after a load");
        QCOMPARE(seenWords.last(), 4); // one, two, three, Chapter
        QCOMPARE(seenChars.last(), 20); // "one two three" (13) + "Chapter" (7)
    }

    void loadOdfOfASecondFileReplacesCountsRatherThanAdding()
    {
        const QString first = m_dir.filePath(QStringLiteral("first.odt"));
        const QString second = m_dir.filePath(QStringLiteral("second.odt"));
        writeOdt(first, "<text:p>one two three four five</text:p>");
        writeOdt(second, "<text:p>a b</text:p>");

        QVERIFY(m_fmt->loadOdf(QUrl(first)));
        QCOMPARE(m_fmt->wordCount(), 5);

        QList<int> seenWords;
        connect(m_fmt, &RichTextHandler::contentsChanged, this, [&] { seenWords << m_fmt->wordCount(); });
        QVERIFY(m_fmt->loadOdf(QUrl(second)));
        QVERIFY(!seenWords.isEmpty());
        QCOMPARE(seenWords.last(), 2); // replaced, not 5 + 2
        QCOMPARE(m_fmt->charCount(), 3);
    }

    void newDocumentNotifiesWordAndCharCountThenInsertTextKeepsWorking()
    {
        m_fmt->insertText(QStringLiteral("one two three"));
        QCOMPARE(m_fmt->wordCount(), 3);

        QList<int> seenWords;
        connect(m_fmt, &RichTextHandler::contentsChanged, this, [&] { seenWords << m_fmt->wordCount(); });
        m_fmt->newDocument();
        QVERIFY2(!seenWords.isEmpty(), "wordCount/charCount's NOTIFY must fire after newDocument(), even resetting to 0");
        QCOMPARE(seenWords.last(), 0);
        QCOMPARE(m_fmt->charCount(), 0);

        m_fmt->insertText(QStringLiteral("four five"));
        QCOMPARE(m_fmt->wordCount(), 2); // the normal incremental path still works after a reset
        QCOMPARE(m_fmt->charCount(), 9);
    }

    void charCountExcludesTheParagraphSeparatorNotLineBreaks()
    {
        m_fmt->insertText(QStringLiteral("one"));
        m_fmt->setCursorPosition(3);
        m_fmt->insertText(QStringLiteral("\ntwo")); // paragraph break: NOT counted
        QCOMPARE(m_fmt->charCount(), 6); // "one" + "two", not 7

        QTextCursor c(doc());
        c.setPosition(doc()->characterCount() - 1);
        c.insertText(QString(QChar::LineSeparator)); // a soft line break (Shift+Enter): IS counted
        QCOMPARE(m_fmt->charCount(), 7);
    }

    void countsUpdateIncrementallyOnEditsAnywhere()
    {
        m_fmt->insertText(QStringLiteral("one"));
        m_fmt->setCursorPosition(3);
        m_fmt->insertText(QStringLiteral("\ntwo\nthree"));
        QCOMPARE(m_fmt->wordCount(), 3);
        QCOMPARE(m_fmt->charCount(), 11); // one+two+three = 3+3+5

        // Edit the FIRST paragraph: the others' kept counts must not be touched/re-derived wrong.
        m_fmt->setCursorPosition(0);
        m_fmt->insertText(QStringLiteral("NEW "));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("NEW one\ntwo\nthree"));
        QCOMPARE(m_fmt->wordCount(), 4);
        QCOMPARE(m_fmt->charCount(), 15);

        // Delete the middle paragraph entirely (selects "two\n").
        m_fmt->setSelectionStart(8);
        m_fmt->setSelectionEnd(12);
        m_fmt->insertText(QString());
        QCOMPARE(m_fmt->plainText(), QStringLiteral("NEW one\nthree"));
        QCOMPARE(m_fmt->wordCount(), 3);
        QCOMPARE(m_fmt->charCount(), 12);
    }

    void selectionCountsAndSelectedText()
    {
        m_fmt->insertText(QStringLiteral("one two\nthree"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(13); // the whole document, across the paragraph break
        QCOMPARE(m_fmt->selectionWordCount(), 3);
        QCOMPARE(m_fmt->selectionCharCount(), 12); // 13 positions - 1 paragraph separator
        QCOMPARE(m_fmt->selectedText(), QStringLiteral("one two\nthree"));

        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(3); // "one"
        QCOMPARE(m_fmt->selectionWordCount(), 1);
        QCOMPARE(m_fmt->selectionCharCount(), 3);
        QCOMPARE(m_fmt->selectedText(), QStringLiteral("one"));
    }

    void countsOnThe300PageBook()
    {
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK", QStringLiteral("/home/caetano/lab/.wt/office-samples/livro-300p.odt"));
        if (!QFile::exists(book))
            QSKIP("livro-300p.odt sample not on this machine");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(book)));
        QVERIFY(m_fmt->charCount() > 900000);
        QVERIFY(m_fmt->wordCount() > 100000);
        // charCount excludes one separator per block; plainText() keeps them as "\n".
        QCOMPARE(m_fmt->charCount(), m_fmt->plainText().size() - (doc()->blockCount() - 1));

        QElapsedTimer t;
        t.start();
        const int last = doc()->characterCount() - 1;
        for (int i = 0; i < 50; ++i) {
            m_fmt->setCursorPosition(qBound(0, last / 2 + i, last));
            m_fmt->insertText(QStringLiteral("x"));
        }
        const qint64 editMs = t.elapsed();
        qInfo("countsOnThe300PageBook: %lld words, %lld chars, 50 mid-document edits in %lld ms (%.2f ms/edit)",
              qint64(m_fmt->wordCount()), qint64(m_fmt->charCount()), editMs, editMs / 50.0);
    }

    // ── office item 2b addendum: newDocument(options) ──────────────────────────────────────────────

    void newDocumentWithoutOptionsIsUnchanged()
    {
        m_fmt->insertText(QStringLiteral("x"));
        const QVariantMap r = m_fmt->newDocument();
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(m_fmt->plainText(), QString());
        QCOMPARE(m_fmt->isModified(), false);
    }

    void newDocumentWithOptionsDefinesStylesAndSetsTheFirstParagraph()
    {
        QVariantMap styles;
        styles.insert(QStringLiteral("Text Body"), QVariantMap { { QStringLiteral("fontSize"), 11.0 } });
        QVariantMap note;
        note.insert(QStringLiteral("parent"), QStringLiteral("Text Body"));
        note.insert(QStringLiteral("italic"), true);
        styles.insert(QStringLiteral("Note"), note);
        QVariantMap options;
        options.insert(QStringLiteral("styles"), styles);
        options.insert(QStringLiteral("paragraphStyle"), QStringLiteral("Note"));

        const QVariantMap r = m_fmt->newDocument(options);
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(r.value(QStringLiteral("error")).toString(), QString());
        QCOMPARE(m_fmt->currentParagraphStyle(), QStringLiteral("Note"));
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Text Body")).value("fontSize").toDouble(), 11.0);
        QVERIFY(m_fmt->styleProperties(QStringLiteral("Note")).value("italic").toBool());
        QCOMPARE(m_fmt->styleProperties(QStringLiteral("Note")).value("parent").toString(), QStringLiteral("Text Body"));
    }

    void newDocumentWithOptionsIsNotAnUndoStepAndIsUnmodified()
    {
        QVariantMap options;
        QVariantMap styles;
        styles.insert(QStringLiteral("Note"), QVariantMap { { QStringLiteral("italic"), true } });
        options.insert(QStringLiteral("styles"), styles);
        m_fmt->insertText(QStringLiteral("x")); // give it SOMETHING to (not) undo
        const QVariantMap r = m_fmt->newDocument(options);
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QCOMPARE(m_fmt->isModified(), false);
        QVERIFY(!doc()->isUndoAvailable());
    }

    void newDocumentWithOptionsWritesStylesToOdf()
    {
        QVariantMap options;
        QVariantMap styles;
        styles.insert(QStringLiteral("Note"), QVariantMap { { QStringLiteral("italic"), true } });
        options.insert(QStringLiteral("styles"), styles);
        QVERIFY(m_fmt->newDocument(options).value(QStringLiteral("ok")).toBool());
        const QString odt = m_dir.filePath(QStringLiteral("new-with-styles.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl(odt)));
        const QByteArray stylesXml = zipEntry(odt, QStringLiteral("styles.xml"));
        QVERIFY2(stylesXml.contains("Note"), stylesXml.constData());

        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl(odt)));
        QVERIFY(m_fmt->styleNames(QStringLiteral("paragraph")).contains(QStringLiteral("Note")));
    }

    void newDocumentWithOptionsErrorLeavesThePreviousDocumentUntouched()
    {
        m_fmt->insertText(QStringLiteral("keep me"));
        m_fmt->setHeading(1);
        QVariantMap options;
        QVariantMap styles;
        styles.insert(QStringLiteral("Broken"), QVariantMap { { QStringLiteral("notAProperty"), 1 } });
        options.insert(QStringLiteral("styles"), styles);

        QVariantMap r = m_fmt->newDocument(options);
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
        QVERIFY(!r.value(QStringLiteral("error")).toString().isEmpty());
        QCOMPARE(m_fmt->plainText(), QStringLiteral("keep me"));
        QCOMPARE(m_fmt->heading(), 1);

        // An unknown paragraphStyle is also rejected, same way.
        QVariantMap options2;
        options2.insert(QStringLiteral("paragraphStyle"), QStringLiteral("No Such Style"));
        r = m_fmt->newDocument(options2);
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("keep me"));
    }
    // ── office item 3: stable block ids ─────────────────────────────────────────────────────────

    void blockIdsAreUniqueAndStableWhileTyping()
    {
        m_fmt->insertText(QStringLiteral("one\ntwo\nthree"));
        const QStringList ids = m_fmt->blockIds();
        QCOMPARE(ids.size(), 3);
        QCOMPARE(QSet<QString>(ids.cbegin(), ids.cend()).size(), 3);
        for (const QString &id : ids)
            QVERIFY(!id.isEmpty());
        QTextCursor c(doc());
        c.setPosition(5); // inside "two"
        c.insertText(QStringLiteral("XY"));
        c.setPosition(0);
        c.insertText(QStringLiteral("Z"));
        c.setPosition(c.document()->characterCount() - 1);
        c.deletePreviousChar();
        QCOMPARE(m_fmt->blockIds(), ids);
        QCOMPARE(m_fmt->blockIdAt(0), ids[0]);
        QCOMPARE(m_fmt->blockIdAt(doc()->findBlockByNumber(1).position() + 1), ids[1]);
        QCOMPARE(m_fmt->blockText(ids[1]), QStringLiteral("tXYwo"));
        QCOMPARE(m_fmt->blockText(QStringLiteral("nope")), QString());
        QCOMPARE(m_fmt->blockIdAt(-5), QString());
    }

    void splitKeepsTheIdBeforeTheCaret()
    {
        m_fmt->insertText(QStringLiteral("Hello world"));
        const QString a = m_fmt->blockIds().value(0);
        m_fmt->setCursorPosition(5);
        m_fmt->insertParagraph();
        const QStringList ids = m_fmt->blockIds();
        QCOMPARE(ids.size(), 2);
        QCOMPARE(ids[0], a);
        QVERIFY(!ids[1].isEmpty() && ids[1] != a);
        QCOMPARE(m_fmt->blockText(a), QStringLiteral("Hello"));
        QCOMPARE(m_fmt->blockText(ids[1]), QStringLiteral(" world"));
    }

    void mergeKeepsTheSurvivingId()
    {
        m_fmt->insertText(QStringLiteral("one\ntwo"));
        const QStringList ids = m_fmt->blockIds();
        QCoreApplication::processEvents();
        QSignalSpy removed(m_fmt, &RichTextHandler::blockRemoved);
        QTextCursor c(doc());
        c.setPosition(doc()->findBlockByNumber(1).position());
        c.deletePreviousChar(); // Backspace at the start of "two"
        QCOMPARE(m_fmt->blockIds(), QStringList { ids[0] });
        QCOMPARE(m_fmt->blockText(ids[0]), QStringLiteral("onetwo"));
        QCOMPARE(removed.count(), 0); // delivered on the next turn
        QCoreApplication::processEvents();
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.at(0).at(0).toString(), ids[1]);

        // An EMPTY paragraph merged away is the one Qt removes: the text's block survives.
        m_fmt->newDocument();
        m_fmt->insertText(QStringLiteral("\ntwo"));
        const QStringList ids2 = m_fmt->blockIds();
        c = QTextCursor(doc());
        c.setPosition(doc()->findBlockByNumber(1).position());
        c.deletePreviousChar();
        QCOMPARE(m_fmt->blockIds(), QStringList { ids2[1] });
    }

    void undoRedoPutTheIdsBack()
    {
        m_fmt->insertText(QStringLiteral("Hello world"));
        const QString a = m_fmt->blockIds().value(0);
        m_fmt->setCursorPosition(5);
        m_fmt->insertParagraph();
        const QString n = m_fmt->blockIds().value(1);
        doc()->undo();
        QCOMPARE(m_fmt->blockIds(), QStringList { a });
        doc()->redo();
        QCOMPARE(m_fmt->blockIds(), (QStringList { a, n }));

        // merge, undo (the merged-away block comes back with ITS id), redo
        QTextCursor c(doc());
        c.setPosition(doc()->findBlockByNumber(1).position());
        c.deletePreviousChar();
        QCOMPARE(m_fmt->blockIds(), QStringList { a });
        doc()->undo();
        QCOMPARE(m_fmt->blockIds(), (QStringList { a, n }));
        QCOMPARE(m_fmt->blockText(n), QStringLiteral(" world"));
        doc()->redo();
        QCOMPARE(m_fmt->blockIds(), QStringList { a });

        // a deleted range of whole paragraphs, undone
        m_fmt->newDocument();
        m_fmt->insertText(QStringLiteral("p1\np2\np3\np4"));
        const QStringList four = m_fmt->blockIds();
        c = QTextCursor(doc());
        c.setPosition(1);
        c.setPosition(doc()->findBlockByNumber(3).position() + 1, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        QCOMPARE(m_fmt->blockIds(), QStringList { four[0] });
        doc()->undo();
        QCOMPARE(m_fmt->blockIds(), four);
        doc()->redo();
        QCOMPARE(m_fmt->blockIds(), QStringList { four[0] });

        // a new edit after an undo drops the redo history: the next split gets a fresh id
        doc()->undo();
        m_fmt->setCursorPosition(1);
        m_fmt->insertParagraph();
        const QStringList now = m_fmt->blockIds();
        QCOMPARE(now.size(), 5);
        QCOMPARE(QSet<QString>(now.cbegin(), now.cend()).size(), 5);
    }

    void pastedBlocksGetNewIds()
    {
        m_fmt->insertText(QStringLiteral("Hello"));
        const QString a = m_fmt->blockIds().value(0);
        m_fmt->setCursorPosition(5);
        m_fmt->insertHtml(QStringLiteral("<p>x</p><p>y</p><p>z</p>"));
        const QStringList ids = m_fmt->blockIds();
        QVERIFY(ids.size() >= 3);
        QCOMPARE(ids[0], a);
        QCOMPARE(QSet<QString>(ids.cbegin(), ids.cend()).size(), ids.size());

        // our own copy → paste: the clipboard carries no ids, the pasted blocks are new
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(doc()->characterCount() - 1);
        m_fmt->copy();
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        m_fmt->paste();
        const QStringList after = m_fmt->blockIds();
        QVERIFY(after.size() > ids.size());
        QCOMPARE(QSet<QString>(after.cbegin(), after.cend()).size(), after.size());
    }

    void blockIdsRoundTripThroughOdf()
    {
        m_fmt->insertText(QStringLiteral("one\ntwo"));
        m_fmt->setHeading(1);
        m_fmt->insertTable(1, 2);
        const QStringList ids = m_fmt->blockIds();
        const QString odt = m_dir.filePath(QStringLiteral("ids.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl::fromLocalFile(odt)));
        const QByteArray content = zipEntry(odt, QStringLiteral("content.xml"));
        for (const QString &id : ids)
            QVERIFY2(content.contains("xml:id=\"" + id.toUtf8() + '"'), content.constData());
        m_fmt->newDocument();
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(odt)));
        QCOMPARE(m_fmt->blockIds(), ids);
    }

    void odfWithoutIdsGetsGeneratedOnes()
    {
        const QString odt = m_dir.filePath(QStringLiteral("noids.odt"));
        writeOdt(odt, "<text:p xml:id=\"dup\">a</text:p><text:p xml:id=\"dup\">b</text:p><text:p>c</text:p>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(odt)));
        const QStringList ids = m_fmt->blockIds();
        QCOMPARE(ids.size(), 3);
        QCOMPARE(ids[0], QStringLiteral("dup"));
        QCOMPARE(QSet<QString>(ids.cbegin(), ids.cend()).size(), 3);
        for (const QString &id : ids)
            QVERIFY(!id.isEmpty());
    }

    void blocksLoadedChangedAndRemovedFiringRules()
    {
        QSignalSpy loaded(m_fmt, &RichTextHandler::blocksLoaded);
        QSignalSpy changed(m_fmt, &RichTextHandler::blockChanged);
        QSignalSpy removed(m_fmt, &RichTextHandler::blockRemoved);
        m_fmt->newDocument();
        QCOMPARE(loaded.count(), 1);
        m_fmt->insertText(QStringLiteral("one\ntwo"));
        const QString odt = m_dir.filePath(QStringLiteral("loaded.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl::fromLocalFile(odt)));
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(odt)));
        QCOMPARE(loaded.count(), 2);
        QCoreApplication::processEvents();
        changed.clear();
        removed.clear(); // a load replaces everything: blocksLoaded, not one removal per block
        const QStringList ids = m_fmt->blockIds();

        // three keystrokes in one block → ONE blockChanged, on the next turn
        QTextCursor c(doc());
        c.setPosition(1);
        c.insertText(QStringLiteral("a"));
        c.insertText(QStringLiteral("b"));
        c.insertText(QStringLiteral("c"));
        QCOMPARE(changed.count(), 0);
        QCoreApplication::processEvents();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.at(0).at(0).toString(), ids[0]);

        // format only: nothing
        changed.clear();
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(3);
        m_fmt->toggleBold();
        m_fmt->setAlignment(QStringLiteral("center"));
        m_fmt->setSelectionEnd(0);
        m_fmt->setZoom(1.5);
        QCoreApplication::processEvents();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(removed.count(), 0);

        // a split reports the shortened block AND the new one
        m_fmt->setCursorPosition(2);
        m_fmt->insertParagraph();
        QCoreApplication::processEvents();
        QStringList seen;
        for (const auto &args : std::as_const(changed))
            seen << args.at(0).toString();
        const QStringList now = m_fmt->blockIds();
        QCOMPARE(now.size(), 3);
        QVERIFY(seen.contains(now[0]) && seen.contains(now[1]));
        QCOMPARE(seen.size(), 2);
        m_fmt->setZoom(1.0);
    }
    // ── office item 3: marks ─────────────────────────────────────────────────────────────────────

    void marksMoveWithTheText()
    {
        m_fmt->insertText(QStringLiteral("Hello teh world"));
        const QString a = m_fmt->blockIds().value(0);
        const QString id = m_fmt->addMark(QStringLiteral("spelling"), a, 6, 9);
        QVERIFY(!id.isEmpty());
        QVariantMap m = markAtMap(m_fmt, 6);
        QCOMPARE(m.value("kind").toString(), QStringLiteral("spelling"));
        QCOMPARE(m.value("id").toString(), id);
        QCOMPARE(m.value("blockId").toString(), a);
        QCOMPARE(m.value("start").toInt(), 6);
        QCOMPARE(m.value("end").toInt(), 9);
        QVERIFY(!m_fmt->markAt(5).isValid());
        QVERIFY(!m_fmt->markAt(9).isValid());

        QTextCursor c(doc());
        c.setPosition(0);
        c.insertText(QStringLiteral("XX")); // before: shifts
        QCOMPARE(markAtMap(m_fmt, 8).value("start").toInt(), 8);
        QCOMPARE(markAtMap(m_fmt, 8).value("end").toInt(), 11);
        c.setPosition(11);
        c.insertText(QStringLiteral("s")); // right after it: not part of it
        QCOMPARE(markAtMap(m_fmt, 8).value("end").toInt(), 11);
        c.setPosition(doc()->characterCount() - 1);
        c.insertText(QStringLiteral("!")); // after: untouched
        QCOMPARE(markAtMap(m_fmt, 8).value("start").toInt(), 8);
        c.setPosition(8);
        c.insertText(QStringLiteral("Y")); // right before it: shifts
        QCOMPARE(markAtMap(m_fmt, 9).value("start").toInt(), 9);
        QCOMPARE(markAtMap(m_fmt, 9).value("end").toInt(), 12);
        QVERIFY(!m_fmt->markAt(8).isValid());

        // a split before it: the mark goes with its text into the new block
        m_fmt->setCursorPosition(2);
        m_fmt->insertParagraph();
        const QStringList ids = m_fmt->blockIds();
        m = markAtMap(m_fmt, 10);
        QCOMPARE(m.value("blockId").toString(), ids[1]);
        QCOMPARE(m.value("start").toInt(), 7);
        QCOMPARE(m.value("end").toInt(), 10);
        // a merge: back into the first block, offsets relative to it
        c.setPosition(doc()->findBlockByNumber(1).position());
        c.deletePreviousChar();
        m = markAtMap(m_fmt, 9);
        QCOMPARE(m.value("blockId").toString(), ids[0]);
        QCOMPARE(m.value("start").toInt(), 9);

        // typing INSIDE it: the result is stale, the mark goes
        c.setPosition(10);
        c.insertText(QStringLiteral("z"));
        QVERIFY(!m_fmt->markAt(9).isValid());
        QVERIFY(m_fmt->marks().isEmpty());

        // its whole range deleted, or part of it: gone too
        m_fmt->addMark(QStringLiteral("spelling"), ids[0], 0, 2);
        m_fmt->addMark(QStringLiteral("spelling"), ids[0], 3, 6);
        c.setPosition(0);
        c.setPosition(2, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(2);
        c.deleteChar(); // inside the second one
        QVERIFY(m_fmt->marks().isEmpty());
    }

    void formatOnlyEditsKeepMarks()
    {
        m_fmt->insertText(QStringLiteral("Hello teh world"));
        const QString a = m_fmt->blockIds().value(0);
        m_fmt->addMark(QStringLiteral("spelling"), a, 6, 9);
        m_fmt->setSelectionStart(4);
        m_fmt->setSelectionEnd(11);
        m_fmt->toggleBold();
        m_fmt->setSelectionEnd(4);
        m_fmt->applyParagraphStyle(QStringLiteral("Heading 1"));
        m_fmt->setZoom(2.0);
        m_fmt->setZoom(1.0);
        QCOMPARE(markAtMap(m_fmt, 6).value("end").toInt(), 9);
    }

    void clearMarksIsScoped()
    {
        m_fmt->insertText(QStringLiteral("aaa bbb\nccc ddd"));
        const QStringList ids = m_fmt->blockIds();
        const int p1 = doc()->findBlockByNumber(1).position();
        QCOMPARE(m_fmt->addMark(QStringLiteral("spelling"), ids[0], 0, 3, QStringLiteral("mine")), QStringLiteral("mine"));
        m_fmt->addMark(QStringLiteral("grammar"), ids[0], 4, 7);
        m_fmt->addMark(QStringLiteral("spelling"), ids[1], 0, 3);
        m_fmt->addMark(QStringLiteral("highlight"), ids[1], 4, 7);
        m_fmt->clearMarks(QStringLiteral("spelling"), ids[0]);
        QVERIFY(!m_fmt->markAt(0).isValid());
        QCOMPARE(markAtMap(m_fmt, 4).value("kind").toString(), QStringLiteral("grammar"));
        QCOMPARE(markAtMap(m_fmt, p1).value("kind").toString(), QStringLiteral("spelling"));
        m_fmt->clearMarks(QStringLiteral("highlight"));
        QVERIFY(!m_fmt->markAt(p1 + 4).isValid());
        QCOMPARE(markAtMap(m_fmt, p1).value("kind").toString(), QStringLiteral("spelling"));
        m_fmt->clearMarks(QStringLiteral("nothing"));
        QCOMPARE(m_fmt->marks().size(), 2);

        // invalid ranges / ids: nothing added
        QCOMPARE(m_fmt->addMark(QStringLiteral("spelling"), QStringLiteral("nope"), 0, 1), QString());
        QCOMPARE(m_fmt->addMark(QStringLiteral("spelling"), ids[0], -1, 1), QString());
        QCOMPARE(m_fmt->addMark(QStringLiteral("spelling"), ids[0], 2, 2), QString());
        QCOMPARE(m_fmt->addMark(QStringLiteral("spelling"), ids[0], 0, 8), QString()); // the separator is not text
        QCOMPARE(m_fmt->marks().size(), 2);
    }

    void marksAreNotDocumentContent()
    {
        m_fmt->insertText(QStringLiteral("I saw teh cat"));
        const QString plain = m_dir.filePath(QStringLiteral("nomarks.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl::fromLocalFile(plain)));
        QVERIFY(!m_fmt->isModified());
        const int undoSteps = doc()->availableUndoSteps();
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(13);
        m_fmt->copy();
        const QString htmlBefore = QGuiApplication::clipboard()->mimeData()->html();
        const QString docHtml = m_fmt->toHtml();

        const QString a = m_fmt->blockIds().value(0);
        m_fmt->addMark(QStringLiteral("spelling"), a, 6, 9);
        m_fmt->addMark(QStringLiteral("highlight"), a, 0, 1);
        QVERIFY(m_fmt->markStyle(QStringLiteral("spelling"), { { QStringLiteral("color"), QStringLiteral("#00aa00") } }).isEmpty());
        QCoreApplication::processEvents();
        QVERIFY(!m_fmt->isModified());
        QCOMPARE(doc()->availableUndoSteps(), undoSteps);
        QCOMPARE(m_fmt->toHtml(), docHtml);
        m_fmt->copy();
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->html(), htmlBefore);
        const QString marked = m_dir.filePath(QStringLiteral("marks.odt"));
        QVERIFY(m_fmt->saveOdf(QUrl::fromLocalFile(marked)));
        QCOMPARE(zipEntry(marked, QStringLiteral("content.xml")), zipEntry(plain, QStringLiteral("content.xml")));

        // a load / new document starts without marks
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(marked)));
        QVERIFY(m_fmt->marks().isEmpty());
    }

    void markSegmentsFollowTheTextAndTheStyle()
    {
        m_fmt->insertText(QStringLiteral("I saw teh cat"));
        const QString a = m_fmt->blockIds().value(0);
        m_fmt->addMark(QStringLiteral("spelling"), a, 6, 9);
        QList<RichTextHandler::MarkSegment> segs = m_fmt->markSegments();
        QCOMPARE(segs.size(), 1);
        QCOMPARE(segs[0].shape, RichTextHandler::MarkSegment::Wave);
        QCOMPARE(segs[0].color, QColor(0xd3, 0x2f, 0x2f));
        const QRectF r6 = caretRect(m_edit.data(), 6), r9 = caretRect(m_edit.data(), 9);
        QVERIFY(qAbs(segs[0].rect.left() - r6.left()) < 0.5);
        QVERIFY(qAbs(segs[0].rect.right() - r9.left()) < 0.5);
        QVERIFY(segs[0].rect.top() > r6.top() + r6.height() / 2); // under the baseline...
        QVERIFY(segs[0].rect.bottom() <= r6.bottom() + 0.5);     // ...inside the line

        // the style is the app's
        QVERIFY(m_fmt->markStyle(QStringLiteral("spelling"), { { QStringLiteral("underline"), QStringLiteral("single") },
                                                                { QStringLiteral("color"), QStringLiteral("#00ff00") } }).isEmpty());
        segs = m_fmt->markSegments();
        QCOMPARE(segs.size(), 1);
        QCOMPARE(segs[0].shape, RichTextHandler::MarkSegment::Underline);
        QCOMPARE(segs[0].color, QColor(0, 255, 0));
        QVERIFY(m_fmt->markStyle(QStringLiteral("spelling"), { { QStringLiteral("underline"), QStringLiteral("zigzag") } }).size() > 0);
        QVERIFY(m_fmt->markStyle(QStringLiteral("spelling"), { { QStringLiteral("bogus"), 1 } }).size() > 0);
        QVERIFY(m_fmt->markStyle(QStringLiteral("spelling"), { { QStringLiteral("underline"), QVariant::fromValue(nullptr) } }).isEmpty());
        QVERIFY(m_fmt->markSegments().isEmpty()); // no underline, no background: nothing to draw

        // highlight: a background over the whole line height
        m_fmt->addMark(QStringLiteral("highlight"), a, 0, 1);
        segs = m_fmt->markSegments();
        QCOMPARE(segs.size(), 1);
        QCOMPARE(segs[0].shape, RichTextHandler::MarkSegment::Background);
        QCOMPARE(segs[0].rect.top(), caretRect(m_edit.data(), 0).top());
        QCOMPARE(segs[0].rect.height(), caretRect(m_edit.data(), 0).height());

        // a mark over a wrapped paragraph: one segment per line
        m_fmt->newDocument();
        m_edit->setProperty("wrapMode", 3); // TextEdit.WrapAnywhere
        m_edit->setProperty("width", 60);
        m_fmt->insertText(QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
        m_fmt->addMark(QStringLiteral("grammar"), m_fmt->blockIds().value(0), 0, 26);
        segs = m_fmt->markSegments();
        QVERIFY2(segs.size() >= 2, QByteArray::number(segs.size()).constData());
        // a band below the text holds nothing
        QVERIFY(m_fmt->markSegments(QRectF(0, 5000, 60, 100)).isEmpty());
    }

    void waveIsDrawnUnderTheWord()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QQuickView view;
        view.setColor(Qt::white);
        QQmlComponent c(view.engine());
        c.setData("import QtQuick\nTextEdit { x: 10; y: 10; width: 280; textFormat: TextEdit.RichText; font.pixelSize: 24; color: \"black\" }", QUrl());
        QScopedPointer<QQuickItem> edit(qobject_cast<QQuickItem *>(c.create()));
        QVERIFY2(edit, qPrintable(c.errorString()));
        view.resize(300, 80);
        edit->setParentItem(view.contentItem());
        RichTextHandler fmt(edit.data());
        fmt.setDocument(edit->property("textDocument").value<QQuickTextDocument *>());
        fmt.newDocument();
        fmt.insertText(QStringLiteral("I saw teh cat"));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        const auto reddish = [](const QImage &img, const QRect &area) {
            int n = 0;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QColor px = img.pixelColor(x, y);
                    if (px.red() > px.green() + 60 && px.red() > px.blue() + 60)
                        ++n;
                }
            return n;
        };
        fmt.addMark(QStringLiteral("spelling"), fmt.blockIds().value(0), 6, 9);
        const QList<RichTextHandler::MarkSegment> segs = fmt.markSegments();
        QCOMPARE(segs.size(), 1);
        const qreal dpr = view.effectiveDevicePixelRatio();
        const QRect area = QRectF(segs[0].rect.translated(10, 10).topLeft() * dpr, segs[0].rect.size() * dpr).toAlignedRect();
        QTRY_VERIFY(reddish(view.grabWindow(), area) > 6);
        fmt.clearMarks(QStringLiteral("spelling"));
        QTRY_COMPARE(reddish(view.grabWindow(), area), 0);
    }
};

QTEST_MAIN(RichTextTest)
#include "richtext_test.moc"
