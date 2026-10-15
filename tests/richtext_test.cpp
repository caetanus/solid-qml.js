// RichTextHandler — the engine behind <RichText> and <DocumentEditor>: what an app that DRIVES the
// document relies on (office item 0). modified tracking (and its reset on load/save/new), the
// {ok, error} contract of the ODF IO, the debounce source (contentsChanged), the readable format
// state (alignment), and an .odt round-trip through our writer + reader. Office item 2: named
// paragraph/character styles (define, apply, re-apply, next style on Enter, direct formatting)
// and their styles.xml round-trip.
#include "shims/richtext.h"
#include "shims/richtext_layout.h"
#include "shims/richtext_odf.h"
#include "shims/richtext_page_bands.h"
#include "shims/richtext_qt_p.h"
#include "qmlcss/QMLCss.h"
#include <QQmlContext>

#include <QClipboard>
#include <QBuffer>
#include <QFontDatabase>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QMetaMethod>
#include <QPainter>
#include <QProcess>
#include <QXmlStreamReader>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBoundaryFinder>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTextDocumentWriter>
#include <QTextFragment>
#include <QTextList>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <private/qquicktextedit_p_p.h>
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
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\"" \
    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\"" \
    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"" \
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\""

// A minimal .odt whose content.xml body is `body`, with optional automatic styles (content.xml)
// and named styles (styles.xml office:styles).
void writeOdt(const QString &path, const QByteArray &body, const QByteArray &automaticStyles = {},
              const QByteArray &namedStyles = {}, const QByteArray &image = {})
{
    QZipWriter zip(path);
    zip.addFile(QStringLiteral("mimetype"), "application/vnd.oasis.opendocument.text");
    if (!image.isEmpty()) zip.addFile(QStringLiteral("Pictures/image.png"), image);
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

// Exercise the actual widget and QQuickTextEdit layout, including its width/viewport hooks.
struct PageEditor {
    QmlCss::CssTheme theme;
    QmlCss::CssLayoutEngine layout{&theme};
    QQuickView view;
    QScopedPointer<QQuickItem> root;
    explicit PageEditor(qreal initialZoom = 1) {
        view.engine()->rootContext()->setContextProperty("cssTheme", &theme);
        view.engine()->rootContext()->setContextProperty("cssLayout", &layout);
        QQmlComponent c(view.engine());
        c.setData("import QtQuick\nimport solidqml.Widgets.RichText\n"
                  "DocumentEditor { width: 900; height: 600; toolbar: false; pageWidth: 794; pageMargins: [94,113,94,113] }", QUrl());
        root.reset(qobject_cast<QQuickItem *>(c.createWithInitialProperties({{QStringLiteral("zoom"), initialZoom}})));
        if (!root) qWarning().noquote() << c.errorString();
        view.resize(900, 600);
        if (root) root->setParentItem(view.contentItem());
    }
    static QList<QQuickItem *> sheets(QQuickItem *parent) {
        QList<QQuickItem *> result;
        for (auto *item : parent->childItems()) {
            if (item->objectName() == QStringLiteral("documentSheet")) result.append(item);
            result.append(sheets(item));
        }
        return result;
    }
    RichTextHandler *fmt() { return root->findChild<RichTextHandler *>(); }
    QQuickItem *edit() { return root->property("_editor").value<QQuickItem *>(); }
    QQuickItem *body() { return root->property("_body").value<QQuickItem *>(); }
};

class RichTextTest : public QObject {
    Q_OBJECT

    QQmlEngine m_engine;
    QScopedPointer<QObject> m_edit;
    RichTextHandler *m_fmt = nullptr;
    QTemporaryDir m_dir;

    QTextDocument *doc() const { return m_fmt->document()->textDocument(); }

    // Independent oracle for wordAndCharCountRules-style checks: a FULL, fresh per-block recount
    // of the LIVE document (the same algorithm countWords() in richtext.cpp uses, duplicated here
    // on purpose — it must never share the incremental bookkeeping it is meant to catch mistakes
    // in). Also walks headings()/currentHeadingIndex() against the live document directly, so a
    // latent use-after-free (a stale QTextBlock handle read after its block died) cannot produce a
    // value that merely LOOKS plausible: every position/level/text headings() reports is checked
    // against doc()->findBlock(pos) right now.
    static int referenceWordCount(const QString &text)
    {
        if (text.isEmpty())
            return 0;
        QTextBoundaryFinder bf(QTextBoundaryFinder::Word, text);
        int count = 0, prev = 0;
        for (bf.toStart(); bf.toNextBoundary() != -1; prev = bf.position()) {
            const QStringView seg = QStringView(text).mid(prev, bf.position() - prev);
            if (!seg.isEmpty() && seg.at(0).isLetterOrNumber())
                ++count;
        }
        return count;
    }

    void verifyHeadingsAndCountsConsistent()
    {
        const QVariantList h = m_fmt->headings();
        int lastPos = -1;
        for (const QVariant &v : h) {
            const QVariantMap m = v.toMap();
            const int pos = m.value(QStringLiteral("position")).toInt();
            QVERIFY(pos > lastPos);
            lastPos = pos;
            const QTextBlock b = doc()->findBlock(pos);
            QVERIFY(b.isValid());
            QCOMPARE(b.position(), pos);
            QVERIFY(b.blockFormat().headingLevel() > 0);
            QCOMPARE(b.blockFormat().headingLevel(), m.value(QStringLiteral("level")).toInt());
            QCOMPARE(b.text(), m.value(QStringLiteral("text")).toString());
            // The stable id an outline keys its rows on (<For key="blockId">).
            QVERIFY(!m.value(QStringLiteral("blockId")).toString().isEmpty());
            QCOMPARE(m.value(QStringLiteral("blockId")).toString(), m_fmt->blockIdAt(pos));
        }
        int idx = -1;
        for (int i = 0; i < h.size(); ++i) {
            if (h[i].toMap().value(QStringLiteral("position")).toInt() > m_fmt->cursorPosition())
                break;
            idx = i;
        }
        QCOMPARE(m_fmt->currentHeadingIndex(), idx);

        int words = 0, chars = 0;
        for (QTextBlock b = doc()->begin(); b.isValid(); b = b.next()) {
            chars += b.text().size();
            words += referenceWordCount(b.text());
        }
        QCOMPARE(m_fmt->wordCount(), words);
        QCOMPARE(m_fmt->charCount(), chars);
    }

private slots:
    void editingColorsAtInsertionPoint()
    {
        m_fmt->insertText("abcd");
        m_fmt->setCursorPosition(2);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "setTextColor", Q_ARG(QString, QString("#ff0000"))));
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "setHighlightColor", Q_ARG(QString, QString("#ffff00"))));
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "toggleSuperscript"));
        QCOMPARE(m_fmt->property("textColor").toString(), QString("#ff0000"));
        QVERIFY(m_fmt->property("superscript").toBool());
        m_fmt->insertText("X");
        QCOMPARE(charAt(doc(), 2).foreground().color(), QColor("#ff0000"));
        QCOMPARE(charAt(doc(), 2).background().color(), QColor("#ffff00"));
        QCOMPARE(charAt(doc(), 2).verticalAlignment(), QTextCharFormat::AlignSuperScript);
        QVERIFY(charAt(doc(), 1).foreground().color() != QColor("#ff0000"));
    }

    void editingParagraphApi()
    {
        m_fmt->insertText("one\ntwo");
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(7);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "setParagraphIndents", Q_ARG(qreal, 24.), Q_ARG(qreal, 12.), Q_ARG(qreal, -8.)));
        for (auto b = doc()->begin(); b.isValid(); b = b.next()) {
            QCOMPARE(b.blockFormat().leftMargin(), 24.);
            QCOMPARE(b.blockFormat().rightMargin(), 12.);
            QCOMPARE(b.blockFormat().textIndent(), -8.);
        }
        doc()->undo();
        QCOMPARE(doc()->begin().blockFormat().leftMargin(), 0.);
        QCOMPARE(doc()->lastBlock().blockFormat().leftMargin(), 0.);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "setLineSpacing", Q_ARG(qreal, 150.)));
        QCOMPARE(doc()->begin().blockFormat().lineHeight(), 150.);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "setParagraphSpacing", Q_ARG(qreal, 9.), Q_ARG(qreal, 13.)));
        QCOMPARE(doc()->lastBlock().blockFormat().bottomMargin(), 13.);
        const QVariantList stops{24., 72.};
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "setTabStops", Q_ARG(QVariantList, stops)));
        QCOMPARE(doc()->begin().blockFormat().tabPositions().size(), 2);
    }

    void editingFindReplace()
    {
        m_fmt->insertText("Cat cat scatter cat");
        m_fmt->setCursorPosition(0);
        const QVariantMap options{{"wholeWords", true}, {"wrap", true}};
        bool found = false;
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "find", Q_RETURN_ARG(bool, found), Q_ARG(QString, QString("cat")), Q_ARG(QVariantMap, options)));
        QVERIFY(found);
        QCOMPARE(m_edit->property("selectedText").toString(), QString("Cat"));
        int count = 0;
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "replaceAll", Q_RETURN_ARG(int, count), Q_ARG(QString, QString("cat")), Q_ARG(QString, QString("dog")), Q_ARG(QVariantMap, options)));
        QCOMPARE(count, 3);
        QCOMPARE(doc()->toPlainText(), QString("dog dog scatter dog"));
        doc()->undo();
        QCOMPARE(doc()->toPlainText(), QString("Cat cat scatter cat"));
    }

    void editingOdfRoundTrip()
    {
        m_fmt->insertText("sample");
        m_fmt->setSelectionStart(0); m_fmt->setSelectionEnd(6);
        m_fmt->setTextColor("#aa0000"); m_fmt->setHighlightColor("#ffff00"); m_fmt->toggleSubscript();
        m_fmt->setParagraphIndents(24, 12, -6); m_fmt->setParagraphSpacing(9, 13);
        m_fmt->setLineSpacing(150); m_fmt->setTabStops({24.,72.});
        const auto path = QUrl::fromLocalFile(m_dir.filePath("editing.odt"));
        QVERIFY(m_fmt->saveOdfCopy(path)); QVERIFY(m_fmt->loadOdf(path));
        QCOMPARE(charAt(doc(), 0).foreground().color(), QColor("#aa0000"));
        QCOMPARE(charAt(doc(), 0).background().color(), QColor("#ffff00"));
        QCOMPARE(charAt(doc(), 0).verticalAlignment(), QTextCharFormat::AlignSubScript);
        const auto f = doc()->begin().blockFormat();
        QCOMPARE(f.leftMargin(), 24.); QCOMPARE(f.rightMargin(), 12.); QCOMPARE(f.textIndent(), -6.);
        QVERIFY(qAbs(f.topMargin() - 9.) < 0.00001); QVERIFY(qAbs(f.bottomMargin() - 13.) < 0.00001); QCOMPARE(f.lineHeight(), 150.);
        QCOMPARE(f.tabPositions().size(), 2);
        QCOMPARE(f.tabPositions()[1].position, 72.);
    }

    void editingFormatPainter()
    {
        m_fmt->insertText("one\ntwo");
        m_fmt->setCursorPosition(1);
        m_fmt->setSelectionStart(0); m_fmt->setSelectionEnd(3);
        m_fmt->setTextColor("#aa0000"); m_fmt->setParagraphIndents(24, 12, -6);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "copyFormat"));
        QVERIFY(m_fmt->property("hasCopiedFormat").toBool());
        m_fmt->setCursorPosition(5); m_fmt->setSelectionStart(4); m_fmt->setSelectionEnd(7);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "applyCopiedFormat"));
        QCOMPARE(charAt(doc(), 4).foreground().color(), QColor("#aa0000"));
        QCOMPARE(doc()->lastBlock().blockFormat().leftMargin(), 24.);
        doc()->undo();
        QVERIFY(charAt(doc(), 4).foreground().color() != QColor("#aa0000"));
        QCOMPARE(doc()->lastBlock().blockFormat().leftMargin(), 0.);
    }

    void formatPainterClearsTargetFormatting()
    {
        m_fmt->insertText("plain\ntarget");
        m_fmt->setSelectionStart(6); m_fmt->setSelectionEnd(12); m_fmt->setCursorPosition(7);
        m_fmt->toggleBold(); m_fmt->setHighlightColor("#ffff00");
        m_fmt->setParagraphIndents(24, 12, 6);
        m_fmt->setSelectionStart(0); m_fmt->setSelectionEnd(0); m_fmt->setCursorPosition(1);
        m_fmt->copyFormat();
        m_fmt->setSelectionStart(6); m_fmt->setSelectionEnd(12); m_fmt->setCursorPosition(7);
        m_fmt->applyCopiedFormat();
        QVERIFY(charAt(doc(), 6).fontWeight() != QFont::Bold);
        QCOMPARE(charAt(doc(), 6).background().style(), Qt::NoBrush);
        QCOMPARE(doc()->lastBlock().blockFormat().leftMargin(), 0.);
    }

    void editingNotifyOnlyOnChange()
    {
        m_fmt->insertText("word");
        m_fmt->setCursorPosition(2);
        QSignalSpy color(m_fmt, &RichTextHandler::textColorChanged);
        QSignalSpy highlight(m_fmt, &RichTextHandler::highlightColorChanged);
        QSignalSpy left(m_fmt, &RichTextHandler::leftIndentChanged);
        m_fmt->setTextColor("#123456");
        QCOMPARE(color.size(), 1);
        m_fmt->setTextColor("#123456");
        QCOMPARE(color.size(), 1);
        m_fmt->setHighlightColor("#ffff00");
        QCOMPARE(highlight.size(), 1);
        m_fmt->setHighlightColor("");
        QCOMPARE(highlight.size(), 2);
        m_fmt->setHighlightColor("");
        QCOMPARE(highlight.size(), 2);
        m_fmt->setParagraphIndents(24, 0, 0);
        QCOMPARE(left.size(), 1);
        m_fmt->setParagraphIndents(24, 0, 0);
        QCOMPARE(left.size(), 1);
        m_fmt->setZoom(2);
        QCOMPARE(left.size(), 1);
        QCOMPARE(m_fmt->leftIndent(), 24.);
    }

    void editingSearchOptions()
    {
        m_fmt->insertText("Cat cat scatter cat42");
        m_fmt->setCursorPosition(0);
        QVERIFY(m_fmt->find("cat", {{"caseSensitive", true}, {"wholeWords", true}}));
        QCOMPARE(m_fmt->selectionStart(), 4);
        QVERIFY(!m_fmt->replace("dog"));
        QVERIFY(doc()->toPlainText().startsWith("Cat dog"));
        QVERIFY(!m_fmt->find("[", {{"regex", true}}));
        QCOMPARE(m_fmt->replaceAll("cat[0-9]+", "number", {{"regex", true}}), 1);
        QVERIFY(doc()->toPlainText().endsWith("number"));
        m_fmt->setSelectionStart(0); m_fmt->setSelectionEnd(0); m_fmt->setCursorPosition(0);
        QVERIFY(!m_fmt->find("dog", {{"backward", true}, {"wrap", false}}));
        QVERIFY(m_fmt->find("dog", {{"backward", true}, {"wrap", true}}));
        QCOMPARE(m_fmt->selectionStart(), 4);
    }

    void editingBookSearchTiming_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::addColumn<bool>("wholeBook");
        QTest::newRow("bare-book") << QStringLiteral("bare") << true;
        for (const auto &engine : {QStringLiteral("custom"), QStringLiteral("qt")}) {
            QTest::newRow(qPrintable(engine + "-book")) << engine << true;
            QTest::newRow(qPrintable(engine + "-1000")) << engine << false;
        }
    }

    void editingBookSearchTiming()
    {
        QFETCH(QString, engine);
        QFETCH(bool, wholeBook);
        const auto path = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(path)) QSKIP("book sample unavailable");
        PageEditor w;
        QVERIFY(w.root);
        if (engine != "bare") w.root->setProperty("layoutEngine", engine);
        auto *fmt = engine == "bare" ? m_fmt : w.fmt();
        // A standalone TextEdit otherwise sizes itself from idealWidth(), feeding
        // each implicit-width change back into Qt layout during undo. Real editors
        // have a width imposed by their viewport; give this handler fixture one too.
        if (engine == "bare") m_edit->setProperty("width", 794);
        QVERIFY(fmt->loadOdf(QUrl::fromLocalFile(path)));
        auto *d = fmt->document()->textDocument();
        if (!wholeBook) {
            // Keep book-sized layout and bookkeeping, but only 1,000 matches.
            QTextCursor group(d);
            group.beginEditBlock();
            int seen = 0, from = 0;
            while (true) {
                auto match = d->find(QStringLiteral("a"), from, QTextDocument::FindCaseSensitively);
                if (match.isNull()) break;
                if (++seen > 1000) match.insertText(QStringLiteral("A"));
                from = match.selectionEnd();
            }
            group.endEditBlock();
        }
        if (engine != "bare") {
            w.view.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w.view));
            QCoreApplication::processEvents();
        }
        QCOMPARE(dynamic_cast<rt::RichTextLayout *>(d->documentLayout()) != nullptr, engine == "custom");
        const QString before = d->toPlainText();
        const auto ids = fmt->blockIds();
        const auto headings = fmt->headings();
        const int words = fmt->wordCount(), chars = fmt->charCount();
        const int markPos = before.indexOf(u'a');
        QVERIFY(markPos >= 0);
        auto addMark = [&] {
            const auto block = d->findBlock(markPos);
            QVERIFY(!fmt->addMark("spelling", fmt->blockIdAt(markPos),
                                 markPos - block.position(), markPos - block.position() + 1).isEmpty());
        };
        addMark();
        QSignalSpy changes(d, &QTextDocument::contentsChange);
        QElapsedTimer timer; timer.start();
        QVERIFY(!fmt->find("task13-no-hit-8bc31c64", {{"wrap", false}}));
        const double searchMs = timer.nsecsElapsed()/1e6;
        timer.restart();
        const int count = fmt->replaceAll("a", "A", {{"caseSensitive", true}});
        d->documentLayout()->documentSize();
        const auto replaceMs = timer.nsecsElapsed()/1e6;
        const QString after = d->toPlainText();
        QCOMPARE(count, before.count(u'a'));
        if (!wholeBook) QCOMPARE(count, 1000);
        QCOMPARE(after, QString(before).replace(u'a', u'A'));
        QVERIFY(!fmt->markAt(markPos).isValid());
        addMark();
        const int forwardSignals = changes.size();
        changes.clear();
        timer.restart();
        d->undo();
        d->documentLayout()->documentSize();
        const auto undoMs = timer.nsecsElapsed()/1e6;
        QCOMPARE(d->toPlainText(), before);
        QVERIFY(!fmt->markAt(markPos).isValid());
        QCOMPARE(fmt->blockIds(), ids);
        QCOMPARE(fmt->headings(), headings);
        QCOMPARE(fmt->wordCount(), words);
        QCOMPARE(fmt->charCount(), chars);
        const int undoSignals = changes.size();
        auto verifyCold = [&] {
            QScopedPointer<QTextDocument> cold(d->clone());
            if (engine == "custom") cold->setDocumentLayout(new rt::RichTextLayout(cold.data()));
            QCOMPARE(d->pageCount(), cold->pageCount());
            auto expected = cold->begin();
            for (auto b = d->begin(); b.isValid(); b = b.next(), expected = expected.next()) {
                QCOMPARE(d->documentLayout()->blockBoundingRect(b), cold->documentLayout()->blockBoundingRect(expected));
                QCOMPARE(b.layout()->lineCount(), expected.layout()->lineCount());
                for (int i = 0; i < b.layout()->lineCount(); ++i) {
                    QCOMPARE(b.layout()->lineAt(i).position(), expected.layout()->lineAt(i).position());
                    QCOMPARE(b.layout()->lineAt(i).naturalTextWidth(), expected.layout()->lineAt(i).naturalTextWidth());
                }
            }
        };
        verifyCold();
        addMark();
        timer.restart();
        d->redo();
        d->documentLayout()->documentSize();
        const auto redoMs = timer.nsecsElapsed()/1e6;
        QCOMPARE(d->toPlainText(), after);
        QVERIFY(!fmt->markAt(markPos).isValid());
        QCOMPARE(fmt->blockIds(), ids);
        QCOMPARE(fmt->wordCount(), words);
        QCOMPARE(fmt->charCount(), chars);
        verifyCold();
        for (const auto &entry : fmt->headings()) {
            const auto heading = entry.toMap();
            const auto block = d->findBlock(heading.value("position").toInt());
            QCOMPARE(heading.value("text").toString(), block.text());
            QCOMPARE(heading.value("level").toInt(), block.blockFormat().headingLevel());
        }
        QCOMPARE(forwardSignals, 1);
        QCOMPARE(undoSignals, 1);
        QCOMPARE(changes.size(), 2); // one undo and one redo, both already coalesced by Qt
        QFile loadFile("/proc/loadavg");
        QVERIFY(loadFile.open(QIODevice::ReadOnly));
        const double load = loadFile.readAll().split(' ').first().toDouble();
        qInfo("REPLAY engine=%s count=%d search_ms=%.3f replace_ms=%.3f undo_ms=%.3f redo_ms=%.3f signals=%d/%d load=%.2f",
              qPrintable(engine), count, searchMs, replaceMs, undoMs, redoMs, forwardSignals, undoSignals, load);
        if (load > 6) QSKIP("correctness passed; timing budget skipped at load > 6");
        QVERIFY2(undoMs <= 3 * replaceMs, "undo must cost at most 3x replaceAll");
        QVERIFY2(redoMs <= 3 * replaceMs, "redo must cost at most 3x replaceAll");
    }

    void viewportGeometry_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::newRow("custom") << QString("custom");
        QTest::newRow("qt") << QString("qt");
    }

    void viewportGeometry()
    {
        QFETCH(QString, engine);
        PageEditor widget;
        QVERIFY(widget.root);
        QVERIFY(widget.root->property("pageViewportX").isValid());
        QVERIFY(!widget.root->property("ruler").isValid());
        QVERIFY(!widget.root->property("rulerUnit").isValid());
        QVERIFY(!widget.root->property("effectiveRulerUnit").isValid());
        QVERIFY(!widget.root->findChild<QQuickItem *>("documentRuler"));
        widget.root->setProperty("layoutEngine", engine);
        widget.view.show();
        widget.fmt()->insertText("first");
        widget.fmt()->insertPageBreak();
        widget.fmt()->insertText("second");
        widget.fmt()->insertPageBreak();
        widget.fmt()->insertText("third");
        widget.edit()->setProperty("cursorPosition", 0);
        QTest::qWait(50);
        widget.body()->setProperty("contentY", 0.);
        QTest::qWait(30);
        auto value = [&](const char *name) { return widget.root->property(name).toReal(); };
        QCOMPARE(value("pageViewportX"), 53.);
        QCOMPARE(value("pageViewportY"), 0.);
        QSignalSpy changed(widget.root.data(), SIGNAL(viewportGeometryChanged()));
        QVERIFY(changed.isValid());
        widget.body()->setProperty("contentY", 1200.);
        QTest::qWait(30);
        QCOMPARE(value("pageViewportY"), -53.); // caret off-screen: second sheet
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.body()->setProperty("contentY", 1200.);
        QTest::qWait(30);
        QCOMPARE(changed.count(), 0);
        widget.body()->setProperty("contentY", 0.);
        QTest::qWait(30);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.root->setProperty("zoom", 1.5);
        QTest::qWait(50);
        QCOMPARE(value("pageViewportX"), 0.);
        QCOMPARE(value("pageViewportY"), 0.);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.body()->setProperty("contentX", 60.);
        QTest::qWait(30);
        QCOMPARE(value("pageViewportX"), -60.);
        QCOMPARE(changed.count(), 1);
        widget.body()->setProperty("contentX", 0.);
        QTest::qWait(30);
        changed.clear();
        widget.root->setWidth(1300);
        widget.view.resize(1300, 600);
        QTest::qWait(30);
        QCOMPARE(value("pageViewportX"), 54.5);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.root->setHeight(650);
        widget.view.resize(1300, 650);
        QTest::qWait(30);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.root->setProperty("pageWidth", 700.);
        QTest::qWait(30);
        QCOMPARE(value("pageViewportX"), 125.);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.root->setProperty("pageHeight", 1000.);
        QTest::qWait(30);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.root->setProperty("pageMargins", QVariantList{90, 100, 90, 100});
        QTest::qWait(30);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.fmt()->insertText("abc");
        QTest::qWait(30);
        QCOMPARE(changed.count(), 0);
        widget.root->setProperty("toolbar", true);
        QTest::qWait(30);
        QCOMPARE(value("pageViewportY"), 38.);
        QCOMPARE(changed.count(), 1);
    }

    void viewportGeometryPageChoice()
    {
        PageEditor widget;
        QVERIFY(widget.root);
        widget.root->setProperty("pageHeight", 300.);
        widget.view.show();
        QTest::qWait(30);
        widget.fmt()->insertText("first");
        widget.fmt()->insertPageBreak();
        widget.fmt()->insertText("second");
        widget.fmt()->insertPageBreak();
        widget.fmt()->insertText("third");
        const int second = widget.fmt()->plainText().indexOf("second");
        QVERIFY(second > 0);
        widget.edit()->setProperty("cursorPosition", second);
        QTest::qWait(30);
        widget.body()->setProperty("contentY", 0.);
        QTest::qWait(30);
        QCOMPARE(widget.root->property("currentPage").toInt(), 2);
        QCOMPARE(widget.root->property("pageViewportY").toReal(), 324.);
        QSignalSpy changed(widget.root.data(), SIGNAL(viewportGeometryChanged()));
        widget.edit()->setProperty("cursorPosition", second + 1);
        QTest::qWait(30);
        QCOMPARE(changed.count(), 0);
        widget.edit()->setProperty("cursorPosition", 0);
        QTest::qWait(30);
        QCOMPARE(widget.root->property("pageViewportY").toReal(), 0.);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        // A viewport top in the gap belongs to the preceding sheet, even with
        // the caret off-screen. The caret-only currentPage remains unchanged.
        widget.body()->setProperty("contentY", 310.);
        QTest::qWait(30);
        QCOMPARE(widget.root->property("pageViewportY").toReal(), -310.);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.root->setProperty("pageGap", 40.);
        QTest::qWait(30);
        QCOMPARE(changed.count(), 1);
        changed.clear();
        widget.body()->setProperty("contentY", 350.);
        QTest::qWait(30);
        QCOMPARE(widget.root->property("pageViewportY").toReal(), -10.);
        QCOMPARE(widget.root->property("currentPage").toInt(), 1);
        QCOMPARE(changed.count(), 1);
    }

    void tableColumnResizeUndoAndOdf()
    {
        m_fmt->insertTable(1, 2);
        auto *table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().first());
        auto format = table->format();
        format.setColumnWidthConstraints({QTextLength(QTextLength::FixedLength, 100), QTextLength(QTextLength::FixedLength, 100)});
        table->setFormat(format);
        bool started = false;
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "beginColumnResize", Q_RETURN_ARG(bool, started), Q_ARG(int, table->firstPosition()), Q_ARG(int, 1)));
        QVERIFY(started);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "resizeColumn", Q_ARG(qreal, 20.)));
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "resizeColumn", Q_ARG(qreal, 30.)));
        QCOMPARE(table->format().columnWidthConstraints()[0].rawValue(), 100.);
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "endColumnResize"));
        QCOMPARE(table->format().columnWidthConstraints()[0].rawValue(), 130.);
        QCOMPARE(table->format().columnWidthConstraints()[1].rawValue(), 70.);
        doc()->undo();
        QCOMPARE(table->format().columnWidthConstraints()[0].rawValue(), 100.);
        doc()->redo();
        const auto path = QUrl::fromLocalFile(m_dir.filePath("columns.odt"));
        QVERIFY(m_fmt->saveOdfCopy(path)); QVERIFY(m_fmt->loadOdf(path));
        table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().first());
        QVERIFY(qAbs(table->format().columnWidthConstraints()[0].rawValue() - 130.) < 0.00001);
        QVERIFY(qAbs(table->format().columnWidthConstraints()[1].rawValue() - 70.) < 0.00001);
    }

    void tableColumnPointer_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::newRow("custom") << QString("custom");
        QTest::newRow("qt") << QString("qt");
    }
    void tableColumnPointer()
    {
        QFETCH(QString, engine);
        PageEditor widget;
        QVERIFY(widget.root);
        widget.root->setProperty("layoutEngine", engine);
        widget.fmt()->insertTable(1, 2);
        widget.view.show(); QTest::qWait(30);
        auto *document = widget.fmt()->document()->textDocument();
        auto *table = qobject_cast<QTextTable *>(document->rootFrame()->childFrames().first());
        const auto bounds = document->documentLayout()->frameBoundingRect(table);
        const qreal y = document->documentLayout()->blockBoundingRect(table->cellAt(0,0).firstCursorPosition().block()).center().y();
        QVariantMap border;
        for (qreal x=bounds.left()+8; x<bounds.right()-8; ++x) {
            border = widget.fmt()->columnBorderAt(x, y);
            if (!border.isEmpty()) break;
        }
        QVERIFY(!border.isEmpty());
        const qreal borderX = border.value("x").toDouble();
        QVERIFY(!widget.fmt()->columnBorderAt(borderX + 2.9, y).isEmpty());
        QVERIFY(widget.fmt()->columnBorderAt(borderX + 3.1, y).isEmpty());
        const auto start = widget.edit()->mapToScene({borderX, y}).toPoint();
        QTest::mouseMove(&widget.view, start);
        const auto before = table->format().columnWidthConstraints();
        QTest::mousePress(&widget.view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&widget.view, start+QPoint(20,0));
        QCOMPARE(table->format().columnWidthConstraints(), before);
        auto *preview = widget.root->findChild<QQuickItem *>("tableColumnPreview");
        QVERIFY(preview && preview->isVisible());
        auto *handle = widget.root->findChild<QQuickItem *>("tableColumnHandle");
        QCOMPARE(handle->property("cursorShape").toInt(), int(Qt::SplitHCursor));
        QTest::mouseRelease(&widget.view, Qt::LeftButton, Qt::NoModifier, start+QPoint(20,0));
        const auto after = table->format().columnWidthConstraints();
        QVERIFY(after != before);
        QCOMPARE(after.size(), 2);
        document->undo();
        QCOMPARE(table->format().columnWidthConstraints(), before);
    }

    void tableTabNavigation()
    {
        m_fmt->insertTable(2, 2);
        auto *table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().first());
        QVERIFY(table);
        auto first = table->cellAt(0, 0).firstCursorPosition();
        first.insertText("one");
        auto second = table->cellAt(0, 1).firstCursorPosition();
        second.insertText("two");
        m_fmt->setCursorPosition(first.position());
        bool handled = false;
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "tableTab", Q_RETURN_ARG(bool, handled), Q_ARG(bool, false)));
        QVERIFY(handled);
        QCOMPARE(m_edit->property("selectedText").toString(), QString("two"));
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "tableTab", Q_RETURN_ARG(bool, handled), Q_ARG(bool, true)));
        QCOMPARE(m_edit->property("selectedText").toString(), QString("one"));
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(0);
        m_fmt->setCursorPosition(table->cellAt(1, 1).firstPosition());
        QVERIFY(QMetaObject::invokeMethod(m_fmt, "tableTab", Q_RETURN_ARG(bool, handled), Q_ARG(bool, false)));
        QCOMPARE(table->rows(), 3);
        doc()->undo();
        QCOMPARE(table->rows(), 2);
    }

    void odfLinksKeepStyleAndTargetThroughNestedSpans()
    {
        const QString path = m_dir.filePath("links.odt");
        writeOdt(path, "<text:p>Before <text:a xlink:href=\"https://example.test/\"><text:span text:style-name=\"Emphasis\">Link</text:span></text:a> after</text:p>", {},
            "<style:style style:name=\"Internet_20_link\" style:family=\"text\"><style:text-properties fo:color=\"#000080\" style:text-underline-style=\"solid\"/></style:style>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        const auto check = [&] {
            const auto link = charAt(doc(), 7);
            QCOMPARE(link.anchorHref(), QString("https://example.test/"));
            QVERIFY(link.isAnchor()); QVERIFY(link.fontUnderline()); QVERIFY(link.fontItalic());
            QCOMPARE(link.foreground().color(), QColor("#000080"));
            QVERIFY(!charAt(doc(), 12).isAnchor());
            QVERIFY(!charAt(doc(), 12).fontUnderline());
        };
        check();
        const QString copy = m_dir.filePath("links-copy.odt");
        QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(copy)));
        check();
    }

    void odfParagraphSpacingSurvivesTableBoundaries()
    {
        const QString path = m_dir.filePath("table-spacing.odt");
        writeOdt(path, "<text:p text:style-name=\"Before\">Before</text:p><table:table><table:table-column/><table:table-row><table:table-cell><text:p>Cell</text:p></table:table-cell></table:table-row></table:table><text:p text:style-name=\"After\">After</text:p>", {},
            "<style:default-style style:family=\"paragraph\"><style:paragraph-properties fo:line-height=\"20px\"/></style:default-style>"
            "<style:style style:name=\"Before\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-bottom=\"6px\"/></style:style>"
            "<style:style style:name=\"After\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"6px\"/></style:style>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_edit->setProperty("width", 300);
        m_fmt->configurePages(300, 300, 0, {10,10,10,10});
        auto table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().first());
        QVERIFY(table);
        const qreal cellY = table->cellAt(0,0).firstCursorPosition().block().layout()->position().y();
        QCOMPARE(cellY - doc()->begin().layout()->position().y(), 26.0);
        QCOMPARE(doc()->lastBlock().layout()->position().y() - cellY, 26.0);
    }

    void odfNormalLinesUseFractionalFontSpacing()
    {
        if (!QFontDatabase::families().contains("Liberation Serif")) QSKIP("Liberation Serif fixture font is unavailable");
        const QString path = m_dir.filePath("fractional-lines.odt");
        writeOdt(path, "<text:p>First line<text:line-break/>Second line<text:line-break/>Third line</text:p>", {},
            "<style:default-style style:family=\"paragraph\"><style:text-properties fo:font-family=\"Liberation Serif\" fo:font-size=\"12pt\"/></style:default-style>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_edit->setProperty("width", 300);
        m_fmt->configurePages(300, 300, 0, {10,10,10,10});
        const auto layout = doc()->begin().layout();
        QCOMPARE(layout->lineCount(), 3);
        // LibreOffice's 12pt Liberation Serif baseline interval at 96 dpi.
        QVERIFY(qAbs(layout->lineAt(1).y() - layout->lineAt(0).y() - 18.3984375) <= 1.0 / 64);
        QVERIFY(qAbs(layout->lineAt(2).y() - layout->lineAt(0).y() - 36.796875) <= 1.0 / 64);
    }

    void odfWidowsAndOrphansProtectPageSplits()
    {
        const QString path = m_dir.filePath("widows.odt");
        for (int lines : {2, 5}) {
            const QByteArray body = QByteArray("<text:p text:style-name=\"Filler\">First</text:p><text:p text:style-name=\"Protected\">Body")
                + QByteArray("<text:line-break/>Body").repeated(lines - 1) + "</text:p>";
            writeOdt(path, body, {}, QByteArray("<style:style style:name=\"Filler\" style:family=\"paragraph\"><style:paragraph-properties fo:line-height=\"")
                + (lines == 2 ? "50px" : "19px") + "\"/></style:style>"
                "<style:style style:name=\"Protected\" style:family=\"paragraph\"><style:paragraph-properties fo:line-height=\"20px\" fo:widows=\"2\" fo:orphans=\"2\"/></style:style>");
            QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
            m_edit->setProperty("width", 300);
            const int height = lines == 2 ? 100 : 120;
            m_fmt->configurePages(300, height, 0, {10,10,10,10});
            const auto block = doc()->findBlockByNumber(1);
            QCOMPARE(block.layout()->lineCount(), lines);
            for (int i = 0; i < lines; ++i) {
                const qreal y = block.layout()->position().y() + block.layout()->lineAt(i).y();
                QCOMPARE(int(y / height), lines == 2 || i >= 3 ? 1 : 0);
            }
            QCOMPARE(m_fmt->styleProperties("Protected").value("widows").toInt(), 2);
            const QString copy = m_dir.filePath("widows-copy.odt");
            QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
            QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(copy)));
            QCOMPARE(m_fmt->styleProperties("Protected").value("orphans").toInt(), 2);
        }
    }

    void odfBorderlessTablesUseEqualDefaultColumns()
    {
        const QString path = m_dir.filePath("borderless-table.odt");
        writeOdt(path, "<table:table><table:table-column/><table:table-column/><table:table-row><table:table-cell table:style-name=\"Plain\"><text:p>X</text:p></table:table-cell><table:table-cell table:style-name=\"Plain\"><text:p>A longer cell</text:p></table:table-cell></table:table-row></table:table>",
            "<style:style style:name=\"Plain\" style:family=\"table-cell\"><style:table-cell-properties fo:border=\"none\" fo:padding=\"0px\"/></style:style>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_edit->setProperty("width", 300);
        m_fmt->configurePages(300, 300, 0, {10,10,10,10});
        const auto table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().first());
        QVERIFY(table);
        QCOMPARE(table->format().border(), 0.0);
        QCOMPARE(table->format().cellPadding(), 0.0);
        const auto first = table->cellAt(0,0).firstCursorPosition().block();
        const auto second = table->cellAt(0,1).firstCursorPosition().block();
        QCOMPARE(second.layout()->position().x() - first.layout()->position().x(), 140.0);
    }

    void odfKeepWithNextMovesOnlyFittingGroups()
    {
        const QString path = m_dir.filePath("keep-next.odt");
        writeOdt(path, "<text:p text:style-name=\"Tall\">First</text:p><text:p text:style-name=\"Keep\">Heading</text:p><text:p text:style-name=\"Tall\">Body</text:p>", {},
            "<style:style style:name=\"Tall\" style:family=\"paragraph\"><style:paragraph-properties fo:line-height=\"40px\"/></style:style>"
            "<style:style style:name=\"Keep\" style:family=\"paragraph\"><style:paragraph-properties fo:line-height=\"20px\" fo:keep-with-next=\"always\"/></style:style>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_edit->setProperty("width", 300);
        m_fmt->configurePages(300, 100, 0, {10,10,10,10});
        auto heading = [&] { return doc()->findBlockByNumber(1).layout()->position().y(); };
        QCOMPARE(heading(), 110.0);
        QTextCursor edit(doc());
        edit.setPosition(doc()->lastBlock().position());
        edit.insertText(QStringLiteral("More body text ").repeated(15));
        QScopedPointer<QTextDocument> cold(doc()->clone());
        cold->setDocumentLayout(new rt::RichTextLayout(cold.data()));
        cold->documentLayout()->documentSize();
        for (auto b = doc()->begin(), expected = cold->begin(); b.isValid(); b = b.next(), expected = expected.next()) {
            QCOMPARE(b.layout()->lineCount(), expected.layout()->lineCount());
            for (int i = 0; i < b.layout()->lineCount(); ++i)
                QCOMPARE(b.layout()->position() + b.layout()->lineAt(i).position(), expected.layout()->position() + expected.layout()->lineAt(i).position());
        }
        doc()->undo();
        // Resizing restores normal flow; an oversized group must never loop or skip pages.
        m_fmt->configurePages(300, 160, 0, {10,10,10,10});
        QVERIFY(qAbs(heading() - 50.0) < 1);
        m_fmt->configurePages(300, 70, 0, {10,10,10,10});
        QVERIFY(heading() < 150);
    }

    void odfPageBandsRenderAndRoundTrip()
    {
        const QString path = m_dir.filePath("page-bands.odt");
        QZipWriter zip(path);
        zip.addFile("content.xml", "<office:document-content" ODF_NS "><office:body><office:text><text:p>Body</text:p></office:text></office:body></office:document-content>");
        zip.addFile("styles.xml", "<office:document-styles" ODF_NS "><office:styles><style:style style:name=\"Band\" style:family=\"paragraph\"><style:text-properties fo:font-size=\"12pt\" fo:color=\"#cc0000\"/></style:style></office:styles>"
            "<office:automatic-styles><style:style style:name=\"Centered\" style:family=\"paragraph\" style:parent-style-name=\"Band\"><style:paragraph-properties fo:text-align=\"center\"/></style:style>"
            "<style:page-layout style:name=\"Page\"><style:page-layout-properties fo:page-width=\"300px\" fo:page-height=\"200px\" fo:margin=\"10px\"/><style:header-style><style:header-footer-properties fo:min-height=\"30px\" fo:margin-bottom=\"10px\"/></style:header-style><style:footer-style><style:header-footer-properties fo:min-height=\"40px\" fo:margin-top=\"10px\"/></style:footer-style></style:page-layout>"
            "</office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"Page\"><style:header><text:p text:style-name=\"Centered\">Heading</text:p></style:header>"
            "<style:footer><text:p text:style-name=\"Centered\">Page <text:page-number>1</text:page-number> of <text:page-count>1</text:page-count></text:p></style:footer></style:master-page></office:master-styles></office:document-styles>");
        zip.close();
        m_fmt->setLayoutEngine("custom");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_edit->setProperty("width", 300);
        m_fmt->configurePages(300, 200, 0, {10,10,10,10});
        const QString copy = m_dir.filePath("page-bands-copy.odt");
        QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
        QVERIFY(QZipReader(copy).fileData("styles.xml").contains("text:page-number"));
        const auto paint = [&] {
            QImage output(300,200,QImage::Format_RGB32); output.fill(Qt::white);
            QPainter painter(&output); doc()->documentLayout()->draw(&painter, {}); painter.end();
            return output;
        };
        const QImage before = paint();
        int footerInk = 0, headerInk = 0;
        for (int y = 0; y < 200; ++y) for (int x = 80; x < 220; ++x) {
            const auto c = before.pixelColor(x,y);
            if (c.red() > c.green() + 80) { if (y < 35) ++headerInk; if (y > 155) ++footerInk; }
        }
        QVERIFY(headerInk > 20); QVERIFY(footerInk > 20);
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(copy)));
        QCOMPARE(m_fmt->documentPageWidth(), 300.);
        m_fmt->configurePages(300, 200, 0, {10,10,10,10});
        QCOMPARE(paint(), before);
        QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
        const QString asyncCopy = m_dir.filePath("page-bands-async.odt");
        QTest::failOnWarning(QRegularExpression(".*different thread.*"));
        QSignalSpy saved(m_fmt, &RichTextHandler::saveCopyFinished);
        m_fmt->saveOdfCopyAsync(QUrl::fromLocalFile(asyncCopy));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(saved[0][0].toBool());
        QCOMPARE(QZipReader(asyncCopy).fileData("styles.xml"), QZipReader(copy).fileData("styles.xml"));
        QTextCursor cursor(doc()); cursor.movePosition(QTextCursor::End);
        QTextBlockFormat next; next.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
        cursor.insertBlock(next); cursor.insertText("Second page");
        m_fmt->configurePages(300, 200, 0, {10,10,10,10});
        QCOMPARE(doc()->documentLayout()->pageCount(), 2);
        const auto lines = rt::pageBandLines(doc(), 1, 2);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[1].text, QStringLiteral("Page 2 of 2"));
        QCOMPARE(lines[1].color, QColor("#cc0000"));
        QVERIFY(qAbs(lines[1].rect.center().x() - 150) < 1);
        QVERIFY(qAbs(lines[1].rect.top() - 360) < 1);
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QQuickWindow window; window.setColor(Qt::white); window.resize(300,200);
        auto *edit = qobject_cast<QQuickItem *>(m_edit.data());
        edit->setParentItem(window.contentItem());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (const QString &engine : {QStringLiteral("custom"), QStringLiteral("qt")}) {
            m_fmt->setLayoutEngine(engine);
            m_fmt->configurePages(300, 200, 0, {10,10,10,10});
            edit->setY(-200);
            QTRY_VERIFY(edit->findChild<QQuickItem *>("footerBand"));
            QTRY_COMPARE(edit->findChild<QQuickItem *>("footerBand")->property("page").toInt(), 2);
            const auto redPixels = [&] {
                const auto image = window.grabWindow(); int count = 0;
                for (int y=155; y<185; ++y) for (int x=80; x<220; ++x) {
                    const auto c = image.pixelColor(x,y); if (c.red() > c.green()+80) ++count;
                }
                return count;
            };
            QTRY_VERIFY(redPixels() > 20);
            edit->setY(0);
            QTRY_COMPARE(edit->findChild<QQuickItem *>("footerBand")->property("page").toInt(), 1);
        }
        edit->setParentItem(nullptr);
        m_fmt->newDocument();
        QVERIFY(rt::pageBandLines(doc(), 0, 1).isEmpty());
    }

    void odfActiveFooterReservesBodySpace()
    {
        const QString path = m_dir.filePath("footer-space.odt");
        for (bool active : {true, false}) {
            QZipWriter zip(path);
            zip.addFile("content.xml", "<office:document-content" ODF_NS "><office:body><office:text><text:p>One</text:p><text:p>Two</text:p><text:p>Three</text:p></office:text></office:body></office:document-content>");
            zip.addFile("styles.xml", QByteArray("<office:document-styles" ODF_NS "><office:styles><style:default-style style:family=\"paragraph\"><style:paragraph-properties fo:line-height=\"20px\"/></style:default-style></office:styles><office:automatic-styles>"
                "<style:page-layout style:name=\"Page\"><style:page-layout-properties fo:page-width=\"300px\" fo:page-height=\"100px\" fo:margin=\"10px\"/><style:footer-style><style:header-footer-properties fo:min-height=\"20px\" fo:margin-top=\"10px\"/></style:footer-style></style:page-layout>"
                "</office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"Page\">")
                + (active ? "<style:footer><text:p>Footer</text:p></style:footer>" : "")
                + "</style:master-page></office:master-styles></office:document-styles>");
            zip.close();
            QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
            m_edit->setProperty("width", 300);
            m_fmt->configurePages(300, 100, 0, {10,10,10,10});
            QCOMPARE(m_fmt->documentPageMargins(), QVariantList({10,10,10,10}));
            QCOMPARE(doc()->rootFrame()->frameFormat().bottomMargin(), active ? 30.0 : 10.0);
        }
        m_fmt->newDocument();
        m_fmt->configurePages(300, 100, 0, {10,10,10,10});
        QCOMPARE(doc()->rootFrame()->frameFormat().bottomMargin(), 10.0);
    }

    void downscaledImagesUseAreaSampling()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QQuickWindow window; window.setColor(Qt::white); window.resize(300, 600);
        auto *edit = qobject_cast<QQuickItem *>(m_edit.data());
        edit->setParentItem(window.contentItem());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        const qreal dpr = window.effectiveDevicePixelRatio();
        QImage source(1024, 1024, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, (x % 4 == 0) ? Qt::black : Qt::white);
        const QString png = m_dir.filePath("stripes.png");
        QVERIFY(source.save(png));
        for (const QString &engine : {QStringLiteral("custom"), QStringLiteral("qt")}) {
            m_fmt->newDocument();
            m_fmt->setLayoutEngine(engine);
            QVERIFY(m_fmt->insertImage(QUrl::fromLocalFile(png)));
            m_edit->setProperty("width", 300);
            m_fmt->configurePages(300, 600, 0, {10,10,10,10});
            QTextCursor cursor(doc()); cursor.setPosition(0); cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            auto format = cursor.charFormat().toImageFormat();
            format.setWidth(64); format.setHeight(64); cursor.setCharFormat(format);
            m_fmt->configurePages(300, 600, 0, {10,10,10,10});
            const auto sampled = doc()->resource(QTextDocument::ImageResource, QUrl(format.name())).value<QImage>();
            QCOMPARE(sampled.size(), QSize(qRound(64*dpr), qRound(64*dpr)));
            QVERIFY2(qAbs(sampled.pixelColor(20,20).red() - 191) <= 1, qPrintable(QString::number(sampled.pixelColor(20,20).red())));
            const auto key = sampled.cacheKey();
            m_fmt->configurePages(300, 600, 0, {10,10,10,10});
            QCOMPARE(doc()->resource(QTextDocument::ImageResource, QUrl(format.name())).value<QImage>().cacheKey(), key);
            QImage output(300,600,QImage::Format_RGB32); output.fill(Qt::white);
            QPainter painter(&output); doc()->documentLayout()->draw(&painter, {}); painter.end();
            QVERIFY(qAbs(output.pixelColor(30,30).red() - 191) <= 1);
            QTRY_VERIFY(qAbs(window.grabWindow().pixelColor(qRound(30 * window.effectiveDevicePixelRatio()), qRound(30 * window.effectiveDevicePixelRatio())).red() - 191) <= 2);
            const QString copy = m_dir.filePath("stripes.odt");
            QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
            QCOMPARE(QImage::fromData(QZipReader(copy).fileData("Pictures/image1.png")), source);
            m_fmt->setZoom(2);
            QCOMPARE(doc()->resource(QTextDocument::ImageResource, QUrl(format.name())).value<QImage>().size(), QSize(qRound(128*dpr), qRound(128*dpr)));
            m_fmt->setZoom(1);
            format.clearProperty(QTextFormat::ImageWidth); format.setHeight(32);
            cursor.setCharFormat(format);
            m_fmt->configurePages(300,600,0,{10,10,10,10});
            QCOMPARE(doc()->resource(QTextDocument::ImageResource, QUrl(format.name())).value<QImage>().size(), QSize(qRound(32*dpr), qRound(32*dpr)));
        }
        edit->setParentItem(nullptr);
    }

    void odfRelativeImagesFollowTextWidthAndAspect()
    {
        QImage image(1000, 500, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        const QString path = m_dir.filePath("relative-image.odt");
        writeOdt(path, "<text:p><draw:frame text:anchor-type=\"as-char\" svg:width=\"1000px\" svg:height=\"500px\" style:rel-width=\"100%\" style:rel-height=\"scale\"><draw:image xlink:href=\"Pictures/image.png\"/></draw:frame></text:p>", {}, {}, png);
        m_fmt->setLayoutEngine("custom");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_edit->setProperty("width", 300);
        m_fmt->configurePages(300, 600, 0, {10,10,10,10});
        const auto check = [&](qreal width) {
            doc()->documentLayout()->documentSize();
            const auto line = doc()->firstBlock().layout()->lineAt(0);
            QCOMPARE(line.naturalTextWidth(), width);
            QVERIFY(qAbs(line.ascent() - width / 2) <= 1.0 / 64);
            QImage rendered(500, 600, QImage::Format_RGB32); rendered.fill(Qt::white);
            QPainter painter(&rendered);
            doc()->documentLayout()->draw(&painter, {});
            painter.end();
            QCOMPARE(rendered.pixelColor(30, 30), QColor(Qt::red));
        };
        check(280);
        m_edit->setProperty("width", 500);
        m_fmt->configurePages(500, 600, 0, {10,20,10,20});
        check(460);
        const QString copy = m_dir.filePath("relative-image-copy.odt");
        QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
        keepForInspection(copy);
        QVERIFY(QZipReader(copy).fileData("content.xml").contains("style:rel-width=\"100%\""));
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(copy)));
        QCOMPARE(charAt(doc(), 0).doubleProperty(rt::kRelativeImageWidthProperty), 100.0);
        m_edit->setProperty("width", 300);
        m_fmt->configurePages(300, 600, 0, {10,10,10,10});
        check(280);
        m_fmt->setLayoutEngine("qt");
        check(280);
    }

    void odfListLabelWidthControlsTextIndent()
    {
        const QString path = m_dir.filePath("list-indent.odt");
        writeOdt(path, "<text:list text:style-name=\"Bullets\"><text:list-item><text:p>Outer</text:p><text:list><text:list-item><text:p>Inner</text:p></text:list-item></text:list></text:list-item></text:list>", {},
            "<text:list-style style:name=\"Bullets\"><text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"•\"><style:list-level-properties text:space-before=\"0px\" text:min-label-width=\"15px\"/></text:list-level-style-bullet>"
            "<text:list-level-style-bullet text:level=\"2\" text:bullet-char=\"•\"><style:list-level-properties text:space-before=\"15px\" text:min-label-width=\"15px\"/></text:list-level-style-bullet></text:list-style>");
        m_fmt->setLayoutEngine("custom");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_fmt->configurePages(300, 300, 0, {10,10,10,10});
        const auto x = [&](int n) {
            doc()->documentLayout()->documentSize();
            const auto block = doc()->findBlockByNumber(n);
            return block.layout()->position().x() + block.layout()->lineAt(0).x();
        };
        QCOMPARE(x(0), 25.0);
        QCOMPARE(x(1), 40.0);
        const QString copy = m_dir.filePath("list-indent-copy.odt");
        QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
        keepForInspection(copy);
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(copy)));
        m_fmt->configurePages(300, 300, 0, {10,10,10,10});
        QCOMPARE(doc()->firstBlock().blockFormat().doubleProperty(rt::kListTextOffsetProperty), 15.0);
        QCOMPARE(doc()->firstBlock().blockFormat().leftMargin(), 0.0);
        QCOMPARE(x(0), 25.0);
        QCOMPARE(x(1), 40.0);
    }

    void odfParagraphSpacingAddsAndResetsAtPageTop()
    {
        const QString path = m_dir.filePath("paragraph-spacing.odt");
        writeOdt(path, "<text:p text:style-name=\"Spaced\">One</text:p><text:p text:style-name=\"Spaced\">Two</text:p><text:p text:style-name=\"LargeGap\">Three</text:p>", {},
            "<style:style style:name=\"Spaced\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"6px\" fo:margin-bottom=\"6px\" fo:line-height=\"20px\"/></style:style>"
            "<style:style style:name=\"LargeGap\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"60px\" fo:line-height=\"20px\"/></style:style>");
        m_fmt->setLayoutEngine("custom");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        m_fmt->configurePages(300, 100, 0, {10,10,10,10});
        doc()->documentLayout()->documentSize();
        const auto y = [&](int n) {
            const auto block = doc()->findBlockByNumber(n);
            return block.layout()->position().y() + block.layout()->lineAt(0).y();
        };
        // Fixed line-height positions include the same baseline offset in both blocks.
        QCOMPARE(y(1) - y(0), 32.0);
        QCOMPARE(y(2) - y(0), 100.0);
    }

    void odfLanguagesSurviveStylesAndRoundTrip()
    {
        const QString path = m_dir.filePath("languages.odt");
        writeOdt(path, "<text:p>Default</text:p><text:p text:style-name=\"AutoPara\">French <text:span text:style-name=\"Portuguese\">BR</text:span><text:span text:style-name=\"AutoChar\">DE</text:span></text:p>",
            "<style:style style:name=\"AutoPara\" style:family=\"paragraph\" style:parent-style-name=\"French\"/>"
            "<style:style style:name=\"AutoChar\" style:family=\"text\"><style:text-properties fo:language=\"de\" fo:country=\"DE\"/></style:style>",
            "<style:default-style style:family=\"paragraph\"><style:text-properties fo:language=\"en\" fo:country=\"US\"/></style:default-style>"
            "<style:style style:name=\"French\" style:family=\"paragraph\"><style:text-properties fo:language=\"fr\" fo:country=\"FR\"/></style:style>"
            "<style:style style:name=\"Portuguese\" style:family=\"text\"><style:text-properties fo:language=\"pt\" fo:country=\"BR\"/></style:style>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        QCOMPARE(m_fmt->property("documentLanguage").toString(), QString("en-US"));
        const auto language = [&](int position) {
            QString value;
            QMetaObject::invokeMethod(m_fmt, "languageAt", Q_RETURN_ARG(QString, value), Q_ARG(int, position));
            return value;
        };
        const auto blockLanguage = [&](int position) {
            QString value;
            QMetaObject::invokeMethod(m_fmt, "blockLanguage", Q_RETURN_ARG(QString, value), Q_ARG(QString, m_fmt->blockIdAt(position)));
            return value;
        };
        QCOMPARE(language(0), QString("en-US"));
        QCOMPARE(blockLanguage(0), QString());
        QCOMPARE(language(8), QString("fr-FR"));
        QCOMPARE(blockLanguage(8), QString("fr-FR"));
        QCOMPARE(language(15), QString("pt-BR"));
        QCOMPARE(language(17), QString("de-DE"));
        QSignalSpy changes(m_fmt, SIGNAL(documentLanguageChanged()));
        QVERIFY(changes.isValid());
        const QString copy = m_dir.filePath("languages-copy.odt");
        QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(copy)));
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(copy)));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(language(15), QString("pt-BR"));
        QCOMPARE(language(17), QString("de-DE"));
        QCOMPARE(blockLanguage(8), QString("fr-FR"));
        m_fmt->newDocument();
        QCOMPARE(changes.count(), 1);
        QCOMPARE(m_fmt->property("documentLanguage").toString(), QString());
    }

    void initTestCase()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QmlCss::registerTypes();
        qmlRegisterType<RichTextHandler>("solidqml.native", 1, 0, "RichTextHandler");
    }

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

    void odfUsesDocumentDefaultFont()
    {
        const QString path = m_dir.filePath("default-font.odt");
        writeOdt(path, "<text:p>Body <text:span>span</text:span></text:p>", {},
            "<style:default-style style:family=\"paragraph\"><style:text-properties fo:font-family=\"Liberation Serif\" fo:font-size=\"12pt\"/></style:default-style>");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        const auto f = doc()->begin().begin().fragment().charFormat();
        QCOMPARE(f.font().resolve(doc()->defaultFont()).family(), QString("Liberation Serif"));
        QCOMPARE(f.font().resolve(doc()->defaultFont()).pointSizeF(), 12.0);
        for (auto it = doc()->begin().begin(); !it.atEnd(); ++it)
            QCOMPARE(it.fragment().charFormat().font().resolve(doc()->defaultFont()).family(), QString("Liberation Serif"));
    }

    void odfDocumentPageGeometry()
    {
        const QString path = m_dir.filePath("geometry.odt");
        QZipWriter zip(path);
        zip.addFile("content.xml", "<office:document-content" ODF_NS "><office:automatic-styles>"
            "<style:style style:name=\"First\" style:family=\"paragraph\" style:parent-style-name=\"Body\"/>"
            "</office:automatic-styles><office:body><office:text><text:p text:style-name=\"First\">Hello</text:p>"
            "</office:text></office:body></office:document-content>");
        zip.addFile("styles.xml", "<office:document-styles" ODF_NS "><office:styles>"
            "<style:style style:name=\"Body\" style:family=\"paragraph\" style:master-page-name=\"Letter\"/>"
            "</office:styles><office:automatic-styles>"
            "<style:page-layout style:name=\"unused\"><style:page-layout-properties fo:page-width=\"21cm\" fo:page-height=\"29.7cm\"/></style:page-layout>"
            "<style:page-layout style:name=\"used\"><style:page-layout-properties fo:page-width=\"8.5in\" fo:page-height=\"11in\" fo:margin=\"1in\" fo:margin-left=\"0.5in\"/></style:page-layout>"
            "</office:automatic-styles><office:master-styles>"
            "<style:master-page style:name=\"Standard\" style:page-layout-name=\"unused\"/>"
            "<style:master-page style:name=\"Letter\" style:page-layout-name=\"used\"/>"
            "</office:master-styles></office:document-styles>");
        zip.close();
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        QCOMPARE(m_fmt->property("documentPageWidth").toReal(), 816.0);
        QCOMPARE(m_fmt->property("documentPageHeight").toReal(), 1056.0);
        QCOMPARE(m_fmt->property("documentPageMargins").toList(), QVariantList({96,96,96,48}));
        const QString saved = m_dir.filePath("geometry-saved.odt");
        QVERIFY(m_fmt->saveOdfCopy(QUrl::fromLocalFile(saved)));
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(saved)));
        QCOMPARE(m_fmt->documentPageWidth(), 816.0);
        QCOMPARE(m_fmt->documentPageMargins(), QVariantList({96,96,96,48}));
        QSignalSpy changes(m_fmt, SIGNAL(documentPageGeometryChanged()));
        QVERIFY(changes.isValid());
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(path)));
        QCOMPARE(changes.count(), 0);
        m_fmt->newDocument();
        QCOMPARE(changes.count(), 1);
        QCOMPARE(m_fmt->property("documentPageWidth").toReal(), 0.0);
    }

    void fontAtCaretOnlyFormatsNewText()
    {
        m_fmt->insertText(QStringLiteral("hello"));
        m_fmt->setCursorPosition(2);
        m_edit->setProperty("cursorPosition", 2);
        const QTextCharFormat original = charAt(doc(), 1);
        doc()->setModified(false);
        const int undo = doc()->availableUndoSteps();
        m_fmt->setFontFamily(QStringLiteral("X"));
        m_fmt->setFontSize(23);
        QCOMPARE(m_fmt->fontFamily(), QStringLiteral("X"));
        QCOMPARE(m_fmt->fontSize(), 23.0);
        QVERIFY(!doc()->isModified());
        QCOMPARE(doc()->availableUndoSteps(), undo);
        for (int i = 0; i < 5; ++i)
            QCOMPARE(charAt(doc(), i), original);
        m_fmt->insertText(QStringLiteral("ab"));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("heabllo"));
        for (int i = 0; i < 7; ++i) {
            if (i == 2 || i == 3) {
                QCOMPARE(charAt(doc(), i).fontFamilies().toStringList(), QStringList{QStringLiteral("X")});
                QCOMPARE(charAt(doc(), i).fontPointSize(), 23.0);
            } else {
                QCOMPARE(charAt(doc(), i), original);
            }
        }
        m_fmt->setCursorPosition(6);
        QVERIFY(m_fmt->fontFamily() != QStringLiteral("X"));
    }

    void pendingFontUsesTheNativeKeyboardCursor()
    {
        QQuickWindow window;
        window.resize(300, 100);
        auto *edit = qobject_cast<QQuickItem *>(m_edit.data());
        edit->setParentItem(window.contentItem());
        edit->setSize(QSizeF(300, 100));
        m_fmt->insertText(QStringLiteral("hello"));
        m_edit->setProperty("cursorPosition", 2);
        m_fmt->setCursorPosition(2);
        m_fmt->setFontFamily(QStringLiteral("X"));
        m_fmt->setFontSize(23);
        edit->forceActiveFocus();
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::keyClick(&window, Qt::Key_A);
        QTest::keyClick(&window, Qt::Key_B);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("heabllo"));
        for (int i = 0; i < 7; ++i)
            QCOMPARE(charAt(doc(), i).fontFamilies().toStringList().value(0),
                     i == 2 || i == 3 ? QStringLiteral("X") : QString());
        // Moving without typing discards a newly pending format.
        m_edit->setProperty("cursorPosition", 5);
        m_fmt->setCursorPosition(5);
        m_fmt->setFontFamily(QStringLiteral("Y"));
        m_edit->setProperty("cursorPosition", 6);
        m_fmt->setCursorPosition(6);
        QVERIFY(m_fmt->fontFamily() != QStringLiteral("Y"));
        edit->setParentItem(nullptr);
    }

    void pendingFontSizeStaysCanonicalAcrossZoom()
    {
        m_fmt->insertText(QStringLiteral("hello"));
        m_fmt->setCursorPosition(2);
        m_fmt->setFontSize(23);
        m_fmt->setZoom(2);
        QCOMPARE(m_fmt->fontSize(), 23.0);
        m_fmt->insertText(QStringLiteral("ab"));
        QCOMPARE(charAt(doc(), 2).fontPointSize(), 46.0);
        QCOMPARE(charAt(doc(), 3).fontPointSize(), 46.0);
    }

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

    void asyncCopyMatchesBookAndLeavesDocumentAlone()
    {
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(book)));
        const QString sync = m_dir.filePath(QStringLiteral("sync.odt"));
        const QString async = m_dir.filePath(QStringLiteral("async.odt"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(m_fmt->saveOdfCopy(QUrl(sync)));
        const double syncMs = timer.nsecsElapsed() / 1e6;
        const bool modified = doc()->isModified();
        const int undo = doc()->availableUndoSteps();
        QSignalSpy finished(m_fmt, SIGNAL(saveCopyFinished(bool,QString)));
        QVERIFY(finished.isValid());
        bool onUiThread = false;
        connect(m_fmt, &RichTextHandler::saveCopyFinished, this, [&] {
            onUiThread = QThread::currentThread() == m_fmt->thread();
        }, Qt::DirectConnection);
        timer.restart();
        m_fmt->saveOdfCopyAsync(QUrl(async));
        const double snapshotMs = timer.nsecsElapsed() / 1e6;
        // Edit the live document immediately: the worker must see the earlier snapshot.
        m_fmt->insertText(QStringLiteral("not in the snapshot"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
        QVERIFY(onUiThread);
        QVERIFY2(finished[0][0].toBool(), qPrintable(finished[0][1].toString()));
        for (const QString &entry : {QStringLiteral("content.xml"), QStringLiteral("styles.xml"),
                                     QStringLiteral("META-INF/manifest.xml")})
            QCOMPARE(zipEntry(async, entry), zipEntry(sync, entry));
        qInfo("autosave book: synchronous %.3f ms; UI snapshot call %.3f ms", syncMs, snapshotMs);
        doc()->undo();
        QCOMPARE(doc()->isModified(), modified);
        QCOMPARE(doc()->availableUndoSteps(), undo);
    }

    void asyncCopyCoalescesAndCarriesImages()
    {
        m_fmt->insertText(QStringLiteral("original"));
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::red);
        const QString png = m_dir.filePath(QStringLiteral("async.png"));
        QVERIFY(image.save(png));
        m_fmt->setCursorPosition(8);
        QVERIFY(m_fmt->insertImage(QUrl(png)));
        const QString sync = m_dir.filePath(QStringLiteral("image-sync.odt"));
        QVERIFY(m_fmt->saveOdfCopy(QUrl(sync)));
        QSignalSpy finished(m_fmt, SIGNAL(saveCopyFinished(bool,QString)));
        const QString first = m_dir.filePath(QStringLiteral("first.odt"));
        const QString skipped = m_dir.filePath(QStringLiteral("skipped.odt"));
        const QString last = m_dir.filePath(QStringLiteral("last.odt"));
        m_fmt->saveOdfCopyAsync(QUrl(first));
        m_fmt->saveOdfCopyAsync(QUrl(skipped));
        m_fmt->saveOdfCopyAsync(QUrl(last));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 30000);
        QVERIFY(!QFile::exists(skipped));
        for (const QString &path : {first, last}) {
            QZipReader zip(sync);
            for (const auto &entry : zip.fileInfoList())
                if (entry.filePath != QStringLiteral("meta.xml"))
                    QCOMPARE(zipEntry(path, entry.filePath), zipEntry(sync, entry.filePath));
        }
        const bool modified = doc()->isModified();
        const int undo = doc()->availableUndoSteps();
        m_fmt->saveOdfCopyAsync(QUrl(m_dir.filePath(QStringLiteral("missing/x.odt"))));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 30000);
        QVERIFY(!finished[2][0].toBool());
        QVERIFY(!finished[2][1].toString().isEmpty());
        QCOMPARE(doc()->isModified(), modified);
        QCOMPARE(doc()->availableUndoSteps(), undo);
    }

    void pdfExportBook()
    {
        const auto book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (book.isEmpty()) QSKIP("Set RICHTEXT_BOOK");
        m_edit->setProperty("wrapMode", QQuickTextEdit::Wrap);
        m_edit->setProperty("width", 794);
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(book)));
        m_fmt->configurePages(794, 1123, 20, {94, 113, 94, 113});
        const int pages = m_fmt->pageCount();
        QSignalSpy done(m_fmt, &RichTextHandler::pdfExported);
        const auto rss = [] {
            QFile status("/proc/self/status"); if (!status.open(QIODevice::ReadOnly)) return qint64(0);
            const auto match = QRegularExpression("VmRSS:\\s+(\\d+)").match(QString::fromUtf8(status.readAll()));
            return match.captured(1).toLongLong();
        };
        const auto beforeRss = rss();
        auto peakRss = beforeRss;
        QSignalSpy progress(m_fmt, &RichTextHandler::pdfProgress);
        QList<qint64> progressTimes;
        QElapsedTimer wall, interval;
        connect(m_fmt, &RichTextHandler::pdfProgress, this, [&] { progressTimes.append(wall.elapsed()); });
        qint64 stall = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { stall = qMax(stall, interval.restart()); peakRss = qMax(peakRss, rss()); });
        heartbeat.start(0);
        wall.start(); interval.start();
        const QString path = m_dir.filePath("book.pdf");
        QVERIFY(m_fmt->exportPdfAsync(path));
        const auto snapshotMs = wall.elapsed();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 120000);
        heartbeat.stop();
        QVERIFY2(done[0][0].toBool(), qPrintable(done[0][2].toString()));
        QCOMPARE(done[0][3].toInt(), pages);
        qInfo() << "PDF book pages" << pages << "wall ms" << wall.elapsed()
                << "snapshot ms" << snapshotMs << "max GUI interval ms" << stall
                << "peak RSS delta KiB" << peakRss - beforeRss;
        QVERIFY(!progress.isEmpty());
        int previousPage = 0;
        for (int i = 0; i < progress.size(); ++i) {
            QVERIFY(progress[i][0].toInt() > previousPage);
            previousPage = progress[i][0].toInt();
            QCOMPARE(progress[i][1].toInt(), pages);
            if (i) QVERIFY(progressTimes[i] - progressTimes[i-1] >= 90);
        }
        keepForInspection(path);
        QProcess info;
        info.start("pdfinfo", {path});
        QVERIFY(info.waitForFinished());
        QVERIFY(info.readAllStandardOutput().contains("595.5 x 842.25 pts"));
        const QString editedPath = m_dir.filePath("book-edited.pdf");
        QVERIFY(m_fmt->exportPdfAsync(editedPath));
        m_fmt->newDocument();
        m_fmt->insertText(QStringLiteral("EDIT DURING BOOK EXPORT"));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 120000);
        QVERIFY(done[1][0].toBool());
        QCOMPARE(done[1][3].toInt(), pages);
        QProcess process;
        process.start("pdftotext", {path, "-"});
        QVERIFY(process.waitForFinished());
        const auto baseline = process.readAllStandardOutput();
        process.start("pdftotext", {editedPath, "-"});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.readAllStandardOutput(), baseline);

    }

    void pdfExportFixture_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::newRow("custom") << QString("custom");
        QTest::newRow("qt") << QString("qt");
    }
    void pdfExportFixture()
    {
        QFETCH(QString, engine);
        m_fmt->setLayoutEngine(engine);
        QQuickView view;
        view.setColor(Qt::white);
        view.resize(400, 300);
        auto *editor = qobject_cast<QQuickItem *>(m_edit.data());
        editor->setParentItem(view.contentItem());
        editor->setWidth(400);
        QString html = "<p>Vector export fixture</p><ul><li>Bullet alpha</li><li>Bullet beta</li></ul>"
                       "<table border='2' style='border-style:solid' cellspacing='0' cellpadding='4' width='100%' bgcolor='#ddeeff'>";
        for (int i = 0; i < 18; ++i) html += QString("<tr><td>Table row %1</td></tr>").arg(i);
        html += "</table><p><span style='color:#cc0000;background-color:#ffff00'>Colour highlight</span> "
                "<u>Underline</u> <s>Strike</s></p><p>Sub<sub>script</sub> Super<sup>script</sup></p>";
        doc()->setHtml(html);
        QImage source(240, 160, QImage::Format_RGB32); source.fill(QColor("#00aa44"));
        doc()->addResource(QTextDocument::ImageResource, QUrl("fixture-image"), source);
        QTextCursor cursor(doc()); cursor.movePosition(QTextCursor::End); cursor.insertBlock();
        QTextImageFormat image; image.setName("fixture-image"); image.setWidth(60); image.setHeight(40);
        cursor.insertImage(image);
        cursor.insertBlock();
        QTextBlockFormat breakFormat; breakFormat.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
        cursor.setBlockFormat(breakFormat); cursor.insertText("Explicit final page");
        m_fmt->configurePages(400, 300, 20, {20, 20, 20, 20});
        view.show();
        QTest::qWait(50);
        QCOMPARE(bool(dynamic_cast<rt::RichTextLayout *>(doc()->documentLayout())), engine == "custom");
        const QImage screen = view.grabWindow();
        QVERIFY(!screen.isNull());
        const int pages = m_fmt->pageCount();
        QVERIFY(pages >= 3);
        struct Line { QString text; int page; qreal y; };
        QList<Line> expected;
        for (auto block = doc()->begin(); block.isValid(); block = block.next()) {
            auto *layout = block.layout();
            for (int i = 0; i < layout->lineCount(); ++i) {
                const auto line = layout->lineAt(i);
                const auto text = block.text().mid(line.textStart(), line.textLength()).simplified();
                if (text.isEmpty() || text.contains(QChar::ObjectReplacementCharacter)) continue;
                const auto y = doc()->documentLayout()->blockBoundingRect(block).y() + line.y();
                expected.append({text, int(y / 320) + 1, y - int(y / 320) * 320});
            }
        }
        QSignalSpy done(m_fmt, &RichTextHandler::pdfExported);
        const auto path = m_dir.filePath("fixture.pdf");
        QVERIFY(m_fmt->exportPdfAsync(path));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY2(done[0][0].toBool(), qPrintable(done[0][2].toString()));
        QCOMPARE(done[0][3].toInt(), pages);
        QProcess process;
        process.start("pdftotext", {"-bbox-layout", path, "-"});
        QVERIFY(process.waitForFinished());
        QXmlStreamReader xml(process.readAllStandardOutput());
        QList<Line> actual;
        int page = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("page")) ++page;
            if (xml.name() != QLatin1String("line")) continue;
            const qreal y = xml.attributes().value("yMin").toDouble() * 4 / 3;
            QStringList words;
            while (!(xml.isEndElement() && xml.name() == QLatin1String("line")) && !xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == QLatin1String("word")) words.append(xml.readElementText());
            }
            QString text = words.join(' ').normalized(QString::NormalizationForm_KC);
            if (text.startsWith(QChar(0x2022))) text = text.mid(1).trimmed();
            actual.append({text, page, y});
        }
        QVERIFY(!xml.hasError());
        QCOMPARE(actual.size(), expected.size());
        for (int i = 0; i < expected.size(); ++i) {
            // Poppler inserts word boundaries at sub/superscript run changes.
            QCOMPARE(QString(actual[i].text).remove(' '), QString(expected[i].text).remove(' '));
            QCOMPARE(actual[i].page, expected[i].page);
            QVERIFY2(qAbs(actual[i].y - expected[i].y) < 2, qPrintable(QString("%1: PDF y=%2, screen y=%3").arg(expected[i].text).arg(actual[i].y).arg(expected[i].y)));
        }
        process.start("pdffonts", {path});
        QVERIFY(process.waitForFinished());
        const auto fonts = QString::fromUtf8(process.readAllStandardOutput()).split('\n');
        QVERIFY(fonts.size() > 3);
        for (const auto &font : fonts.mid(2)) {
            if (font.trimmed().isEmpty()) continue;
            const auto columns = font.simplified().split(' ');
            QCOMPARE(columns[columns.size() - 5], QString("yes"));
        }
        process.start("pdfimages", {"-list", path});
        QVERIFY(process.waitForFinished());
        const auto images = QString::fromUtf8(process.readAllStandardOutput());
        QVERIFY(QRegularExpression("image\\s+240\\s+160").match(images).hasMatch());
        const auto raster = m_dir.filePath("fixture");
        process.start("pdftoppm", {"-r", "96", "-f", "1", "-singlefile", "-png", path, raster});
        QVERIFY(process.waitForFinished());
        const QImage pdf(raster + ".png");
        QCOMPARE(pdf.size(), screen.size());
        qint64 difference = 0;
        for (int y = 0; y < pdf.height(); ++y) for (int x = 0; x < pdf.width(); ++x) {
            const auto a = pdf.pixelColor(x, y), b = screen.pixelColor(x, y);
            difference += qAbs(a.red()-b.red()) + qAbs(a.green()-b.green()) + qAbs(a.blue()-b.blue());
        }
        const qreal mean = qreal(difference) / (pdf.width() * pdf.height() * 3);
        qInfo() << "PDF table vs QSG screen mean RGB error" << mean;
        QVERIFY(mean < 12);
        auto *table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().first());
        QVERIFY(table);
        const auto tableRect = doc()->documentLayout()->frameBoundingRect(table);
        int borderPixels = 0, matchingBorderPixels = 0;
        // Table frame rectangles include cell padding differently in the two
        // engines. Find the strongest vertical edge in the narrow left gutter.
        for (int x = qMax(0, qFloor(tableRect.left()) - 12); x < qMin(screen.width(), qCeil(tableRect.left()) + 4); ++x) {
            int pixels = 0, matching = 0;
            for (int y = qCeil(tableRect.top()) + 5; y < screen.height() - 20; ++y) {
                if (screen.pixelColor(x, y).lightness() > 220) continue;
                ++pixels;
                bool matches = false;
                for (int dx = -1; dx <= 1; ++dx)
                    if (x + dx >= 0 && x + dx < pdf.width() && pdf.pixelColor(x + dx, y).lightness() < 220) matches = true;
                if (matches) ++matching;
            }
            if (pixels > borderPixels) { borderPixels = pixels; matchingBorderPixels = matching; }
        }
        QVERIFY(borderPixels > 100);
        QVERIFY(matchingBorderPixels > borderPixels * 0.9);

        QVERIFY(!m_fmt->addMark("spelling", m_fmt->blockIdAt(0), 0, 6).isEmpty());
        QVERIFY(!m_fmt->addMark("llm", m_fmt->blockIdAt(0), 7, 13).isEmpty());
        QVERIFY(QMetaObject::invokeMethod(m_edit.data(), "select", Q_ARG(int, 0), Q_ARG(int, 13)));
        QVERIFY(m_fmt->exportPdfAsync(path));
        QTRY_COMPARE(done.size(), 2);
        QVERIFY(done[1][0].toBool());
        process.start("pdftoppm", {"-r", "96", "-f", "1", "-singlefile", "-png", path, raster});
        QVERIFY(process.waitForFinished());
        QCOMPARE(QImage(raster + ".png"), pdf);
        editor->setParentItem(nullptr);
    }

    void pdfExportFailure()
    {
        QVERIFY(!m_fmt->exportPdfAsync(m_dir.filePath("unpaginated.pdf")));
        m_fmt->configurePages(400, 500, 20, {30, 40, 30, 40});
        QSignalSpy done(m_fmt, &RichTextHandler::pdfExported);
        QVERIFY(m_fmt->exportPdfAsync(m_dir.filePath("missing/export.pdf")));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][0].toBool());
        QVERIFY(!done[0][2].toString().isEmpty());
        QCOMPARE(done[0][3].toInt(), 0);
        QVERIFY(m_fmt->exportPdfAsync(m_dir.filePath("recovery.pdf")));
        QTRY_COMPARE(done.size(), 2);
        QVERIFY(done[1][0].toBool());
    }

    void pdfExportSnapshot_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::addColumn<int>("resolution");
        QTest::addColumn<qreal>("zoom");
        QTest::newRow("custom-96") << QString("custom") << 96 << qreal(1);
        QTest::newRow("custom-600") << QString("custom") << 600 << qreal(1);
        QTest::newRow("qt-96") << QString("qt") << 96 << qreal(1);
        QTest::newRow("custom-zoom") << QString("custom") << 96 << qreal(2);
    }
    void pdfExportSnapshot()
    {
        QFETCH(QString, engine);
        QFETCH(int, resolution);
        QFETCH(qreal, zoom);
        m_fmt->setLayoutEngine(engine);
        m_fmt->insertText(QStringLiteral("Snapshot first page"));
        m_fmt->insertPageBreak();
        m_fmt->insertText(QStringLiteral("Snapshot second page"));
        m_fmt->setZoom(zoom);
        m_edit->setProperty("width", 400 * zoom);
        m_fmt->configurePages(400 * zoom, 500 * zoom, 20 * zoom, {30 * zoom, 40 * zoom, 30 * zoom, 40 * zoom});
        doc()->setPageSize(QSizeF(400 * zoom, 520 * zoom));
        QSignalSpy done(m_fmt, &RichTextHandler::pdfExported);
        const QString path = m_dir.filePath("snapshot.pdf");
        QVERIFY(m_fmt->exportPdfAsync(path, {{"title", "PDF fixture"}, {"author", "PDF author"}, {"resolution", resolution}}));
        QVERIFY(!m_fmt->exportPdfAsync(path));
        m_fmt->newDocument();
        m_fmt->insertText(QStringLiteral("EDIT MUST NOT LEAK"));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY2(done[0][0].toBool(), qPrintable(done[0][2].toString()));
        QCOMPARE(done[0][3].toInt(), 2);
        QProcess process;
        process.start("pdftotext", {path, "-"});
        QVERIFY(process.waitForFinished());
        const auto text = QString::fromUtf8(process.readAllStandardOutput()).normalized(QString::NormalizationForm_KC);
        QVERIFY2(text.contains("Snapshot first page"), qPrintable(text));
        QVERIFY(text.contains("Snapshot second page"));
        QVERIFY(!text.contains("EDIT MUST NOT LEAK"));
        process.start("pdfinfo", {path});
        QVERIFY(process.waitForFinished());
        const auto info = process.readAllStandardOutput();
        QVERIFY(info.contains("300 x 375 pts"));
        QVERIFY(info.contains("PDF fixture"));
        QVERIFY(info.contains("PDF author"));
        QVERIFY(info.contains("office"));
        process.start("pdffonts", {path});
        QVERIFY(process.waitForFinished());
        QVERIFY(process.readAllStandardOutput().contains("yes"));
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
        // Imported body text uses the document default, independently of the editor's font.
        const QVariantMap standard = m_fmt->styleProperties(QStringLiteral("Default Paragraph Style"));
        QCOMPARE(standard.value("fontSize").toDouble(), 12.0);
        QCOMPARE(standard.value("fontFamily").toString(), QStringLiteral("Times New Roman"));
        QCOMPARE(charAt(doc(), 9).fontPointSize(), 12.0);
        QCOMPARE(charAt(doc(), 9).fontFamilies().toStringList().value(0), QStringLiteral("Times New Roman"));
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
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
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
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
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
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
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

    void marksPaintAfterRelayout_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::addColumn<bool>("pages");
        for (const auto &engine : {QString("qt"), QString("custom")})
            for (bool pages : {false, true})
                QTest::newRow(qPrintable(engine + (pages ? "-pages" : "-continuous"))) << engine << pages;
    }

    void marksPaintAfterRelayout()
    {
        QFETCH(QString, engine);
        QFETCH(bool, pages);
        PageEditor w;
        QVERIFY(w.root);
        w.root->setProperty("layoutEngine", engine);
        w.root->setProperty("pageView", pages);
        w.view.show();
        const auto path = m_dir.filePath("marks-relayout.odt");
        writeOdt(path, "<text:p text:style-name=\"Center\">LinkedIn</text:p>",
                 "<style:style style:name=\"Center\" style:family=\"paragraph\"><style:paragraph-properties fo:text-align=\"center\"/></style:style>");
        QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(path)));
        w.fmt()->markStyle("spelling", {{"color", "#ff0000"}});
        w.fmt()->addMark("spelling", w.fmt()->blockIds().first(), 0, 8);
        auto paintedBounds = [&] {
            const auto image = w.view.grabWindow();
            QRect bounds;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto c = image.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 100 && c.blue() < 100)
                        bounds |= QRect(x, y, 1, 1);
                }
            return QRectF(bounds.x()/w.view.effectiveDevicePixelRatio(), bounds.y()/w.view.effectiveDevicePixelRatio(),
                          bounds.width()/w.view.effectiveDevicePixelRatio(), bounds.height()/w.view.effectiveDevicePixelRatio());
        };
        auto aligned = [&] {
            const QRectF painted = paintedBounds();
            const QRectF start = caretRect(w.edit(), 0), end = caretRect(w.edit(), 8);
            const QPointF origin = w.edit()->mapToScene(QPointF());
            return !painted.isEmpty() && qAbs(painted.left()-origin.x()-start.left()) < 2
                && qAbs(painted.right()-origin.x()-end.left()) < 2
                && painted.top() >= origin.y()+start.center().y()
                && painted.bottom() <= origin.y()+start.bottom()+1;
        };
        QTRY_VERIFY(aligned());
        w.root->setProperty("pageWidth", 650.);
        w.root->setProperty("pageMargins", QVariantList{40., 60., 40., 80.});
        QTest::qWait(30);
        QTRY_VERIFY(aligned());
        w.root->setProperty("zoom", 1.25);
        QTest::qWait(30);
        QTRY_VERIFY(aligned());
        // Same-size layout: changing alignment must invalidate the cached painting.
        w.fmt()->setAlignment("right");
        QTest::qWait(30);
        QTRY_VERIFY(aligned());
        // Queue a mark while TextEdit still has deferred layout work.
        w.fmt()->clearMarks("spelling");
        w.root->setProperty("pageWidth", 720.);
        w.fmt()->addMark("spelling", w.fmt()->blockIds().first(), 0, 8);
        QTest::qWait(30);
        QTRY_VERIFY(aligned());
        w.root->setProperty("pageHeight", 900.);
        w.root->setWidth(860.);
        QTest::qWait(30);
        QTRY_VERIFY(aligned());
        // Default-font relayout bypasses the handler's edit/mark notifications.
        auto font = qobject_cast<QQuickTextEdit *>(w.edit())->font();
        font.setPixelSize(22);
        qobject_cast<QQuickTextEdit *>(w.edit())->setFont(font);
        QTest::qWait(30);
        QTRY_VERIFY(aligned());
        // Background overlays use the same cache, including its inherited transform.
        w.fmt()->markStyle("spelling", {{"underline", QVariant::fromValue(nullptr)}, {"background", "#ff0000"}});
        auto highlightAligned = [&] {
            const auto painted = paintedBounds();
            const auto start = caretRect(w.edit(), 0), end = caretRect(w.edit(), 8);
            const auto origin = w.edit()->mapToScene(QPointF());
            return !painted.isEmpty() && qAbs(painted.left()-origin.x()-start.left()) < 2
                && qAbs(painted.right()-origin.x()-end.left()) < 2
                && qAbs(painted.top()-origin.y()-start.top()) < 2
                && qAbs(painted.height()-start.height()) < 2;
        };
        QTRY_VERIFY(highlightAligned());
    }

    void pageToContinuousAfterScroll_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::newRow("qt") << QString("qt");
        QTest::newRow("custom") << QString("custom");
    }

    void pageToContinuousAfterScroll()
    {
        QFETCH(QString, engine);
        PageEditor w;
        QVERIFY(w.root);
        w.root->setProperty("layoutEngine", engine);
        w.root->setProperty("pageHeight", 300.);
        w.root->setProperty("pageGap", 40.);
        w.view.show();
        QTest::qWait(30);
        w.fmt()->insertText("first");
        w.fmt()->insertPageBreak();
        w.fmt()->insertText("second");
        w.fmt()->insertPageBreak();
        w.fmt()->insertText("third");
        w.edit()->setProperty("cursorPosition", 0);
        QTest::qWait(30);
        w.body()->setProperty("contentY", 350.);
        QTest::qWait(30);
        w.root->setProperty("pageView", false);
        QTRY_COMPARE(w.fmt()->pageCount(), 1);
        QTRY_VERIFY(std::isfinite(w.body()->property("contentHeight").toReal()));
        QVERIFY(caretRect(w.edit(), 13).top() < 300.);
        QTRY_VERIFY(w.edit()->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker")));
        auto font = qobject_cast<QQuickTextEdit *>(w.edit())->font();
        font.setPixelSize(22);
        qobject_cast<QQuickTextEdit *>(w.edit())->setFont(font);
        QTest::qWait(30);
        const auto markers = w.edit()->findChildren<QQuickItem *>(QStringLiteral("pageBreakMarker"));
        QCOMPARE(markers.size(), 2);
        for (const auto *marker : markers) {
            const int block = w.fmt()->blockIds().indexOf(marker->property("blockId").toString());
            const int position = w.fmt()->document()->textDocument()->findBlockByNumber(block).position();
            QVERIFY(qAbs(marker->y()+marker->height()-caretRect(w.edit(), position).top()) < 1);
        }
        w.root->setProperty("pageView", true);
        QTRY_COMPARE(w.fmt()->pageCount(), 3);
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

    void qtPaginationExperiment()
    {
        m_edit->setProperty("width", 794);
        m_edit->setProperty("wrapMode", 1);
        m_fmt->insertText(QStringLiteral("First\nSecond"));
        QTextCursor c(doc());
        c.setPosition(6);
        QTextBlockFormat bf;
        bf.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
        c.mergeBlockFormat(bf);
        auto frame = doc()->rootFrame()->frameFormat();
        frame.setTopMargin(94); frame.setBottomMargin(94 + 24);
        frame.setLeftMargin(113); frame.setRightMargin(113);
        doc()->rootFrame()->setFrameFormat(frame);
        doc()->setPageSize(QSizeF(794, 1123 + 24));
        QCOMPARE(doc()->pageCount(), 2);
        QVERIFY(qAbs(doc()->documentLayout()->blockBoundingRect(doc()->findBlock(6)).top() - (1147.0 + 94)) < 1);
        QVERIFY(qAbs(caretRect(m_edit.data(), 6).top() - (1147.0 + 94)) < 1);
        m_edit->setProperty("cursorPosition", 6);
        m_fmt->setCursorPosition(6);
        m_fmt->insertText(QStringLiteral("x"));
        QCOMPARE(doc()->pageSize(), QSizeF(794, 1147));
    }

    void removeMark_data()
    {
        QTest::addColumn<QString>("engine");
        QTest::newRow("custom") << QStringLiteral("custom");
        QTest::newRow("qt") << QStringLiteral("qt");
    }

    void removeMark()
    {
        QFETCH(QString, engine);
        PageEditor w;
        QVERIFY(w.root);
        w.root->setProperty("layoutEngine", engine);
        auto *fmt = w.fmt();
        auto *d = fmt->document()->textDocument();
        fmt->insertText(QStringLiteral("I saw teh cat\nAnother word"));
        fmt->insertText(QStringLiteral("!"));
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "undo")); // retain redo as well as undo
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        QCoreApplication::processEvents();
        QCOMPARE(dynamic_cast<rt::RichTextLayout *>(d->documentLayout()) != nullptr, engine == "custom");
        d->setModified(false);
        const QString html = fmt->toHtml();
        const int undo = d->availableUndoSteps(), redo = d->availableRedoSteps();
        QVERIFY(d->isUndoAvailable());
        QVERIFY(d->isRedoAvailable());
        QSignalSpy content(d, &QTextDocument::contentsChanged);
        QSignalSpy modified(d, &QTextDocument::modificationChanged);
        const auto ids = fmt->blockIds();
        // Out-of-order insertion exercises the unsorted mark list too.
        const QString keep = fmt->addMark(QStringLiteral("grammar"), ids[1], 0, 7);
        const QString remove = fmt->addMark(QStringLiteral("spelling"), ids[0], 6, 9);
        QVERIFY(!fmt->removeMark(QStringLiteral("unknown")));
        QCOMPARE(fmt->marks().size(), 2);
        const auto keptMark = fmt->markAt(d->findBlockByNumber(1).position());
        const auto segments = fmt->markSegments();
        QVERIFY(!segments.isEmpty());
        const auto redPixels = [&]() {
            const QImage image = w.view.grabWindow();
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const QColor p = image.pixelColor(x, y);
                    if (p.red() > p.green() + 60 && p.red() > p.blue() + 60) ++count;
                }
            return count;
        };
        QTRY_VERIFY(redPixels() > 6);
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "removeMark", Q_RETURN_ARG(QVariant, result),
                                          Q_ARG(QVariant, remove)));
        QVERIFY(result.toBool());
        QTRY_COMPARE(redPixels(), 0); // the overlay repaints, with either layout
        QCOMPARE(fmt->marks().size(), 1);
        QCOMPARE(fmt->marks()[0].id, keep);
        QCOMPARE(fmt->markAt(d->findBlockByNumber(1).position()), keptMark);
        QVERIFY(fmt->markAt(6).isNull());
        QVERIFY(!fmt->removeMark(remove));
        QVERIFY(fmt->removeMark(keep)); // another kind, last mark
        QVERIFY(!fmt->hasMarks());
        QVERIFY(fmt->markSegments().isEmpty());

        // Caller-supplied ids need not be unique: remove ONE matching mark, not all of them.
        fmt->addMark(QStringLiteral("custom-kind"), ids[1], 0, 2, QStringLiteral("duplicate"));
        fmt->addMark(QStringLiteral("highlight"), ids[0], 0, 1, QStringLiteral("duplicate"));
        QVERIFY(fmt->removeMark(QStringLiteral("duplicate")));
        QCOMPARE(fmt->marks().size(), 1);
        QCOMPARE(fmt->markAt(0).toMap().value("kind").toString(), QStringLiteral("highlight"));
        QVERIFY(fmt->removeMark(QStringLiteral("duplicate")));
        QVERIFY(!fmt->removeMark(QStringLiteral("duplicate")));
        QCoreApplication::processEvents();
        QCOMPARE(fmt->toHtml(), html);
        QCOMPARE(d->availableUndoSteps(), undo);
        QCOMPARE(d->availableRedoSteps(), redo);
        QVERIFY(d->isUndoAvailable());
        QVERIFY(d->isRedoAvailable());
        QVERIFY(!fmt->isModified());
        QCOMPARE(content.count(), 0);
        QCOMPARE(modified.count(), 0);
    }

    void blockIdsInRangeBoundariesAndTables()
    {
        RichTextHandler detached;
        QCOMPARE(detached.blockIdsInRange(0, 10), QStringList());
        QCOMPARE(m_fmt->blockIdsInRange(0, 0), m_fmt->blockIds()); // empty paragraph
        m_fmt->insertText(QString::fromUtf8("a😀b\n\nlast"));
        auto ids = m_fmt->blockIds();
        QCOMPARE(ids.size(), 3);
        QCOMPARE(m_fmt->blockIdsInRange(1, 3), QStringList({ids[0]})); // UTF-16 positions
        QCOMPARE(m_fmt->blockIdsInRange(4, 4), QStringList({ids[0]})); // separator belongs to first
        QCOMPARE(m_fmt->blockIdsInRange(4, 5), QStringList({ids[0], ids[1]}));
        QCOMPARE(m_fmt->blockIdsInRange(5, 5), QStringList({ids[1]})); // empty middle paragraph
        QCOMPARE(m_fmt->blockIdsInRange(5, 6), QStringList({ids[1], ids[2]}));
        const int last = doc()->characterCount() - 1;
        QCOMPARE(m_fmt->blockIdsInRange(last, last), QStringList({ids[2]}));
        QCOMPARE(m_fmt->blockIdsInRange(-100, last + 100), ids);
        QCOMPARE(m_fmt->blockIdsInRange(-100, -1), QStringList());
        QCOMPARE(m_fmt->blockIdsInRange(last + 1, last + 100), QStringList());
        QCOMPARE(m_fmt->blockIdsInRange(6, 5), QStringList());

        QTextCursor c(doc());
        c.movePosition(QTextCursor::End);
        auto *table = c.insertTable(2, 2);
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col)
                table->cellAt(row, col).firstCursorPosition().insertText(QStringLiteral("cell\nsecond"));
        // Independent full traversal includes multiple paragraphs in a cell and frame boundaries.
        for (int from = 0; from < doc()->characterCount(); ++from) {
            const int to = qMin(from + 12, doc()->characterCount() - 1);
            QStringList expected;
            for (auto b = doc()->begin(); b.isValid(); b = b.next())
                if (b.position() <= to && b.position() + b.length() > from)
                    expected.append(m_fmt->blockIdAt(b.position()));
            QCOMPARE(m_fmt->blockIdsInRange(from, to), expected);
        }
        PageEditor w;
        QVERIFY(w.root);
        w.fmt()->insertText(QStringLiteral("one\ntwo"));
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "blockIdsInRange", Q_RETURN_ARG(QVariant, result),
                                          Q_ARG(QVariant, 0), Q_ARG(QVariant, 4)));
        QCOMPARE(result.toStringList(), w.fmt()->blockIds());
    }

    void blockIdsInRangeBookCost()
    {
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (book.isEmpty()) QSKIP("Set RICHTEXT_BOOK to the 300-page book");
        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(book)));
        QVERIFY(doc()->blockCount() > 1000);
        const int position = doc()->lastBlock().position();
        const QStringList expected{m_fmt->blockIdAt(position)};
        constexpr int repeats = 2000;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < repeats; ++i) {
            QStringList found;
            for (auto b = doc()->begin(); b.isValid(); b = b.next())
                if (b.position() <= position && b.position() + b.length() > position)
                    found.append(m_fmt->blockIdAt(b.position()));
            QCOMPARE(found, expected);
        }
        const qint64 walkNs = timer.nsecsElapsed();
        timer.restart();
        for (int i = 0; i < repeats; ++i)
            QCOMPARE(m_fmt->blockIdsInRange(position, position), expected);
        const qint64 indexedNs = timer.nsecsElapsed();
        qInfo("range book: blocks=%d, full walk=%.3f us/query, indexed=%.3f us/query",
              doc()->blockCount(), double(walkNs) / repeats / 1000, double(indexedNs) / repeats / 1000);
        QVERIFY2(indexedNs * 10 < walkNs, "A tiny range must not walk the whole book");
    }

    void documentSelectionProperties()
    {
        PageEditor w;
        QVERIFY(w.root);
        for (const char *name : {"cursorPosition", "selectionStart", "selectionEnd"}) {
            QVERIFY2(w.root->property(name).isValid(), name);
            QCOMPARE(w.root->property(name).toInt(), 0);
        }
        QSignalSpy cursor(w.root.data(), SIGNAL(cursorPositionChanged()));
        QSignalSpy start(w.root.data(), SIGNAL(selectionStartChanged()));
        QSignalSpy end(w.root.data(), SIGNAL(selectionEndChanged()));
        QVERIFY(cursor.isValid() && start.isValid() && end.isValid());
        auto check = [&](int c, int a, int b, int nc, int na, int nb) {
            QCOMPARE(w.root->property("cursorPosition").toInt(), c);
            QCOMPARE(w.root->property("selectionStart").toInt(), a);
            QCOMPARE(w.root->property("selectionEnd").toInt(), b);
            QCOMPARE(cursor.count(), nc);
            QCOMPARE(start.count(), na);
            QCOMPARE(end.count(), nb);
            cursor.clear(); start.clear(); end.clear();
        };
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "insertText", Q_ARG(QVariant, "abc\ndef")));
        check(7, 7, 7, 1, 1, 1);
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "setCursorPosition", Q_ARG(QVariant, 2)));
        check(2, 2, 2, 1, 1, 1);
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "setCursorPosition", Q_ARG(QVariant, 2)));
        check(2, 2, 2, 0, 0, 0);
        QVERIFY(QMetaObject::invokeMethod(w.edit(), "select", Q_ARG(int, 2), Q_ARG(int, 6)));
        check(6, 2, 6, 1, 0, 1);
        QVERIFY(QMetaObject::invokeMethod(w.edit(), "select", Q_ARG(int, 6), Q_ARG(int, 2)));
        check(2, 2, 6, 1, 0, 0); // reversed selection: only the caret moved
        w.fmt()->setAlignment(QStringLiteral("right"));
        QCoreApplication::processEvents();
        check(2, 2, 6, 0, 0, 0); // formatting and relayout do not move the selection
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "setCursorPosition", Q_ARG(QVariant, 7)));
        check(7, 7, 7, 1, 1, 1);
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        w.edit()->forceActiveFocus();
        QTest::keyClick(&w.view, Qt::Key_X);
        check(8, 8, 8, 1, 1, 1);
        w.fmt()->newDocument();
        check(0, 0, 0, 1, 1, 1);
    }

    void documentWheelZoom_data()
    {
        QTest::addColumn<bool>("pages");
        QTest::addColumn<QString>("surface");
        for (bool pages : {false, true})
            for (const auto &surface : {"text", "gap", "table", "margin", "image", "marks"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(pages).arg(surface)))
                    << pages << QString::fromLatin1(surface);
    }
    void documentWheelZoom()
    {
        QFETCH(bool, pages);
        QFETCH(QString, surface);
        PageEditor w;
        QVERIFY(w.root);
        w.root->setProperty("pageView", pages);
        w.root->setProperty("pageHeight", 400);
        w.root->setProperty("pageGap", 40);
        w.root->setProperty("pageMargins", QVariantList{30, 30, 30, 30});
        w.edit()->setProperty("text", "<p>Wheel target</p><table border='1'><tr><td>Table target</td></tr></table>"
                                    + QString("<p>Scrolling content</p>").repeated(100));
        auto *document = w.fmt()->document()->textDocument();
        QImage image(40, 40, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::blue);
        const QString imagePath = m_dir.filePath("wheel.png");
        QVERIFY(image.save(imagePath));
        QTextCursor imageCursor(document);
        imageCursor.movePosition(QTextCursor::EndOfBlock);
        imageCursor.insertImage(QUrl::fromLocalFile(imagePath).toString());
        const int imagePosition = imageCursor.position() - 1;
        QVERIFY(!w.fmt()->addMark("highlight", w.fmt()->blockIdAt(0), 0, 5).isEmpty());
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        QTest::qWait(30);
        w.body()->setProperty("contentY", 0);
        QPointF at;
        if (surface == "text" || surface == "table" || surface == "marks" || surface == "image") {
            const auto *doc = w.fmt()->document()->textDocument();
            const int pos = surface == "image" ? imagePosition
                : doc->toPlainText().indexOf(surface == "table" ? "Table" : "Wheel");
            QVERIFY(pos >= 0);
            at = w.edit()->mapToScene(caretRect(w.edit(), pos).center() + QPointF(10, 0));
        } else if (surface == "gap") {
            at = w.edit()->mapToScene(QPointF(50, 390)); // page gap (top inset is 30)
        } else at = w.body()->mapToScene(QPointF(5, 200));
        QVERIFY(w.body()->contains(w.body()->mapFromScene(at)));
        QSignalSpy zoom(w.root.data(), SIGNAL(zoomRequested(double)));
        QVERIFY(zoom.isValid());
        ulong timestamp = 1000;
        const auto send = [&](QPoint angle, QPoint pixel, Qt::KeyboardModifiers mods,
                              Qt::ScrollPhase phase = Qt::NoScrollPhase) {
            QWheelEvent event(at, w.view.mapToGlobal(at.toPoint()), pixel, angle,
                             Qt::NoButton, mods, phase, false,
                             pixel.isNull() ? Qt::MouseEventNotSynthesized : Qt::MouseEventSynthesizedBySystem);
            event.setTimestamp(timestamp += 20);
            QCoreApplication::sendEvent(&w.view, &event);
        };
        const auto ctrl = Qt::ControlModifier;
        const qreal before = w.body()->property("contentY").toReal();
        send({0, -120}, {}, ctrl);
        QCOMPARE(zoom.size(), 1);
        QCOMPARE(zoom.takeFirst().at(0).toDouble(), -0.1);
        send({0, 240}, {0, 240}, ctrl); // angle wins, never count both
        QCOMPARE(zoom.size(), 2);
        for (const auto &call : zoom) QCOMPARE(call.at(0).toDouble(), 0.1);
        zoom.clear();
        for (int i = 0; i < 3; ++i) send({0, 30}, {}, ctrl);
        QCOMPARE(zoom.size(), 0);
        send({0, 30}, {}, ctrl);
        QCOMPARE(zoom.size(), 1);
        zoom.clear();
        send({}, {}, ctrl, Qt::ScrollBegin);
        for (int i = 0; i < 5; ++i) send({}, {0, -20}, ctrl, Qt::ScrollUpdate);
        QCOMPARE(zoom.size(), 0);
        send({}, {0, -20}, ctrl, Qt::ScrollUpdate);
        send({}, {}, ctrl, Qt::ScrollEnd);
        QCOMPARE(zoom.size(), 1);
        QCOMPARE(zoom.takeFirst().at(0).toDouble(), -0.1);
        send({}, {}, ctrl);
        QCOMPARE(zoom.size(), 0);
        QTest::qWait(30);
        QCOMPARE(w.body()->property("contentY").toReal(), before);
        send({0, -120}, {}, Qt::NoModifier);
        QTRY_VERIFY(w.body()->property("contentY").toReal() > before);
        QMetaObject::invokeMethod(w.body(), "cancelFlick");
        const qreal afterWheel = w.body()->property("contentY").toReal();
        send({}, {}, Qt::NoModifier, Qt::ScrollBegin);
        send({}, {0, -40}, Qt::NoModifier, Qt::ScrollUpdate);
        send({}, {0, -40}, Qt::NoModifier, Qt::ScrollUpdate);
        send({}, {}, Qt::NoModifier, Qt::ScrollEnd);
        // Qt itself suppresses pixel-only updates after an accepted ScrollBegin
        // on the empty margin. Preserve that existing non-Ctrl delivery path.
        if (surface != "margin")
            QTRY_VERIFY(w.body()->property("contentY").toReal() > afterWheel);
        send({0, -120}, {}, Qt::ShiftModifier);
        QCOMPARE(zoom.size(), 0);
        w.root->setVisible(false);
        send({0, -120}, {}, ctrl);
        QCOMPARE(zoom.size(), 0);
        w.root->setVisible(true);
        w.root->setEnabled(false);
        send({0, -120}, {}, ctrl);
        QCOMPARE(zoom.size(), 0);
        w.root->setEnabled(true);
        at = QPointF(-10, -10);
        send({0, -120}, {}, ctrl);
        QCOMPARE(zoom.size(), 0);
    }

    void documentScrollbar_data()
    {
        QTest::addColumn<bool>("pages");
        QTest::newRow("continuous") << false;
        QTest::newRow("pages") << true;
    }
    void documentScrollbar()
    {
        QFETCH(bool, pages);
        PageEditor w;
        QVERIFY(w.root);
        w.root->setProperty("pageView", pages);
        auto *track = w.body()->findChild<QQuickItem *>(QStringLiteral("cssScrollTrack"));
        QVERIFY(track);
        auto *thumb = track->findChild<QQuickItem *>(QStringLiteral("cssScrollThumb"));
        QVERIFY(thumb);
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!book.isEmpty()) QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
        else w.fmt()->insertText(QString("A scrollbar test line.\n").repeated(15000));
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        QCoreApplication::processEvents();
        const int pos = w.fmt()->document()->textDocument()->characterCount() / 2;
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "scrollToPosition", Q_ARG(QVariant, pos)));
        QCoreApplication::processEvents();
        const qreal maxY = w.body()->property("contentHeight").toReal() - w.body()->height();
        const qreal travel = track->height() - thumb->height();
        QVERIFY(travel > 0);
        const qreal fraction = w.body()->property("contentY").toReal() / maxY;
        QVERIFY(qAbs(thumb->y() / travel - fraction) < 0.0001);
        auto point = thumb->mapToScene(QPointF(thumb->width()/2, thumb->height()/2)).toPoint();
        QTest::mousePress(&w.view, Qt::LeftButton, Qt::NoModifier, point);
        const auto target = track->mapToScene(QPointF(track->width()/2, travel * 0.25 + thumb->height()/2)).toPoint();
        QTest::mouseMove(&w.view, target, 20);
        QTest::mouseRelease(&w.view, Qt::LeftButton, Qt::NoModifier, target);
        QVERIFY(qAbs(w.body()->property("contentY").toReal() / maxY - 0.25) < 0.005);
        // A track click pages down; wheel over both text and track works without following caret.
        qreal before = w.body()->property("contentY").toReal();
        QTest::mouseClick(&w.view, Qt::LeftButton, Qt::NoModifier,
                          track->mapToScene(QPointF(6, track->height()-5)).toPoint());
        QVERIFY(w.body()->property("contentY").toReal() > before);
        for (QPointF at : {QPointF(400,300), track->mapToScene(QPointF(6,100))}) {
            before = w.body()->property("contentY").toReal();
            QWheelEvent ev(at, w.view.mapToGlobal(at.toPoint()), QPoint(0, -40), QPoint(0, -120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&w.view, &ev);
            QTRY_VERIFY(w.body()->property("contentY").toReal() > before);
        }
    }

    void keyboardKeepsCaretVisible_data()
    {
        QTest::addColumn<bool>("pages");
        QTest::addColumn<qreal>("zoom");
        for (bool pages : {false, true})
            for (qreal zoom : {0.5, 1.0, 2.0})
                QTest::newRow(qPrintable(QString("%1-%2").arg(pages).arg(zoom))) << pages << zoom;
    }

    void keyboardKeepsCaretVisible()
    {
        QFETCH(bool, pages);
        QFETCH(qreal, zoom);
        PageEditor w(zoom);
        QVERIFY(w.root);
        w.root->setProperty("pageView", pages);
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!book.isEmpty()) QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
        else w.fmt()->insertText(QString("A line of text to navigate.\n").repeated(1000));
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        w.edit()->forceActiveFocus();
        auto visible = [&] {
            const auto r = caretRect(w.edit(), w.edit()->property("cursorPosition").toInt());
            const auto p = w.edit()->mapToItem(w.body(), r.topLeft());
            return p.y() >= -1 && p.y() + r.height() <= w.body()->height() + 1;
        };
        for (auto key : {Qt::Key_End, Qt::Key_Home}) {
            QTest::keyClick(&w.view, key, Qt::ControlModifier);
            QTRY_VERIFY(visible());
        }
        for (auto key : {Qt::Key_Down, Qt::Key_End, Qt::Key_PageDown, Qt::Key_PageUp}) {
            const int before = w.edit()->property("cursorPosition").toInt();
            QTest::keyClick(&w.view, key);
            QTRY_VERIFY(visible());
            if (key != Qt::Key_End) QVERIFY2(w.edit()->property("cursorPosition").toInt() != before, qPrintable(QString::number(key)));
        }
        QTest::keyClick(&w.view, Qt::Key_Down, Qt::ShiftModifier);
        QTRY_VERIFY(visible());
        QVERIFY(w.edit()->property("selectionStart") != w.edit()->property("selectionEnd"));
        QTest::keyClick(&w.view, Qt::Key_PageDown, Qt::ShiftModifier);
        QTRY_VERIFY(visible());
        QVERIFY(w.edit()->property("selectionStart") != w.edit()->property("selectionEnd"));
        // Independent viewport movement must never pull the caret back into view.
        w.body()->setProperty("contentY", 2000);
        QTest::qWait(50);
        QCOMPARE(w.body()->property("contentY").toReal(), 2000.0);
        w.edit()->setProperty("cursorPosition", 0);
        QTRY_VERIFY(visible());
        const int last = w.fmt()->document()->textDocument()->characterCount() - 1;
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "setCursorPosition", Q_ARG(QVariant, last)));
        QTRY_VERIFY(visible());
        QTest::keyClick(&w.view, Qt::Key_A);
        QTRY_VERIFY(visible());
        QGuiApplication::clipboard()->setText(QString("Pasted line.\n").repeated(80));
        w.fmt()->paste();
        QTRY_VERIFY(visible());
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "undo"));
        QTRY_VERIFY(visible());
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "redo"));
        QTRY_VERIFY(visible());
        // Selecting a search hit uses TextEdit.select, the same cursor-change path.
        auto hit = w.fmt()->document()->textDocument()->find(QStringLiteral("Pasted line."));
        QVERIFY(!hit.isNull());
        QVERIFY(QMetaObject::invokeMethod(w.edit(), "select", Q_ARG(int, hit.selectionStart()), Q_ARG(int, hit.selectionEnd())));
        QTRY_VERIFY(visible());
    }

    void loadOdfResetsViewportToPageTop()
    {
        const QString path = m_dir.filePath("viewport.odt");
        writeOdt(path, QByteArray("<text:p>Title</text:p>") + QByteArray("<text:p>Body paragraph</text:p>").repeated(150));
        PageEditor w;
        QVERIFY(w.root);
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        for (int position : {0, 30}) {
            QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(path)));
            w.edit()->setProperty("cursorPosition", position);
            QTest::qWait(30);
            w.body()->setProperty("contentY", 1400);
            QVariant result;
            QVERIFY(QMetaObject::invokeMethod(w.root.data(), "loadOdf", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, path)));
            QVERIFY(result.toMap().value("ok").toBool());
            QCOMPARE(w.edit()->property("cursorPosition").toInt(), 0);
            QTest::qWait(50); // Include queued caret following and layout polish.
            QCOMPARE(w.body()->property("contentY").toReal(), 0.0);
        }
    }

    void pageViewGeometryAndNavigation()
    {
        PageEditor w;
        QVERIFY(w.root);
        QVERIFY(w.root->property("pageView").toBool());
        QCOMPARE(w.root->property("pageCount").toInt(), 1);
        w.fmt()->insertText(QStringLiteral("First"));
        w.fmt()->setCursorPosition(5);
        w.fmt()->insertPageBreak();
        w.fmt()->insertText(QStringLiteral("Second"));
        QTRY_COMPARE(w.root->property("pageCount").toInt(), 2);
        QVERIFY(w.edit()->height() >= w.body()->property("contentHeight").toReal());
        auto *d = w.fmt()->document()->textDocument();
        const int second = d->lastBlock().position();
        QVERIFY(qAbs(caretRect(w.edit(), second).top() - (1147.0 + 94)) < 1);
        w.edit()->setProperty("cursorPosition", second);
        QTRY_COMPARE(w.root->property("currentPage").toInt(), 2);
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "pageAt", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, second)));
        QCOMPARE(result.toInt(), 2);
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "scrollToPage", Q_ARG(QVariant, 2)));
        QTRY_COMPARE(w.body()->property("contentY").toReal(), 1147.0);
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        QTest::qWait(30);
        for (auto *marker : w.edit()->findChildren<QQuickItem *>(QStringLiteral("pageBreakMarker")))
            QVERIFY(!marker->isVisible());
        auto *textEdit = qobject_cast<QQuickTextEdit *>(w.edit());
        QVERIFY(textEdit);
        // The gap is nearer the next page's first line than the preceding short page's text.
        const int gapPosition = textEdit->positionAt(114, 1135);
        QCOMPARE(d->findBlock(gapPosition).position(), second);
        w.root->setProperty("zoom", 1.5);
        QTRY_COMPARE(d->pageSize(), QSizeF(1191, 1720.5));
        QVERIFY(qAbs(caretRect(w.edit(), second).top() - (1720.5 + 141)) < 1);
        w.root->setWidth(1000);
        QCoreApplication::processEvents();
        QCOMPARE(d->pageSize(), QSizeF(1191, 1720.5));
        for (auto *sheet : PageEditor::sheets(w.root.data())) {
            QCOMPARE(sheet->width(), 1191.0);
            QCOMPARE(sheet->height(), 1684.5);
        }
        w.root->setProperty("pageView", false);
        QTRY_VERIFY(d->pageSize().height() <= 0);
        w.body()->setProperty("contentY", 0);
        QTRY_VERIFY(w.edit()->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker")));
        QVERIFY(w.edit()->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker"))->isVisible());
    }

    void pagesStartAtRequestedZoomAndSurviveReload()
    {
        PageEditor w(1.5);
        QVERIFY(w.root);
        QCOMPARE(w.fmt()->zoom(), 1.5);
        w.fmt()->insertText(QStringLiteral("First"));
        w.edit()->setProperty("cursorPosition", 5);
        w.fmt()->insertPageBreak();
        w.fmt()->insertText(QStringLiteral("Second"));
        const QUrl file = QUrl::fromLocalFile(m_dir.filePath("two-pages.odt"));
        QVERIFY(w.fmt()->saveOdfCopy(file));
        for (int i = 0; i < 2; ++i) {
            QVERIFY(w.fmt()->loadOdf(file));
            QTRY_COMPARE(w.root->property("pageCount").toInt(), 2);
            QCOMPARE(w.fmt()->document()->textDocument()->pageSize(), QSizeF(1191, 1720.5));
        }
        w.root->setProperty("pageWidth", 1000);
        QTRY_COMPARE(w.fmt()->document()->textDocument()->pageSize(), QSizeF(1500, 1720.5));
    }

    void missingTextEditSizeSlotFailsSoft()
    {
        QObject futureEditor;
        QTest::ignoreMessage(QtWarningMsg, "RichText: Qt TextEdit updateSize() is unavailable; view size may be stale");
        QVERIFY(!rt::refreshTextEditSize(&futureEditor));
    }

    void pageGeometryIsNotAnEdit()
    {
        PageEditor w;
        QVERIFY(w.root);
        QCoreApplication::processEvents();
        auto *d = w.fmt()->document()->textDocument();
        QVERIFY(!d->isModified());
        QVERIFY(!d->isUndoAvailable());
        w.fmt()->insertText(QStringLiteral("Keep my undo"));
        d->setModified(false);
        const int steps = d->availableUndoSteps();
        const auto before = QUrl::fromLocalFile(m_dir.filePath("page-view.odt"));
        const auto after = QUrl::fromLocalFile(m_dir.filePath("continuous-view.odt"));
        QVERIFY(w.fmt()->saveOdfCopy(before));
        w.root->setProperty("pageGap", 36);
        QCoreApplication::processEvents();
        QCOMPARE(d->availableUndoSteps(), steps);
        QVERIFY(!d->isModified());
        w.root->setProperty("pageView", false);
        QCoreApplication::processEvents();
        QCOMPARE(d->availableUndoSteps(), steps);
        QVERIFY(w.fmt()->saveOdfCopy(after));
        QCOMPARE(zipEntry(before.toLocalFile(), "content.xml"), zipEntry(after.toLocalFile(), "content.xml"));
        d->undo();
        QVERIFY(d->toPlainText().isEmpty());
        QVERIFY(d->isRedoAvailable());
        w.root->setProperty("pageView", true);
        QCoreApplication::processEvents();
        QVERIFY(d->isRedoAvailable());
        d->redo();
        QCOMPARE(d->toPlainText(), QStringLiteral("Keep my undo"));
    }

    void bookPagesKeepLinesOutOfGapsAndRecycleSheets()
    {
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        PageEditor w;
        QVERIFY(w.root);
        QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
        auto *d = w.fmt()->document()->textDocument();
        QVERIFY(d->pageCount() > 100);
        int lines = 0, spanning = 0;
        for (auto b = d->begin(); b.isValid(); b = b.next()) {
            const QRectF rect = d->documentLayout()->blockBoundingRect(b);
            if (int(rect.top() / 1147) != int(rect.bottom() / 1147)) ++spanning;
            for (int i = 0; i < b.layout()->lineCount(); ++i) {
                const auto line = b.layout()->lineAt(i);
                const qreal top = rect.top() + line.y();
                const qreal local = top - qFloor(top / 1147) * 1147;
                QVERIFY2(local >= 93 && local + line.height() <= 1030,
                         qPrintable(QStringLiteral("block %1 line %2: y=%3 h=%4 local=%5").arg(b.blockNumber()).arg(i).arg(top).arg(line.height()).arg(local)));
                ++lines;
            }
        }
        qInfo("PAGE_GEOMETRY lines=%d spanning_blocks=%d pages=%d", lines, spanning, d->pageCount());
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        for (int page : {1, 50, 100, 2}) {
            QVERIFY(QMetaObject::invokeMethod(w.root.data(), "scrollToPage", Q_ARG(QVariant, page)));
            QCoreApplication::processEvents();
            const auto sheets = PageEditor::sheets(w.root.data());
            QVERIFY(!sheets.isEmpty());
            QVERIFY(sheets.size() <= 4);
            for (auto *sheet : sheets) {
                QCOMPARE(sheet->width(), 794.0);
                QCOMPARE(sheet->height(), 1123.0);
                QVERIFY(sheet->y() + sheet->height() >= w.body()->property("contentY").toReal() - 600);
                QVERIFY(sheet->y() <= w.body()->property("contentY").toReal() + 1200);
            }
            // Cursor movement now follows the caret. Edit in the viewport and capture the
            // pool AFTER positioning, so this checks edit-driven recycling, not navigation.
            const int position = qobject_cast<QQuickTextEdit *>(w.edit())->positionAt(
                120, w.body()->property("contentY").toReal() + w.body()->height() / 2);
            w.edit()->setProperty("cursorPosition", position);
            QCoreApplication::processEvents();
            const auto oldSheets = PageEditor::sheets(w.root.data());
            QSignalSpy changed(w.root.data(), SIGNAL(pagesChanged()));
            w.fmt()->insertText(QStringLiteral("x"));
            QCoreApplication::processEvents();
            QCOMPARE(changed.size(), 0);
            for (auto *sheet : oldSheets) QVERIFY(PageEditor::sheets(w.root.data()).contains(sheet));
        }
    }

    void paginatedTablesAndOversizeImages()
    {
        PageEditor w;
        QVERIFY(w.root);
        auto *d = w.fmt()->document()->textDocument();
        QTextCursor c(d);
        // Structural ingress must select Qt before editing a paragraph-engine document.
        w.fmt()->insertTable(100, 2);
        auto *table = qobject_cast<QTextTable *>(d->rootFrame()->childFrames().first());
        QVERIFY(table);
        for (int row = 0; row < 100; ++row)
            table->cellAt(row, 0).firstCursorPosition().insertText(QStringLiteral("A table row"));
        table->cellAt(50, 0).firstCursorPosition().insertText(QStringLiteral("A long cell that crosses page boundaries. ").repeated(400));
        QVERIFY(d->pageCount() >= 3);
        QVERIFY(d->documentLayout()->blockBoundingRect(table->cellAt(50, 0).firstCursorPosition().block()).height() > 1147);
        for (int row = 0; row < 100; ++row) {
            const auto block = table->cellAt(row, 0).firstCursorPosition().block();
            const auto rect = d->documentLayout()->blockBoundingRect(block);
            for (int i = 0; i < block.layout()->lineCount(); ++i) {
                const auto line = block.layout()->lineAt(i);
                const qreal y = rect.top() + line.y();
                const qreal local = y - qFloor(y / 1147) * 1147;
                QVERIFY(local >= 93 && local + line.height() <= 1030);
            }
        }
        c.movePosition(QTextCursor::End);
        c.insertBlock();
        QImage image(20, 1400, QImage::Format_ARGB32); image.fill(Qt::red);
        d->addResource(QTextDocument::ImageResource, QUrl("test:tall"), image);
        QTextImageFormat imageFormat; imageFormat.setName("test:tall");
        imageFormat.setWidth(20); imageFormat.setHeight(900);
        c.insertImage(imageFormat);
        const auto fitting = d->documentLayout()->blockBoundingRect(c.block());
        const auto imageLine = c.block().layout()->lineAt(0);
        const qreal imageTop = fitting.top() + imageLine.y();
        const qreal fittingLocal = imageTop - qFloor(imageTop / 1147) * 1147;
        QVERIFY2(fittingLocal >= 93 && fittingLocal + imageLine.height() <= 1030,
                 qPrintable(QStringLiteral("image top=%1 local=%2 height=%3 block=%4,%5").arg(imageTop).arg(fittingLocal).arg(imageLine.height()).arg(fitting.top()).arg(fitting.height())));
        c.insertBlock();
        imageFormat.setHeight(1400);
        c.insertImage(imageFormat);
        const auto rect = d->documentLayout()->blockBoundingRect(c.block());
        QVERIFY(rect.height() >= 1400);
        qInfo("PAGE_OVERSIZE image y=%.2f height=%.2f: indivisible image overflows page text area", rect.y(), rect.height());
    }

    void paginatedMarksSelectionFindAndOutline()
    {
        PageEditor w;
        QVERIFY(w.root);
        w.fmt()->insertText(QStringLiteral("First"));
        w.edit()->setProperty("cursorPosition", 5);
        w.fmt()->insertPageBreak();
        w.fmt()->insertText(QStringLiteral("Second"));
        auto *d = w.fmt()->document()->textDocument();
        const int pos = d->lastBlock().position();
        w.edit()->setProperty("cursorPosition", pos);
        w.fmt()->setHeading(1);
        QCOMPARE(w.fmt()->headings().size(), 1);
        QCOMPARE(w.fmt()->headings().first().toMap().value("position").toInt(), pos);
        QCOMPARE(d->find(QStringLiteral("Second")).selectionStart(), pos);
        w.fmt()->addMark(QStringLiteral("spelling"), w.fmt()->blockIdAt(pos), 0, 6);
        const auto segments = w.fmt()->markSegments();
        QCOMPARE(segments.size(), 1);
        QVERIFY(segments.first().rect.top() >= 1241);
        QVERIFY(segments.first().rect.bottom() < 1300);
        QTRY_COMPARE(w.root->property("pageCount").toInt(), 2);
        QVERIFY(QMetaObject::invokeMethod(w.root.data(), "scrollToPosition", Q_ARG(QVariant, pos)));
        QCOMPARE(w.body()->property("contentY").toReal(), caretRect(w.edit(), pos).top());
        QVERIFY(QMetaObject::invokeMethod(w.edit(), "select", Q_ARG(int, 0), Q_ARG(int, d->characterCount() - 1)));
        w.fmt()->copy();
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("First\nSecond"));
    }

    void bookBareQtPaginationPerformance()
    {
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        PageEditor reference;
        QVERIFY(reference.root);
        m_edit->setProperty("font", reference.edit()->property("font"));
        m_edit->setProperty("width", 794);
        m_edit->setProperty("wrapMode", 1);
        auto *d = doc();
        delete m_fmt; m_fmt = nullptr; // only QQuickTextEdit and Qt's layout remain
        rt::StyleSheet sheet;
        QString error;
        QVERIFY(rt::readOdt(book, d, &sheet, &error));
        auto f = d->rootFrame()->frameFormat();
        f.setTopMargin(94); f.setBottomMargin(118); f.setLeftMargin(113); f.setRightMargin(113);
        d->rootFrame()->setFrameFormat(f);
        d->setPageSize(QSizeF(794, 1147));
        d->documentLayout()->documentSize();
        QTextCursor cursor(d);
        cursor.setPosition(d->characterCount() / 2);
        QElapsedTimer t; t.start();
        cursor.insertText(QStringLiteral("x"));
        d->documentLayout()->blockBoundingRect(cursor.block());
        qInfo("PAGE_QT_ONLY type_ms=%.3f pages=%d", t.nsecsElapsed() / 1e6, d->pageCount());
    }

    void pageCountChangesAreCoalesced()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
        PageEditor w;
        QVERIFY(w.root);
        QCoreApplication::processEvents();
        auto *d = w.fmt()->document()->textDocument();
        w.fmt()->insertText(QStringLiteral("First"));
        QCoreApplication::processEvents();
        QSignalSpy changed(w.root.data(), SIGNAL(pagesChanged()));
        QTextCursor c(d);
        c.movePosition(QTextCursor::End);
        QTextBlockFormat format;
        format.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
        c.insertBlock(format);
        QCOMPARE(d->pageCount(), 2);
        d->undo();
        QCOMPARE(d->pageCount(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(w.root->property("pageCount").toInt(), 1);
        QCOMPARE(changed.size(), 0);
        d->redo();
        QTRY_COMPARE(w.root->property("pageCount").toInt(), 2);
        QCOMPARE(changed.size(), 1);
    }

    void bookPaginationAfterEdits()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        PageEditor w;
        QVERIFY(w.root);
        QCoreApplication::processEvents();
        QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
        QCoreApplication::processEvents();
        auto *d = w.fmt()->document()->textDocument();
        for (int location = 0; location < 3; ++location) {
            for (const QString &text : {QStringLiteral("x"), QStringLiteral("\n"), QStringLiteral("A pasted paragraph that wraps and changes the flow of text on the page.\n")}) {
                const int pos = location == 0 ? 10 : location == 1 ? d->characterCount()/2 : d->characterCount()-2;
                QTextCursor c(d); c.setPosition(pos); c.insertText(text);
                const int pages = d->pageCount();
                QScopedPointer<QTextDocument> fresh(d->clone());
                // clone() defaults to Qt. Compare incremental layout with a
                // cold layout using the same engine and imported ODF policy.
                if (dynamic_cast<rt::RichTextLayout *>(d->documentLayout()))
                    fresh->setDocumentLayout(new rt::RichTextLayout(fresh.data()));
                QCOMPARE(pages, fresh->pageCount());
                auto expected = fresh->begin();
                for (auto b = d->begin(); b.isValid(); b = b.next(), expected = expected.next()) {
                    const auto rect = d->documentLayout()->blockBoundingRect(b);
                    QCOMPARE(rect, fresh->documentLayout()->blockBoundingRect(expected));
                    for (int i = 0; i < b.layout()->lineCount(); ++i) {
                        const auto line = b.layout()->lineAt(i);
                        const qreal y = rect.top()+line.y();
                        const qreal local = y-qFloor(y/1147)*1147;
                        QVERIFY(local >= 93 && local+line.height() <= 1030);
                    }
                    if (b.blockFormat().pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore)
                        QVERIFY(qAbs(rect.top()-qFloor(rect.top()/1147)*1147-94) < 1);
                }
                QTRY_COMPARE(w.root->property("pageCount").toInt(), d->pageCount());
                d->undo();
            }
        }
    }

    void bookPageEditBudget()
    {
        if (!qEnvironmentVariableIsSet("RICHTEXT_PERF_BUDGET"))
            QSKIP("opt-in release performance gate: RICHTEXT_PERF_BUDGET=1");
        QFile loadFile(QStringLiteral("/proc/loadavg"));
        QVERIFY(loadFile.open(QIODevice::ReadOnly));
        const double load = loadFile.readAll().split(' ').first().toDouble();
        if (load > 6) QSKIP("load average exceeds 6");
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        double medians[2];
        for (int pages = 0; pages < 2; ++pages) {
            PageEditor w;
            QVERIFY(w.root);
            w.root->setProperty("pageView", bool(pages));
            QCoreApplication::processEvents();
            QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
            auto *d = w.fmt()->document()->textDocument();
            d->documentLayout()->documentSize();
            QList<double> samples;
            for (int i = 0; i < 7; ++i) {
                const int pos = d->characterCount()/2;
                w.edit()->setProperty("cursorPosition", pos);
                w.fmt()->setCursorPosition(pos);
                QCoreApplication::processEvents();
                QElapsedTimer timer; timer.start();
                w.fmt()->insertText(QStringLiteral("x"));
                d->documentLayout()->blockBoundingRect(d->findBlock(pos));
                samples.append(timer.nsecsElapsed()/1e6);
                d->undo();
                d->documentLayout()->documentSize();
            }
            std::sort(samples.begin(), samples.end());
            medians[pages] = samples[samples.size()/2];
        }
        loadFile.seek(0);
        const double finalLoad = loadFile.readAll().split(' ').first().toDouble();
        qInfo("EDIT_BUDGET continuous_ms=%.3f pages_ms=%.3f load=%.2f/%.2f", medians[0], medians[1], load, finalLoad);
        if (finalLoad > 6) QSKIP("load average exceeds 6");
        QVERIFY2(medians[1] <= 1.5*medians[0], "page edit exceeds 1.5 times continuous view");
        QVERIFY2(medians[1] < 16, "mid-book page edit exceeds one frame");
    }

    void bookColumnDragBudget()
    {
        if (!qEnvironmentVariableIsSet("RICHTEXT_PERF_BUDGET")) QSKIP("opt-in release gate");
        const auto book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        QFile loadFile("/proc/loadavg");
        QVERIFY(loadFile.open(QIODevice::ReadOnly));
        const double load = loadFile.readAll().split(' ').first().toDouble();
        if (load > 6) QSKIP("load average exceeds 6");
        for (bool pages : {false, true}) {
            PageEditor w;
            QVERIFY(w.root);
            w.root->setProperty("pageView", pages);
            QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
            auto *d = w.fmt()->document()->textDocument();
            w.fmt()->setCursorPosition(d->characterCount()-1);
            w.fmt()->insertTable(1, 2);
            QTextCursor last(d); last.movePosition(QTextCursor::End); last.movePosition(QTextCursor::PreviousCharacter);
            auto *table = last.currentTable();
            QVERIFY(table);
            d->documentLayout()->documentSize();
            const auto before = table->format().columnWidthConstraints();
            QVERIFY(w.fmt()->beginColumnResize(table->firstPosition(), 1));
            QSignalSpy changes(d, &QTextDocument::contentsChanged);
            QElapsedTimer timer; timer.start();
            for (int i=0; i<500; ++i) w.fmt()->resizeColumn(i % 40);
            const double previewMs = timer.nsecsElapsed()/1e6;
            QCOMPARE(changes.size(), 0);
            QCOMPARE(table->format().columnWidthConstraints(), before);
            QList<double> samples;
            for (int i=0; i<7; ++i) {
                const int pos = d->characterCount()/2;
                w.edit()->setProperty("cursorPosition", pos);
                w.fmt()->setCursorPosition(pos);
                QCoreApplication::processEvents();
                timer.restart();
                w.fmt()->insertText("x");
                d->documentLayout()->blockBoundingRect(d->findBlock(pos));
                samples.append(timer.nsecsElapsed()/1e6);
                d->undo();
                d->documentLayout()->documentSize();
                w.fmt()->resizeColumn(i);
            }
            std::sort(samples.begin(), samples.end());
            w.fmt()->endColumnResize(false);
            loadFile.seek(0);
            const double finalLoad = loadFile.readAll().split(' ').first().toDouble();
            qInfo("DRAG_BUDGET pages=%d preview_500_ms=%.3f edit_median_ms=%.3f load=%.2f/%.2f",
                  int(pages), previewMs, samples[3], load, finalLoad);
            if (finalLoad > 6) QSKIP("load average exceeds 6");
            QVERIFY(samples[3] < 4);
        }
    }

    void bookEditPerformance_data()
    {
        QTest::addColumn<bool>("pages");
        QTest::newRow("continuous") << false;
        QTest::newRow("pages") << true;
    }

    void bookEditPerformance()
    {
        QFETCH(bool, pages);
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        PageEditor w;
        QVERIFY(w.root);
        w.root->setProperty("pageView", pages);
        QCoreApplication::processEvents();
        QElapsedTimer timer; timer.start();
        QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
        auto *d = w.fmt()->document()->textDocument();
        d->documentLayout()->documentSize();
        qInfo("EDIT_PERF pages=%d open_ms=%.3f", pages, timer.nsecsElapsed()/1e6);
        // Diagnostic switch: assess Qt's public glyph cache without changing production behaviour.
        if (qEnvironmentVariableIsSet("EDIT_LAYOUT_CACHE")) {
            for (auto b = d->begin(); b.isValid(); b = b.next()) b.layout()->setCacheEnabled(true);
        }
        QFile perfControl(qEnvironmentVariable("EDIT_PERF_CONTROL"));
        if (!perfControl.fileName().isEmpty()) QVERIFY(perfControl.open(QIODevice::WriteOnly));
        QFile perfAck(qEnvironmentVariable("EDIT_PERF_ACK"));
        if (!perfAck.fileName().isEmpty()) QVERIFY(perfAck.open(QIODevice::ReadOnly));
        const int repeats = qEnvironmentVariableIntValue("EDIT_REPEATS") > 0 ? qEnvironmentVariableIntValue("EDIT_REPEATS") : 5;
        const auto measure = [&](int scenario, int boundary = -1) {
            QList<double> samples;
            for (int n = 0; n < repeats; ++n) {
                int pos = scenario == 0 ? 10 : scenario == 2 ? d->characterCount()-2 : d->characterCount()/2;
                if (scenario == 5) pos = boundary;
                w.edit()->setProperty("cursorPosition", pos);
                w.fmt()->setCursorPosition(pos);
                QCoreApplication::processEvents();
                if (perfControl.isOpen()) { perfControl.write("enable\n"); perfControl.flush(); if (perfAck.isOpen()) perfAck.readLine(); }
                timer.restart();
                if (scenario == 5) { QTextCursor c(d); c.setPosition(pos); c.deletePreviousChar(); }
                else w.fmt()->insertText(scenario == 3 ? QStringLiteral("\n") : scenario == 4 ? QStringLiteral("A pasted paragraph with enough words to wrap onto another line and exercise pagination after a document edit.\n") : QStringLiteral("x"));
                d->documentLayout()->blockBoundingRect(d->findBlock(pos));
                samples.append(timer.nsecsElapsed()/1e6);
                if (perfControl.isOpen()) { perfControl.write("disable\n"); perfControl.flush(); if (perfAck.isOpen()) perfAck.readLine(); }
                if (pages && scenario == 5) {
                    QScopedPointer<QTextDocument> fresh(d->clone());
                    if (dynamic_cast<rt::RichTextLayout *>(d->documentLayout()))
                        fresh->setDocumentLayout(new rt::RichTextLayout(fresh.data()));
                    QCOMPARE(d->pageCount(), fresh->pageCount());
                    for (auto b = d->begin(); b.isValid(); b = b.next()) {
                        const qreal top = d->documentLayout()->blockBoundingRect(b).top();
                        for (int i = 0; i < b.layout()->lineCount(); ++i) {
                            const auto line = b.layout()->lineAt(i);
                            const qreal y = top+line.y();
                            const qreal local = y-qFloor(y/1147)*1147;
                            QVERIFY(local >= 93 && local+line.height() <= 1030);
                        }
                    }
                }
                d->undo();
                d->documentLayout()->documentSize();
            }
            std::sort(samples.begin(), samples.end());
            qInfo("EDIT_PERF pages=%d scenario=%d median_ms=%.3f", pages, scenario, samples[samples.size()/2]);
        };
        for (int scenario = 0; scenario < 5; ++scenario) {
            if (!perfControl.isOpen() || scenario == 1) measure(scenario);
        }
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        timer.restart();
        for (int i = 0; i < 100; ++i) {
            w.body()->setProperty("contentY", 1000+i*150);
            QCoreApplication::processEvents();
            w.view.grabWindow();
        }
        QFile smaps(QStringLiteral("/proc/self/smaps_rollup"));
        QVERIFY(smaps.open(QIODevice::ReadOnly));
        const auto pss = QRegularExpression(QStringLiteral("Pss: +([0-9]+)")).match(QString::fromLatin1(smaps.readAll())).captured(1);
        qInfo("EDIT_PERF pages=%d scroll100_ms=%.3f pss_kb=%s", pages, timer.nsecsElapsed()/1e6, qPrintable(pss));
        timer.restart();
        for (int i = 0; i < 100; ++i) {
            w.body()->setProperty("contentY", i*600);
            QCoreApplication::processEvents();
            w.view.grabWindow();
        }
        qInfo("EDIT_PERF pages=%d pagedown100_ms=%.3f", pages, timer.nsecsElapsed()/1e6);
        if (perfControl.isOpen()) return;
        // Compute the same actual page-start character for both views, after PSS
        // measurement so the reference layout does not inflate the memory sample.
        int boundary = -1;
        {
            QScopedPointer<QTextDocument> reference(d->clone());
            auto margins = reference->rootFrame()->frameFormat();
            margins.setTopMargin(94); margins.setBottomMargin(118);
            margins.setLeftMargin(113); margins.setRightMargin(113);
            reference->rootFrame()->setFrameFormat(margins);
            reference->setPageSize(QSizeF(794, 1147));
            reference->documentLayout()->documentSize();
            int previousPage = -1;
            for (auto b = reference->begin(); b.isValid() && boundary < 0; b = b.next()) {
                const qreal top = reference->documentLayout()->blockBoundingRect(b).top();
                for (int i = 0; i < b.layout()->lineCount(); ++i) {
                    const auto line = b.layout()->lineAt(i);
                    const int page = int((top+line.y())/1147);
                    const int position = b.position()+line.textStart();
                    if (position > reference->characterCount()/2 && page > previousPage) {
                        boundary = position; break;
                    }
                    previousPage = page;
                }
            }
        }
        QVERIFY(boundary > 0);
        qInfo("EDIT_PERF boundary_position=%d", boundary);
        measure(5, boundary);
    }

    void bookPagePerformance()
    {
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        PageEditor w;
        QVERIFY(w.root);
        QElapsedTimer t; t.start();
        QVERIFY(w.fmt()->loadOdf(QUrl::fromLocalFile(book)));
        auto *d = w.fmt()->document()->textDocument();
        d->documentLayout()->documentSize();
        const double open = t.nsecsElapsed() / 1e6;
        const int mid = d->characterCount() / 2;
        w.edit()->setProperty("cursorPosition", mid);
        w.fmt()->setCursorPosition(mid);
        t.restart();
        w.fmt()->insertText(QStringLiteral("x"));
        d->documentLayout()->blockBoundingRect(d->findBlock(mid));
        const double typing = t.nsecsElapsed() / 1e6;
        w.view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w.view));
        t.restart();
        for (int i = 0; i < 100; ++i) {
            w.body()->setProperty("contentY", 1000 + i * 150);
            QCoreApplication::processEvents();
            w.view.grabWindow();
        }
        const double scrolling = t.nsecsElapsed() / 1e6;
        QFile smaps(QStringLiteral("/proc/self/smaps_rollup"));
        QVERIFY(smaps.open(QIODevice::ReadOnly));
        const auto memory = smaps.readAll();
        const auto pss = QRegularExpression(QStringLiteral("Pss: +([0-9]+)")).match(QString::fromLatin1(memory)).captured(1);
        qInfo("PAGE_PERF open_ms=%.3f type_ms=%.3f scroll100_ms=%.3f pss_kb=%s pages=%d", open, typing, scrolling, qPrintable(pss), d->pageCount());
    }

    void bookViewportPaintsBottomLines_data()
    {
        QTest::addColumn<bool>("pageView");
        QTest::newRow("continuous") << false;
        QTest::newRow("pages") << true;
    }

    void bookViewportPaintsBottomLines()
    {
        QFETCH(bool, pageView);
        const QString book = qEnvironmentVariable("RICHTEXT_BOOK");
        if (!QFile::exists(book)) QSKIP("book sample unavailable");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QmlCss::CssTheme theme;
        QmlCss::CssLayoutEngine layout(&theme);
        QQuickView view, referenceView;
        const auto create = [&](QQuickView &window) {
            window.engine()->rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
            window.engine()->rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);
            QQmlComponent component(window.engine());
            component.setData("import QtQuick\nimport solidqml.Widgets.RichText\n"
                              "DocumentEditor { width: 794; height: 600; toolbar: false; pageView: false; pageWidth: 794; pageMargins: [94,113,94,113] }", QUrl());
            auto *root = qobject_cast<QQuickItem *>(component.create());
            if (!root) qWarning().noquote() << component.errorString();
            window.resize(794, 600);
            window.setColor(Qt::white);
            if (root) {
                root->setProperty("pageView", pageView);
                root->setParentItem(window.contentItem());
            }
            return root;
        };
        QScopedPointer<QQuickItem> root(create(view)), reference(create(referenceView));
        QVERIFY(root && reference);
        auto *fmt = root->findChild<RichTextHandler *>();
        auto *referenceFmt = reference->findChild<RichTextHandler *>();
        QVERIFY(fmt && referenceFmt);
        auto *edit = root->property("_editor").value<QQuickItem *>();
        auto *body = root->property("_body").value<QQuickItem *>();
        auto *referenceEdit = reference->property("_editor").value<QQuickItem *>();
        auto *referenceBody = reference->property("_body").value<QQuickItem *>();
        QVERIFY(edit && body && referenceEdit && referenceBody);
        QVERIFY(fmt->loadOdf(QUrl::fromLocalFile(book)));
        QVERIFY(referenceFmt->loadOdf(QUrl::fromLocalFile(book)));
        referenceEdit->setFlag(QQuickItem::ItemObservesViewport, false);
        view.show();
        referenceView.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QVERIFY(QTest::qWaitForWindowExposed(&referenceView));
        for (qreal zoom : {1.0, 1.5}) {
            root->setProperty("zoom", zoom);
            reference->setProperty("zoom", zoom);
            referenceEdit->setFlag(QQuickItem::ItemObservesViewport, false);
            QTest::qWait(30);
            const QRectF last = caretRect(edit, fmt->document()->textDocument()->characterCount() - 1);
            QVERIFY2(body->property("contentHeight").toReal() >= edit->y() + last.bottom() + 93,
                     qPrintable(QStringLiteral("contentHeight=%1 editY=%2 lastBottom=%3 editHeight=%4").arg(body->property("contentHeight").toReal()).arg(edit->y()).arg(last.bottom()).arg(edit->height())));
            for (qreal scroll : {0.0, 1800.0, 12000.0, 28000.0, 1800.0, 3500.0}) {
                body->setProperty("contentY", scroll);
                referenceBody->setProperty("contentY", scroll);
                QTest::qWait(50);
                QVERIFY(edit->flags().testFlag(QQuickItem::ItemObservesViewport));
                const QImage culled = view.grabWindow();
                const auto *state = static_cast<QQuickTextEditPrivate *>(QQuickItemPrivate::get(edit));
                QVERIFY2(state->renderedRegion.height() < body->height() * 4,
                         qPrintable(QStringLiteral("culling expanded to %1 px after scroll %2")
                                        .arg(state->renderedRegion.height()).arg(scroll)));
                // Independent full-render oracle; never toggle the tested editor's culling.
                const QImage complete = referenceView.grabWindow();
                QVERIFY(!culled.isNull() && !complete.isNull());
                const qreal dpr = view.effectiveDevicePixelRatio();
                const QRect bottom = QRectF(110 * dpr, 480 * dpr, 575 * dpr, 115 * dpr).toAlignedRect();
                int missing = 0, ink = 0;
                for (int y = bottom.top(); y <= bottom.bottom(); ++y) {
                    for (int x = bottom.left(); x <= bottom.right(); ++x) {
                        if (qGray(complete.pixel(x, y)) < 180) {
                            ++ink;
                            if (qGray(culled.pixel(x, y)) > 230) ++missing;
                        }
                    }
                }
                qInfo("viewport zoom %.1f scroll %.0f: bottom ink %d, missing %d", zoom, scroll, ink, missing);
                if (!pageView) QVERIFY(ink > 100); // a page gap legitimately has no text
                if (missing >= 10) {
                    culled.save(QStringLiteral(".scratch/pages-culled.png"));
                    complete.save(QStringLiteral(".scratch/pages-reference.png"));
                }
                QVERIFY2(missing < 10, qPrintable(QStringLiteral("missing bottom glyph pixels at scroll %1: %2").arg(scroll).arg(missing)));
                // A tiny backward move wholly inside the rendered band only needs the
                // scene transform, not rebuilding the text nodes again.
                const qreal top = scroll - edit->y() - 1;
                if (scroll > 0 && top > state->renderedRegion.top()
                    && top + body->height() < state->renderedRegion.bottom()) {
                    const int nodes = state->renderedBlockCount;
                    body->setProperty("contentY", scroll - 1);
                    QTest::qWait(30);
                    QCOMPARE(state->renderedBlockCount, nodes);
                }
            }
        }
    }

    void pageBreakMarkerFollowsItsBlockWithoutEditing()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QQuickWindow window;
        window.resize(400, 200);
        auto *edit = qobject_cast<QQuickItem *>(m_edit.data());
        edit->setParentItem(window.contentItem());
        edit->setSize(QSizeF(400, 800));
        m_fmt->insertText(QStringLiteral("beforeafter"));
        m_fmt->setCursorPosition(6);
        m_fmt->insertPageBreak();
        const QString id = m_fmt->blockIds().at(1);
        const int position = doc()->findBlockByNumber(1).position();
        const int undo = doc()->availableUndoSteps();
        const bool modified = doc()->isModified();
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(edit->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker")));
        auto *marker = edit->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker"));
        QCOMPARE(marker->property("blockId").toString(), id);
        QCOMPARE(marker->property("label").toString(), QStringLiteral("Quebra de página"));
        QVERIFY(qAbs(marker->y() + marker->height() - caretRect(edit, position).y()) < 1);
        QCOMPARE(marker->width(), edit->width());
        const QString before = m_dir.filePath(QStringLiteral("marker-before.odt"));
        QVERIFY(m_fmt->saveOdfCopy(QUrl(before)));
        QVERIFY(m_fmt->setProperty("pageBreakLabel", QStringLiteral("Page break")));
        QTRY_COMPARE(marker->property("label").toString(), QStringLiteral("Page break"));
        QCOMPARE(doc()->availableUndoSteps(), undo);
        QCOMPARE(doc()->isModified(), modified);
        const QString after = m_dir.filePath(QStringLiteral("marker-after.odt"));
        QVERIFY(m_fmt->saveOdfCopy(QUrl(after)));
        QCOMPARE(zipEntry(before, QStringLiteral("content.xml")), zipEntry(after, QStringLiteral("content.xml")));
        m_fmt->setZoom(1.5);
        QTRY_VERIFY(qAbs(marker->height() - 21) < 1);
        QVERIFY(qAbs(marker->y() + marker->height() - caretRect(edit, position).y()) < 1);
        edit->setY(-200);
        QTRY_VERIFY(!edit->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker")));
        edit->setY(0);
        QTRY_VERIFY(edit->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker")));
        doc()->undo(); // zoom
        doc()->undo(); // page break
        QTRY_VERIFY(!edit->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker")));
        QVERIFY(m_fmt->loadOdf(QUrl(before)));
        QTRY_VERIFY(edit->findChild<QQuickItem *>(QStringLiteral("pageBreakMarker")));
        edit->setParentItem(nullptr);
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
    void replaceRangeKeepsTheIdAndIsOneUndoStep()
    {
        m_fmt->insertText(QStringLiteral("I saw teh cat"));
        const QString a = m_fmt->blockIds().value(0);
        m_fmt->setSelectionStart(6);
        m_fmt->setSelectionEnd(9);
        m_fmt->toggleBold(); // the replaced text's format carries over
        m_fmt->setSelectionEnd(6);
        m_fmt->addMark(QStringLiteral("spelling"), a, 6, 9);
        const int steps = doc()->availableUndoSteps();
        QVERIFY(m_fmt->replaceRange(a, 6, 9, QStringLiteral("the")));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("I saw the cat"));
        QCOMPARE(m_fmt->blockIds(), QStringList { a });
        QVERIFY(m_fmt->marks().isEmpty()); // the edit was inside it
        QVERIFY(charAt(doc(), 7).fontWeight() >= QFont::Bold);
        QVERIFY(doc()->availableUndoSteps() > steps);
        doc()->undo();
        QCOMPARE(m_fmt->plainText(), QStringLiteral("I saw teh cat"));
        QCOMPARE(m_fmt->blockIds(), QStringList { a });
        doc()->redo();
        QCOMPARE(m_fmt->plainText(), QStringLiteral("I saw the cat"));

        // "\n" starts a paragraph; the first part keeps the id
        QVERIFY(m_fmt->replaceRange(a, 0, 1, QStringLiteral("We\nAll")));
        QCOMPARE(m_fmt->plainText(), QStringLiteral("We\nAll saw the cat"));
        QCOMPARE(m_fmt->blockIds().value(0), a);
        // a pure insertion
        QVERIFY(m_fmt->replaceRange(a, 2, 2, QStringLiteral("!")));
        QCOMPARE(m_fmt->blockText(a), QStringLiteral("We!"));

        QVERIFY(!m_fmt->replaceRange(QStringLiteral("nope"), 0, 1, QStringLiteral("x")));
        QVERIFY(!m_fmt->replaceRange(a, 2, 1, QStringLiteral("x")));
        QVERIFY(!m_fmt->replaceRange(a, 0, 99, QStringLiteral("x")));
        QCOMPARE(m_fmt->blockText(a), QStringLiteral("We!"));
    }

    // ── latent crash fix: m_headings/m_blockCounts must never read a dead QTextBlock ────────────
    //
    // handleContentsChange() used to hold a QTextBlock PER heading/block directly; isValid() only
    // checks the handle's pointers are non-null, which stays true for a REMOVED block (Qt frees
    // the node's data but the handle itself doesn't know) — reading blockNumber()/position()/
    // text() on one is a use-after-free. Every path below removes a block some other way than
    // doc->clear() (the one path already guarded): a multi-block selection delete, an HTML paste
    // over a multi-block selection, Enter/Backspace + undo/redo of splits and merges, a table
    // (with a heading inside it) removed whole, and the two bulk-reset entry points themselves
    // exercised back to back. verifyHeadingsAndCountsConsistent() re-derives every value from the
    // LIVE document, so a surviving stale read would have to coincidentally match it to pass.

    void headingsAndCountsSurviveMultiBlockDeleteAcrossThreeBlocks()
    {
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral("\nMiddle\nChapter"));
        m_fmt->setCursorPosition(13); // start of "Chapter"
        m_fmt->setHeading(2);
        QCOMPARE(m_fmt->headings().size(), 2);
        verifyHeadingsAndCountsConsistent();

        // Selects from inside "Intro" through all of "Middle" to inside "Chapter": removes the
        // "Middle" block AND the "Chapter" block's own identity (its text merges into "Intro"'s
        // surviving block), while "Intro" itself (and its heading format) survives.
        m_fmt->setSelectionStart(2);
        m_fmt->setSelectionEnd(17); // through "...Chap|ter": keeps "In" + "ter"
        m_fmt->insertText(QString()); // typing nothing over a selection removes it
        verifyHeadingsAndCountsConsistent();
        QCOMPARE(doc()->blockCount(), 1);
        QCOMPARE(m_fmt->plainText(), QStringLiteral("Inter"));
    }

    void headingsAndCountsSurviveHtmlPasteOverAMultiBlockSelection()
    {
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral("\nMiddle\nChapter"));
        m_fmt->setCursorPosition(13);
        m_fmt->setHeading(2);
        QCOMPARE(m_fmt->headings().size(), 2);
        verifyHeadingsAndCountsConsistent();

        m_fmt->setSelectionStart(2);
        m_fmt->setSelectionEnd(16);
        m_fmt->insertHtml(QStringLiteral("<p>X</p><p>Y</p><p>Z</p>")); // a multi-block paste
        verifyHeadingsAndCountsConsistent();

        // Repeat, this time replacing a span that keeps no heading at all (pure body text).
        m_fmt->setSelectionStart(0);
        m_fmt->setSelectionEnd(doc()->characterCount() - 1);
        m_fmt->insertText(QString());
        m_fmt->insertText(QStringLiteral("a\nb\nc\nd"));
        m_fmt->setSelectionStart(1);
        m_fmt->setSelectionEnd(5);
        m_fmt->insertHtml(QStringLiteral("<p>q</p><p>r</p>"));
        verifyHeadingsAndCountsConsistent();
    }

    void headingsAndCountsSurviveEnterBackspaceUndoRedoCycles()
    {
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertParagraph(); // Enter: splits "Intro" at the end
        m_fmt->setCursorPosition(6); // insertParagraph() does not move the handler's own cursor
        m_fmt->insertText(QStringLiteral("Body"));
        QCOMPARE(doc()->blockCount(), 2);
        verifyHeadingsAndCountsConsistent();

        // Backspace at the start of "Body" merges it back into the heading block.
        QTextCursor c(doc());
        c.setPosition(6);
        c.deletePreviousChar();
        QCOMPARE(doc()->blockCount(), 1);
        verifyHeadingsAndCountsConsistent();

        doc()->undo(); // the split comes back
        QCOMPARE(doc()->blockCount(), 2);
        verifyHeadingsAndCountsConsistent();
        doc()->redo(); // merged again
        QCOMPARE(doc()->blockCount(), 1);
        verifyHeadingsAndCountsConsistent();
        doc()->undo();
        doc()->undo(); // further back: undoes insertParagraph() itself
        verifyHeadingsAndCountsConsistent();
        doc()->redo();
        doc()->redo();
        verifyHeadingsAndCountsConsistent();
    }

    void headingsAndCountsSurviveTableRemovalContainingHeadings()
    {
        m_fmt->insertText(QStringLiteral("Before"));
        m_fmt->setCursorPosition(6);
        m_fmt->insertTable(1, 1);
        QTextTable *table = qobject_cast<QTextTable *>(doc()->rootFrame()->childFrames().value(0));
        QVERIFY(table);
        const int cellPos = table->cellAt(0, 0).firstCursorPosition().position();
        m_fmt->setCursorPosition(cellPos);
        m_fmt->insertText(QStringLiteral("Head"));
        m_fmt->setCursorPosition(cellPos);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(doc()->characterCount() - 1);
        m_fmt->insertText(QStringLiteral("After"));
        QCOMPARE(m_fmt->headings().size(), 1);
        verifyHeadingsAndCountsConsistent();

        // Select from inside "Before" to inside "After": removes the whole table (its ONE cell's
        // block may itself survive the merge, same remove_block() mechanics as the plain-block
        // tests above — not asserted here; what matters is that the table is gone and whatever
        // headings() reports afterward is checked against the live document).
        m_fmt->setSelectionStart(2);
        m_fmt->setSelectionEnd(doc()->characterCount() - 2);
        m_fmt->insertText(QString());
        QCOMPARE(countTables(doc()), 0);
        verifyHeadingsAndCountsConsistent();
    }

    void headingsAndCountsSurviveLoadOdfTwice()
    {
        const QString odtA = m_dir.filePath(QStringLiteral("twice-a.odt"));
        writeOdt(odtA, "<text:h text:outline-level=\"1\">Alpha</text:h><text:p>alpha body</text:p>");
        const QString odtB = m_dir.filePath(QStringLiteral("twice-b.odt"));
        writeOdt(odtB, "<text:h text:outline-level=\"1\">Beta</text:h>"
                        "<text:h text:outline-level=\"2\">Gamma</text:h><text:p>beta body text here</text:p>");

        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(odtA)));
        QCOMPARE(m_fmt->headings().size(), 1);
        verifyHeadingsAndCountsConsistent();

        QVERIFY(m_fmt->loadOdf(QUrl::fromLocalFile(odtB)));
        QCOMPARE(m_fmt->headings().size(), 2);
        verifyHeadingsAndCountsConsistent();
    }

    void headingsAndCountsSurviveNewDocumentWhileHeadingsExist()
    {
        m_fmt->insertText(QStringLiteral("Intro"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        m_fmt->setCursorPosition(5);
        m_fmt->insertText(QStringLiteral("\nChapter"));
        m_fmt->setCursorPosition(6);
        m_fmt->setHeading(2);
        QCOMPARE(m_fmt->headings().size(), 2);

        m_fmt->newDocument();
        QCOMPARE(m_fmt->headings(), QVariantList());
        QCOMPARE(m_fmt->wordCount(), 0);
        QCOMPARE(m_fmt->charCount(), 0);
        QCOMPARE(m_fmt->currentHeadingIndex(), -1);

        // the handler keeps working correctly afterward
        m_fmt->insertText(QStringLiteral("Fresh"));
        m_fmt->setCursorPosition(0);
        m_fmt->setHeading(1);
        QCOMPARE(m_fmt->headings().size(), 1);
        verifyHeadingsAndCountsConsistent();
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    RichTextTest test;
    auto arguments = app.arguments();
    // Meson runs the destructive whole-book search/undo workload separately.
    // Discover the remaining slots so newly added tests are included automatically.
    // Direct invocations retain QTest's usual function/data-row selection.
    if (arguments.removeOne(QStringLiteral("--handler-suite"))) {
        const auto *meta = test.metaObject();
        for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
            const auto method = meta->method(i);
            const auto name = method.name();
            if (method.methodType() == QMetaMethod::Slot && method.parameterCount() == 0
                && method.returnMetaType().id() == QMetaType::Void
                && !name.endsWith("_data") && name != "initTestCase" && name != "cleanupTestCase"
                && name != "init" && name != "cleanup" && name != "editingBookSearchTiming")
                arguments.append(QString::fromLatin1(name));
        }
    }
    return QTest::qExec(&test, arguments);
}
#include "richtext_test.moc"
