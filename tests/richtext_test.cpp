// RichTextHandler — the engine behind <RichText> and <DocumentEditor>: what an app that DRIVES the
// document relies on (office item 0). modified tracking (and its reset on load/save/new), the
// {ok, error} contract of the ODF IO, the debounce source (contentsChanged), the readable format
// state (alignment), and an .odt round-trip through Qt's writer + our content.xml reader.
#include "shims/richtext.h"

#include <QDir>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

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
        QVERIFY(m_fmt->bold()); // the heading's look (bold, 20pt) survives …
        QTextCursor at(doc());
        at.setPosition(2);
        QCOMPARE(at.charFormat().fontPointSize(), 20.0);
        // … but not its level: Qt's ODF writer emits <text:p> for headings (no <text:h>/outline
        // level). Named paragraph styles + our own writer are office item 2.
        QEXPECT_FAIL("", "Qt's QTextDocumentWriter drops heading levels (office item 2)", Continue);
        QCOMPARE(m_fmt->heading(), 1);
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

        // Qt's writer keeps the spacing as fo:margin-* on its automatic styles: a reload agrees.
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

    void saveErrorIsReported()
    {
        m_fmt->insertText(QStringLiteral("x"));
        QVERIFY(!m_fmt->saveOdf(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("no/such/dir/x.odt")))));
        QVERIFY(!m_fmt->errorString().isEmpty());
        QCOMPARE(m_fmt->isModified(), true);
    }
};

QTEST_MAIN(RichTextTest)
#include "richtext_test.moc"
