#include "shims/richtext_layout.h"
#include "shims/richtext_odf.h"
#include "shims/richtext.h"
#include "shims/richtext_qt_p.h"
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickTextDocument>
#include <private/qquicktextedit_p.h>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QInputMethodEvent>

#include <QElapsedTimer>
#include <QFile>
#include <QTextFragment>
#include <QTest>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextList>
#include <QTextTable>
#include <QTextLayout>
#include <QTextImageFormat>
#include <cmath>
#include <QRandomGenerator>
#include <QPainter>
#include <QImage>

static void prepare(QTextDocument &d) {
    QFont f("DejaVu Sans"); f.setPixelSize(16); d.setDefaultFont(f);
    d.setDocumentMargin(10); d.setPageSize({300, 140});
}
static void compare(QTextDocument &reference, QTextDocument &custom, bool exact=false) {
    const qreal tolerance=exact ? 0 : .5;
    const qreal stride=reference.pageSize().height();
    auto *a = reference.documentLayout(), *b = custom.documentLayout();
    a->documentSize(); b->documentSize();
    QCOMPARE(b->pageCount(), a->pageCount());
    QVERIFY(qAbs(b->documentSize().width()-a->documentSize().width())<=tolerance);
    auto x = reference.begin(), y = custom.begin();
    for (; x.isValid() && y.isValid(); x = x.next(), y = y.next()) {
        const auto ar = a->blockBoundingRect(x), br = b->blockBoundingRect(y);
        QCOMPARE(y.layout()->lineCount(), x.layout()->lineCount());
        QVERIFY2(qAbs(ar.x()-br.x())<=tolerance && qAbs(ar.y()-br.y())<=tolerance, qPrintable(QString("block %1 origin Qt %2,%3 custom %4,%5").arg(x.blockNumber()).arg(ar.x()).arg(ar.y()).arg(br.x()).arg(br.y())));
        QVERIFY2(qAbs(ar.width()-br.width())<=tolerance, qPrintable(QString("block %1 width Qt %2 custom %3").arg(x.blockNumber()).arg(ar.width()).arg(br.width())));
        QVERIFY2(qAbs(ar.height()-br.height())<=tolerance, qPrintable(QString("block %1 height Qt %2 custom %3").arg(x.blockNumber()).arg(ar.height()).arg(br.height())));
        for (int i = 0; i < x.layout()->lineCount(); ++i) {
            const auto al = x.layout()->lineAt(i), bl = y.layout()->lineAt(i);
            const auto ap = ar.topLeft() + al.position(), bp = br.topLeft() + bl.position();
            QCOMPARE(bl.textStart(), al.textStart()); QCOMPARE(bl.textLength(), al.textLength());
            QCOMPARE(int(std::floor(bp.y()/stride)), int(std::floor(ap.y()/stride)));
            QVERIFY2(qAbs(bp.x()-ap.x()) <= tolerance && qAbs(bp.y()-ap.y()) <= tolerance,
                qPrintable(QString("block %1 line %2: Qt %3,%4 custom %5,%6").arg(x.blockNumber()).arg(i).arg(ap.x()).arg(ap.y()).arg(bp.x()).arg(bp.y())));
            QVERIFY2(qAbs(bl.width()-al.width()) <= tolerance, qPrintable(QString("width Qt %1 custom %2").arg(al.width()).arg(bl.width())));
            QVERIFY(qAbs(bl.ascent()-al.ascent()) <= tolerance);
            QVERIFY2(qAbs(bl.naturalTextRect().x()-al.naturalTextRect().x()) <= tolerance,
                qPrintable(QString("block %1 line %2 ink x Qt %3 custom %4").arg(x.blockNumber()).arg(i).arg(al.naturalTextRect().x()).arg(bl.naturalTextRect().x())));
        }
    }
    QCOMPARE(x.isValid(), y.isValid());
}

static bool bookProjection(QTextDocument &d, QString *error) {
    QTextDocument book; rt::StyleSheet styles;
    if (!rt::readOdt(qEnvironmentVariable("RICHTEXT_BOOK"), &book, &styles, error)) return false;
    d.setDefaultFont(book.defaultFont()); d.setDefaultTextOption(book.defaultTextOption());
    d.setIndentWidth(book.indentWidth()); d.setPageSize({794,1150});
    QTextCursor c(&d); c.beginEditBlock();
    QHash<QTextList *, QTextList *> lists;
    for (auto b=book.begin(); b.isValid(); b=b.next()) {
        if (b!=book.begin()) c.insertBlock(b.blockFormat(), b.charFormat());
        else { c.setBlockFormat(b.blockFormat()); c.setBlockCharFormat(b.charFormat()); }
        for (auto it=b.begin(); !it.atEnd(); ++it) { const auto f=it.fragment(); c.insertText(f.text(),f.charFormat()); }
        if (auto *source=b.textList()) {
            if (lists.contains(source)) lists.value(source)->add(c.block());
            else lists.insert(source,c.createList(source->format()));
        }
    }
    auto root=d.rootFrame()->frameFormat(); root.setTopMargin(94); root.setRightMargin(113);
    root.setBottomMargin(126); root.setLeftMargin(113); d.rootFrame()->setFrameFormat(root);
    c.endEditBlock(); d.clearUndoRedoStacks();
    return true;
}

class InlineBox : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    QSizeF intrinsicSize(QTextDocument *,int,const QTextFormat &) override { return {35,27}; }
    void drawObject(QPainter *p,const QRectF &r,QTextDocument *,int,const QTextFormat &) override { p->fillRect(r,Qt::magenta); }
};

struct QuickView {
    QQmlEngine engine;
    QQuickWindow window;
    QQuickTextEdit *edit=nullptr;
    QuickView() {
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { width: 300; height: 1200; textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap; color: 'black'; focus: true }",QUrl());
        edit=qobject_cast<QQuickTextEdit *>(component.create()); Q_ASSERT(edit);
        edit->setParentItem(window.contentItem()); window.resize(320,240); window.setColor(Qt::white); window.show();
        edit->forceActiveFocus(); edit->setCursorVisible(false);
        if (!QTest::qWaitForWindowExposed(&window))
            QTest::qFail("Test window was not exposed", __FILE__, __LINE__);
    }
    ~QuickView() { delete edit; }
    QTextDocument *doc() const { return edit->textDocument()->textDocument(); }
    QImage frame() {
        // Deliver queued layout adapters before the synchronous polish/sync/render grab.
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        return window.grabWindow();
    }
};

class RichTextLayoutTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); }
    void pasteUnsupportedHtmlIntoCustomDocumentFallsBackBeforeEdit() {
        QuickView view; RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        auto *doc=view.doc(); doc->setPlainText("original text");
        handler.configurePages(300,140,0,{10,10,10,10});
        handler.setLayoutEngine("custom");
        QVERIFY(dynamic_cast<rt::RichTextLayout *>(doc->documentLayout()));
        QImage resource(20,20,QImage::Format_ARGB32); resource.fill(Qt::red);
        doc->addResource(QTextDocument::ImageResource,QUrl("image:retained"),resource);
        QTextCursor c(doc); c.movePosition(QTextCursor::End); c.insertText(" edited");
        const auto before=doc->toHtml();
        bool editedOnQt=false;
        connect(doc,&QTextDocument::contentsChange,&handler,[&] {
            editedOnQt=!dynamic_cast<rt::RichTextLayout *>(doc->documentLayout());
        });
        handler.insertHtml("<table><tr><td><table><tr><td>preserved table</td></tr></table></td></tr></table><p><img src='image:retained'></p>");
        QVERIFY(editedOnQt); QVERIFY(!dynamic_cast<rt::RichTextLayout *>(doc->documentLayout()));
        QVERIFY(!doc->rootFrame()->childFrames().isEmpty());
        QCOMPARE(qvariant_cast<QImage>(doc->resource(QTextDocument::ImageResource,QUrl("image:retained"))),resource);
        const auto after=doc->toHtml(); doc->undo(); QCOMPARE(doc->toHtml(),before);
        doc->redo(); QCOMPARE(doc->toHtml(),after);
        handler.setLayoutEngine("qt"); QCOMPARE(handler.layoutEngine(),QString("qt"));
        QVERIFY(!view.frame().isNull());
    }
    void insertSupportedTableAndFallbackBeforeNestedEdit() {
        QuickView view; RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        handler.configurePages(300,140,0,{10,10,10,10}); handler.setLayoutEngine("custom");
        auto *doc=view.doc(); handler.insertTable(2,2);
        QVERIFY(dynamic_cast<rt::RichTextLayout *>(doc->documentLayout()));
        QVERIFY(view.edit->findChild<QQuickItem *>("tableDecoration"));
        auto *table=qobject_cast<QTextTable *>(doc->rootFrame()->childFrames().first()); QVERIFY(table);
        view.edit->setCursorPosition(table->cellAt(0,0).firstPosition());
        handler.setCursorPosition(table->cellAt(0,0).firstPosition());
        bool editedOnQt=false;
        connect(doc,&QTextDocument::contentsChange,&handler,[&] {
            editedOnQt=!dynamic_cast<rt::RichTextLayout *>(doc->documentLayout());
        });
        handler.insertTable(1,1); QVERIFY(editedOnQt);
        doc->undo(); handler.configurePages(300,140,0,{10,10,10,10});
        QVERIFY(!dynamic_cast<rt::RichTextLayout *>(doc->documentLayout()));
    }
    void engineSwitchRetainsObjectHandler() {
        QuickView view; RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        handler.configurePages(300,140,0,{10,10,10,10});
        auto *layout=view.doc()->documentLayout();
        QPointer<InlineBox> box=new InlineBox; box->setParent(layout);
        layout->registerHandler(QTextFormat::UserObject+1,box);
        QTextCharFormat format; format.setObjectType(QTextFormat::UserObject+1);
        QTextCursor(view.doc()).insertText(QString(QChar::ObjectReplacementCharacter),format);
        handler.setLayoutEngine("qt"); QVERIFY(box);
        QCOMPARE(view.doc()->documentLayout()->handlerForObject(QTextFormat::UserObject+1),static_cast<QTextObjectInterface *>(box.data()));
        handler.setLayoutEngine("custom"); QVERIFY(box);
        QVERIFY(view.doc()->begin().layout()->lineAt(0).height()>=27);
    }
    void engineSwitchPreservesIdsAndMarks() {
        QuickView view; RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        handler.insertText("first paragraph\nsecond paragraph");
        const auto ids=handler.blockIds(); QVERIFY(!ids.isEmpty());
        QVERIFY(!handler.addMark("keep",ids.first(),0,3).isEmpty());
        const auto undo=view.doc()->availableUndoSteps();
        QSignalSpy removed(&handler,&RichTextHandler::blockRemoved);
        handler.configurePages(300,140,0,{10,10,10,10});
        QCOMPARE(handler.blockIds(),ids); QVERIFY(!handler.markSegments().isEmpty());
        auto *installed=dynamic_cast<rt::RichTextLayout *>(view.doc()->documentLayout()); QVERIFY(installed);
        QCOMPARE(installed->counters.shapedBlocks,quint64(view.doc()->blockCount()));
        handler.setLayoutEngine("qt"); QCOMPARE(handler.blockIds(),ids);
        handler.setLayoutEngine("custom"); QCOMPARE(handler.blockIds(),ids);
        QCoreApplication::processEvents(); QCOMPARE(removed.count(),0);
        QCOMPARE(view.doc()->availableUndoSteps(),undo); QVERIFY(!handler.markSegments().isEmpty());
    }
    void quickRewire_data() {
        QTest::addColumn<bool>("preinstalled");
        QTest::newRow("preinstalled") << true; QTest::newRow("in-place") << false;
    }
    void quickRewire() {
        QFETCH(bool,preinstalled); QuickView v;
        auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc();
        auto *imageHandler=dynamic_cast<QObject *>(v.doc()->documentLayout()->handlerForObject(QTextFormat::ImageObject));
        QVERIFY(imageHandler); imageHandler->setParent(doc);
        prepare(*doc); doc->setPlainText(QString("paragraph words for wrapping across a page boundary 0123456789\n").repeated(300));
        auto *layout=new rt::RichTextLayout(doc); doc->setDocumentLayout(layout);
        layout->registerHandler(QTextFormat::ImageObject,imageHandler);
        if (preinstalled) v.edit->textDocument()->setTextDocument(doc);
        QVERIFY(rt::attachCustomLayoutToTextEdit(v.edit,doc,preinstalled));
        RichTextHandler handler; handler.setDocument(v.edit->textDocument());
        v.edit->setCursorVisible(false); auto before=v.frame(); QVERIFY(!before.isNull());
        v.edit->setCursorPosition(4); QTest::keyClick(&v.window,'X'); QVERIFY(doc->toPlainText().startsWith("paraXgraph"));
        QVERIFY(before!=v.frame());
        const QString typed=doc->toPlainText(); v.edit->undo(); QVERIFY(doc->toPlainText()!=typed); v.edit->redo(); QCOMPARE(doc->toPlainText(),typed);
        QTextCursor c(doc); c.select(QTextCursor::Document); QTextCharFormat color; color.setForeground(Qt::red); c.mergeCharFormat(color);
        auto colored=v.frame(); QVERIFY(colored!=before);
        v.edit->setCursorPosition(doc->begin().length()-1); QSignalSpy caret(v.edit,SIGNAL(cursorRectangleChanged()));
        v.edit->setWidth(200); doc->setPageSize({200,160});
        auto geometry=v.frame(); QVERIFY(geometry!=colored); QVERIFY(caret.count()>0);
        QCOMPARE(v.edit->contentHeight(),layout->documentSize().height());
        v.edit->select(0,12); QVERIFY(v.frame()!=geometry); v.edit->deselect();
        v.edit->setCursorPosition(3); const auto rect=v.edit->cursorRectangle(); QCOMPARE(layout->hitTest(rect.center(),Qt::FuzzyHit),3);
        QInputMethodEvent preedit(QString::fromUtf8("é漢"),{}); QGuiApplication::sendEvent(v.edit,&preedit);
        QCOMPARE(doc->begin().layout()->preeditAreaText(),QString::fromUtf8("é漢"));
        QInputMethodEvent commit; commit.setCommitString("ok"); QGuiApplication::sendEvent(v.edit,&commit);
        QVERIFY(doc->toPlainText().contains("ok"));
        const auto id=handler.blockIdAt(0); QVERIFY(!handler.addMark("test",id,0,5).isEmpty());
        QVERIFY(handler.markStyle("test",{{"background","#ffff00"}}).isEmpty()); QVERIFY(!handler.markSegments().isEmpty());
        c=QTextCursor(doc); QTextBlockFormat heading; heading.setHeadingLevel(2); c.mergeBlockFormat(heading);
        QCOMPARE(handler.headings().size(),1); QVERIFY(handler.wordCount()>0);
        c.movePosition(QTextCursor::Start); QImage image(24,18,QImage::Format_ARGB32); image.fill(Qt::blue);
        doc->addResource(QTextDocument::ImageResource,QUrl("test:image"),image); c.insertImage("test:image");
        auto withImage=v.frame(); image=QImage(48,36,QImage::Format_ARGB32); image.fill(Qt::green);
        doc->addResource(QTextDocument::ImageResource,QUrl("test:image"),image); doc->markContentsDirty(0,1);
        QVERIFY(v.frame()!=withImage); QVERIFY(doc->begin().layout()->lineAt(0).height()>=36);
        InlineBox box; layout->registerHandler(QTextFormat::UserObject+1,&box);
        QTextCharFormat object; object.setObjectType(QTextFormat::UserObject+1);
        c=QTextCursor(doc); c.insertText(QString(QChar::ObjectReplacementCharacter),object);
        const auto objectFrame=v.frame();
        bool paintedObject=false;
        for(int y=0;y<objectFrame.height();++y) for(int x=0;x<objectFrame.width();++x)
            paintedObject |= objectFrame.pixelColor(x,y)==QColor(Qt::magenta);
        QVERIFY(paintedObject);
        // Geometry-only changes and normal editing must not require property pulses.
        handler.clearMarks("test");
        v.edit->setCursorVisible(false);
        for (qreal zoom : {1.,1.5}) {
            QFont font("DejaVu Sans"); font.setPixelSize(qRound(16*zoom)); doc->setDefaultFont(font);
            v.edit->setWidth(300*zoom); doc->setPageSize({300*zoom,220*zoom});
            v.edit->setHeight(layout->documentSize().height());
            QuickView oracle;
            auto *copy=doc->clone(&oracle.engine);
            auto *oracleImageHandler=dynamic_cast<QObject *>(oracle.doc()->documentLayout()->handlerForObject(QTextFormat::ImageObject));
            oracleImageHandler->setParent(copy);
            auto *oracleLayout=new rt::RichTextLayout(copy); copy->setDocumentLayout(oracleLayout);
            oracleLayout->registerHandler(QTextFormat::ImageObject,oracleImageHandler);
            oracleLayout->registerHandler(QTextFormat::UserObject+1,&box);
            copy->markContentsDirty(0,copy->characterCount());
            oracle.edit->setWidth(300*zoom); oracle.edit->textDocument()->setTextDocument(copy);
            QVERIFY(rt::attachCustomLayoutToTextEdit(oracle.edit,copy));
            oracle.edit->setHeight(oracleLayout->documentSize().height());
            oracle.edit->setFlag(QQuickItem::ItemObservesViewport,false); oracle.edit->setCursorVisible(false); oracle.frame();
            for (int offset : {0,170,850,1600,850,170}) {
                v.edit->setY(-offset); oracle.edit->setY(-offset);
                const auto full=oracle.frame();
                v.edit->setFlag(QQuickItem::ItemObservesViewport,true); handler.refreshViewport();
                v.edit->setCursorVisible(false);
                const auto culled=v.frame();
                if(culled!=full) { full.save(".scratch/6-quick-full.png"); culled.save(".scratch/6-quick-culled.png"); qInfo()<<"CULL mismatch"<<zoom<<offset; }
                QCOMPARE(culled,full);
            }
        }
        layout->counters={};
        for (int i=0;i<100;++i) { layout->documentSize(); layout->pageCount(); layout->blockBoundingRect(doc->lastBlock()); }
        v.frame(); QCOMPARE(layout->counters.callbacks,quint64(0));
        c=QTextCursor(doc); c.setPosition(doc->characterCount()/2); c.insertText("i"); v.frame();
        QVERIFY(layout->counters.shapedBlocks<=2); QVERIFY(layout->counters.convergences>0);
    }
    void paragraphs_data() {
        QTest::addColumn<int>("kind");
        for (int i=0; i<25; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void paragraphs() {
        QFETCH(int, kind);
        QTextDocument qt; prepare(qt); QTextCursor c(&qt);
        if (kind < 9) {
            QTextBlockFormat f; f.setBottomMargin((kind/3-1)*10); c.setBlockFormat(f); c.insertText("one");
            f = {}; f.setTopMargin((kind%3-1)*12); c.insertBlock(f); c.insertText("two");
        } else {
            for (int n=0; n<12; ++n) {
                QTextBlockFormat f;
                f.setTopMargin(18); f.setBottomMargin(12);
                if (kind==10) f.setLineHeight(150, QTextBlockFormat::ProportionalHeight);
                if (kind==11) f.setLineHeight(25, QTextBlockFormat::FixedHeight);
                if (kind==12) f.setLineHeight(25, QTextBlockFormat::MinimumHeight);
                if (kind==13) f.setLineHeight(6, QTextBlockFormat::LineDistanceHeight);
                if (kind==14 && n%3==0) f.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
                if (kind==15 && n%3==0) f.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysAfter);
                if (kind==18 || kind==23) f.setLayoutDirection(Qt::RightToLeft);
                if (kind==19) { QTextOption::Tab tab; tab.position=110; tab.type=QTextOption::RightTab; f.setTabPositions({tab}); }
                if (kind==23) f.setTextIndent(12);
                if (kind==24) f.setNonBreakableLines(true);
                if (kind==20) f.setAlignment(Qt::AlignJustify);
                if (kind==21) { f.setIndent(1); f.setTextIndent(12); }
                if (kind==22) f.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore | QTextFormat::PageBreak_AlwaysAfter);
                if (kind==16) { f.setIndent(1); f.setTextIndent(-12); f.setRightMargin(8); }
                if (n) c.insertBlock(f); else c.setBlockFormat(f);
                if (kind==21 || kind==23) { QTextListFormat list; list.setStyle(QTextListFormat::ListDecimal); list.setIndent(2); c.createList(list); }
                if (kind==18 || kind==23) c.insertText(QString::fromUtf8("العربية 123 שלום abc "));
                if (kind==19) c.insertText("left\tright ");
                if (kind!=17) c.insertText(QString("words for wrapping across the page boundary ").repeated(n%4+1));
            }
        }
        QScopedPointer<QTextDocument> custom(qt.clone());
        custom->setDocumentLayout(new rt::RichTextLayout(custom.data()));
        compare(qt, *custom);
    }
    void numberedListEditsRepaintSuffix() {
        QuickView view; auto *doc=view.doc(); prepare(*doc);
        QTextCursor c(doc); c.beginEditBlock(); c.createList(QTextListFormat::ListDecimal);
        for(int i=0;i<105;++i) { if(i) c.insertBlock(); c.insertText("numbered item"); }
        c.endEditBlock();
        auto *layout=new rt::RichTextLayout(doc); doc->setDocumentLayout(layout);
        QVERIFY(rt::attachCustomLayoutToTextEdit(view.edit,doc,false));
        RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        view.edit->setHeight(layout->documentSize().height()); view.edit->setCursorVisible(false); view.frame();
        const auto ninth=doc->findBlockByNumber(8);
        ninth.textList()->remove(ninth); // Same paragraph count/height, different suffix ordinals.
        QuickView oracle; auto *copy=doc->clone(&oracle.engine); auto *fresh=new rt::RichTextLayout(copy); copy->setDocumentLayout(fresh);
        oracle.edit->textDocument()->setTextDocument(copy); oracle.edit->setHeight(fresh->documentSize().height());
        oracle.edit->setCursorVisible(false); oracle.edit->setFlag(QQuickItem::ItemObservesViewport,false); oracle.frame();
        for(int block : {0,8,98}) {
            const qreal y=layout->blockBoundingRect(doc->findBlockByNumber(block)).y();
            view.edit->setY(-y); oracle.edit->setY(-y);
            handler.refreshViewport();
            QCOMPARE(view.frame(),oracle.frame());
        }
    }
    void listMarkers() {
        QList<QImage> images;
        for (auto style : {QTextListFormat::ListDisc,QTextListFormat::ListCircle,QTextListFormat::ListSquare,QTextListFormat::ListDecimal}) {
            QTextDocument d; prepare(d); QTextCursor c(&d); c.insertText("item"); c.createList(style);
            d.setDocumentLayout(new rt::RichTextLayout(&d));
            QImage image(300,140,QImage::Format_ARGB32); image.fill(Qt::white); QPainter painter(&image); d.documentLayout()->draw(&painter,{}); painter.end();
            for(const auto &previous:images) QVERIFY(image!=previous);
            images.append(image);
        }
    }
    void inlineObjects_data() {
        QTest::addColumn<int>("mode");
        for(int mode=0;mode<11;++mode) QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void inlineObjects() {
        QFETCH(int,mode); QTextDocument qt; prepare(qt);
        InlineBox box;
        qt.documentLayout()->registerHandler(QTextFormat::UserObject+1,&box);
        QImage image(31,43,QImage::Format_ARGB32); image.fill(Qt::green);
        qt.addResource(QTextDocument::ImageResource,QUrl("test:standalone"),image);
        QTextCursor c(&qt); QTextBlockFormat block;
        if(mode && mode<=4) block.setLineHeight(mode==1?150:25,mode);
        c.setBlockFormat(block);
        QTextImageFormat imageFormat; imageFormat.setName("test:standalone");
        if(mode>4) imageFormat.setVerticalAlignment(static_cast<QTextCharFormat::VerticalAlignment>(mode-4));
        c.insertText("before "); c.insertImage(imageFormat); c.insertText(" middle ");
        QTextCharFormat object; object.setObjectType(QTextFormat::UserObject+1);
        if(mode>4) object.setVerticalAlignment(static_cast<QTextCharFormat::VerticalAlignment>(mode-4));
        c.insertText(QString(QChar::ObjectReplacementCharacter),object);
        c.insertText(" after");
        QScopedPointer<QTextDocument> custom(qt.clone()); auto *layout=new rt::RichTextLayout(custom.data()); custom->setDocumentLayout(layout);
        layout->registerHandler(QTextFormat::UserObject+1,&box); custom->markContentsDirty(0,custom->characterCount());
        qt.documentLayout()->documentSize();
        compare(qt,*custom);
        QImage output(300,140,QImage::Format_ARGB32); output.fill(Qt::white); QPainter painter(&output);
        layout->draw(&painter,{}); painter.end();
        int green=0,magenta=0;
        for(int y=0;y<output.height();++y) for(int x=0;x<output.width();++x) {
            green+=output.pixelColor(x,y)==QColor(Qt::green); magenta+=output.pixelColor(x,y)==QColor(Qt::magenta);
        }
        QImage reference(300,140,QImage::Format_ARGB32); reference.fill(Qt::white); QPainter qp(&reference); qt.documentLayout()->draw(&qp,{}); qp.end();
        int qtGreen=0, qtMagenta=0;
        for(int y=0;y<reference.height();++y) for(int x=0;x<reference.width();++x) {
            qtGreen+=reference.pixelColor(x,y)==QColor(Qt::green); qtMagenta+=reference.pixelColor(x,y)==QColor(Qt::magenta);
        }
        QCOMPARE(green,qtGreen); QCOMPARE(magenta,qtMagenta); QVERIFY(green>0); QVERIFY(magenta>0);
        const auto bounds=[](const QImage &image, QColor color) {
            QRect rect;
            for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
                if(image.pixelColor(x,y)==color) rect=rect.united(QRect(x,y,1,1));
            return rect;
        };
        QCOMPARE(bounds(output,Qt::green),bounds(reference,Qt::green));
        QCOMPARE(bounds(output,Qt::magenta),bounds(reference,Qt::magenta));
    }
    void incrementalHitWork() {
        QTextDocument d; prepare(d);
        d.setPlainText(QString("short paragraph\n").repeated(2000));
        QTextCursor title(&d); QTextBlockFormat centered; centered.setAlignment(Qt::AlignHCenter);
        title.setBlockFormat(centered);
        QTextCursor cell(d.findBlockByNumber(10)); auto *table=cell.insertTable(2,2);
        table->cellAt(0,0).firstCursorPosition().insertText("cell text");
        auto *layout=new rt::RichTextLayout(&d); d.setDocumentLayout(layout);
        for (int scenario=0;scenario<4;++scenario) {
            QTextCursor c(&d);
            c.setPosition(scenario==0 ? 0 : scenario==1 ? d.characterCount()/2 : scenario==2 ? table->cellAt(0,0).firstPosition() : 0);
            if (scenario==3) {
                QTextCursor barrier(d.findBlockByNumber(50));
                auto format=barrier.blockFormat(); format.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore); barrier.setBlockFormat(format);
                c.movePosition(QTextCursor::EndOfBlock);
                const int lines=c.block().layout()->lineCount();
                while(c.block().layout()->lineCount()==lines) c.insertText("w");
                c.deletePreviousChar();
                QCOMPARE(c.block().layout()->lineCount(),lines);
            }
            const int previousLines=c.block().layout()->lineCount();
            layout->counters={}; c.insertText("w");
            if(scenario==3) QVERIFY(c.block().layout()->lineCount()>previousLines);
            const auto rect=c.block().layout()->lineForTextPosition(c.position()-c.block().position()).naturalTextRect().translated(c.block().layout()->position());
            QVERIFY(layout->hitTest(rect.center(),Qt::FuzzyHit)>=0);
            qInfo()<<"HIT WORK"<<scenario<<layout->counters.hitEntries<<layout->counters.placements;
            QVERIFY2(layout->counters.hitEntries<100,"A local edit visited the document's entire hit index");
            QCOMPARE(layout->counters.hitRebuilds,quint64(0));
        }
    }
    void seededTableHitIndex() {
        QTextDocument d; prepare(d);
        d.setPlainText(QString("paragraph before and after table\n").repeated(100));
        QTextCursor c(d.findBlockByNumber(5)); auto *table=c.insertTable(3,3);
        for(int row=0;row<3;++row) for(int col=0;col<3;++col)
            table->cellAt(row,col).firstCursorPosition().insertText(QString("cell words ").repeated(10+row+col));
        auto *layout=new rt::RichTextLayout(&d); d.setDocumentLayout(layout);
        QRandomGenerator random(10010);
        for(int step=0;step<40;++step) {
            const auto cell=table->cellAt(random.bounded(3),random.bounded(3));
            QTextCursor edit=cell.firstCursorPosition();
            edit.setPosition(cell.firstPosition()+random.bounded(qMax(1,cell.lastPosition()-cell.firstPosition())));
            if(step%5==0) edit.insertBlock();
            else if(step%5==1) edit.deleteChar();
            else if(step%5==2) d.undo();
            else edit.insertText("www");
            QScopedPointer<QTextDocument> cold(d.clone());
            cold->setDocumentLayout(new rt::RichTextLayout(cold.data()));
            compare(*cold,d,true);
            QCOMPARE(layout->hitIndexSnapshot(),static_cast<rt::RichTextLayout *>(cold->documentLayout())->hitIndexSnapshot());
            for(auto block=d.begin();block.isValid();block=block.next())
                for(int line=0;line<block.layout()->lineCount();++line) {
                    const auto rect=block.layout()->lineAt(line).naturalTextRect().translated(block.layout()->position());
                    for(const auto point:{rect.center(),rect.topLeft()-QPointF(4,2),rect.bottomRight()+QPointF(4,2)})
                        for(auto accuracy:{Qt::FuzzyHit,Qt::ExactHit})
                            QCOMPARE(layout->hitTest(point,accuracy),cold->documentLayout()->hitTest(point,accuracy));
                }
        }
    }
    void hitBookBenchmark() {
        if (qEnvironmentVariableIsEmpty("HIT_BENCHMARK")) QSKIP("Opt-in release benchmark");
        QuickView view; RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        handler.setLayoutEngine("custom");
        QVERIFY(handler.loadOdf(QUrl::fromLocalFile(qEnvironmentVariable("RICHTEXT_BOOK"))));
        handler.configurePages(794,1118,32,{94,113,94,113});
        auto &d=*view.doc();
        int tablePosition=-1;
        for (auto *frame:d.rootFrame()->childFrames()) if (auto *table=qobject_cast<QTextTable *>(frame)) { tablePosition=table->cellAt(0,0).firstPosition(); break; }
        QVERIFY(tablePosition>=0);
        for (int queries=0;queries<2;++queries) for(int scenario=0;scenario<5;++scenario) {
            QFile load("/proc/loadavg"); QVERIFY(load.open(QIODevice::ReadOnly)); const auto average=load.readAll().trimmed();
            QVERIFY2(average.split(' ').first().toDouble()<6,"Invalid benchmark load");
            QList<double> times;
            for(int i=0;i<40;++i) {
                int position=scenario==0 ? 0 : scenario==1 ? 10 : scenario==2 ? d.characterCount()/2 : scenario==3 ? tablePosition : d.characterCount()-1;
                view.edit->setCursorPosition(position); handler.setCursorPosition(position);
                auto *metrics = dynamic_cast<rt::RichTextLayout *>(d.documentLayout());
                if (i == 0 && metrics) metrics->counters = {};
                QElapsedTimer timer; timer.start(); handler.insertText("x");
                if(queries) { view.edit->setCursorPosition(position+1); const auto rect=view.edit->cursorRectangle(); d.documentLayout()->hitTest(rect.center(),Qt::FuzzyHit); }
                times.append(timer.nsecsElapsed()/1e6);
                if (i == 0 && metrics) qInfo() << "HIT_WORK" << queries << scenario << metrics->counters.shapedBlocks
                    << metrics->counters.placements << metrics->counters.convergences << metrics->counters.translations;
                QCOMPARE(d.characterAt(position),QChar('x')); d.undo();
            }
            std::sort(times.begin(),times.end());
            qInfo()<<"HIT_BENCH"<<queries<<scenario<<"median"<<times[20]<<"p95"<<times[38]<<"load"<<average;
        }
    }
    void fractionalSpacingConvergesAfterEdit() {
        QTextDocument d;
        prepare(d);
        d.setDefaultFont(QFont("Liberation Serif", 12));
        auto root = d.rootFrame()->frameFormat();
        root.setProperty(rt::kDocumentSpacingProperty, true);
        d.setPlainText(QString("A paragraph with enough words to wrap over several lines of fractional spacing. ").repeated(6).append('\n').repeated(100));
        d.rootFrame()->setFrameFormat(root);
        auto *layout = new rt::RichTextLayout(&d);
        d.setDocumentLayout(layout);
        d.documentLayout()->documentSize();
        layout->counters = {};
        QTextCursor cursor(&d); cursor.insertText("x");
        QVERIFY(layout->counters.convergences > 0);
        QVERIFY(layout->counters.placements < 10);
        auto cold = d.clone();
        cold->setDocumentLayout(new rt::RichTextLayout(cold));
        compare(*cold, d, true);
        delete cold;
    }
    void incrementalEdits() {
        QuickView view; RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        QTextDocument &d=*view.doc(); prepare(d);
        d.setPlainText(QString("A paragraph with enough text for wrapping to several lines.\n").repeated(80));
        auto *layout = new rt::RichTextLayout(&d); d.setDocumentLayout(layout);
        handler.setDocument(nullptr); handler.setDocument(view.edit->textDocument());
        d.markContentsDirty(0,d.characterCount());
        QVERIFY(rt::attachCustomLayoutToTextEdit(view.edit,&d,false));
        QRandomGenerator random(601);
        for (int step=0; step<100; ++step) {
            QTextCursor c(&d); c.setPosition(random.bounded(d.characterCount()-1));
            switch (step%8) {
            case 0: c.insertText("x"); break;
            case 1: c.insertBlock(); break;
            case 2: c.deletePreviousChar(); break;
            case 3: c.setPosition(qMin(d.characterCount()-1,c.position()+300),QTextCursor::KeepAnchor); c.removeSelectedText(); break;
            case 4: { c.setPosition(qMin(d.characterCount()-1,c.position()+40),QTextCursor::KeepAnchor); QTextCharFormat f; f.setFontPointSize(13); c.mergeCharFormat(f); break; }
            case 5: { QTextListFormat f; f.setIndent(1); f.setStyle(QTextListFormat::ListDecimal); c.createList(f); break; }
            case 6: d.undo(); break;
            case 7: d.redo(); break;
            }
            const auto ids=handler.blockIds();
            QVERIFY(!ids.contains(QString())); QCOMPARE(QSet<QString>(ids.begin(),ids.end()).size(),d.blockCount());
            QScopedPointer<QTextDocument> cold(d.clone());
            cold->setDocumentLayout(new rt::RichTextLayout(cold.data()));
            compare(*cold,d,true);
            QCOMPARE(layout->hitIndexSnapshot(),static_cast<rt::RichTextLayout *>(cold->documentLayout())->hitIndexSnapshot());
            for(auto b=d.begin();b.isValid();b=b.next()) {
                for(int line=0;line<b.layout()->lineCount();++line) {
                    const auto rect=b.layout()->lineAt(line).naturalTextRect().translated(b.layout()->position());
                    for(const auto point:{rect.center(),rect.topLeft()-QPointF(5,3),rect.bottomRight()+QPointF(8,4)})
                        for(auto accuracy:{Qt::FuzzyHit,Qt::ExactHit})
                            QCOMPARE(layout->hitTest(point,accuracy),cold->documentLayout()->hitTest(point,accuracy));
                }
            }
            if (QTest::currentTestFailed()) { qInfo() << "random step" << step; return; }
        }
        layout->counters={}; QTextCursor c(&d); c.setPosition(d.characterCount()/2); c.insertText("i");
        QVERIFY(layout->counters.shapedBlocks <= 2);
        QVERIFY(layout->counters.convergences > 0);
        QVERIFY(layout->counters.placements < 10);
    }
    void translatedConvergence() {
        QTextDocument d; prepare(d); d.setPlainText(QString("one line\n").repeated(100));
        auto *layout=new rt::RichTextLayout(&d); d.setDocumentLayout(layout);
        QTextBlockFormat f; f.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
        QTextCursor barrier(d.findBlockByNumber(45)); barrier.setBlockFormat(f);
        QTextCursor c(d.findBlockByNumber(40)); f.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore | QTextFormat::PageBreak_AlwaysAfter);
        layout->counters={}; c.setBlockFormat(f);
        QScopedPointer<QTextDocument> cold(d.clone()); cold->setDocumentLayout(new rt::RichTextLayout(cold.data())); compare(*cold,d,true);
        QVERIFY(layout->counters.shapedBlocks<=2); QVERIFY(layout->counters.convergences>0);
        QVERIFY(layout->counters.translations>0); QVERIFY(layout->counters.placements<10);
        QCOMPARE(layout->counters.hitRebuilds,quint64(0));
        QVERIFY(layout->counters.hitEntries<20);
        QCOMPARE(layout->hitIndexSnapshot(),static_cast<rt::RichTextLayout *>(cold->documentLayout())->hitIndexSnapshot());
        qInfo()<<"TRANSLATED counters"<<layout->counters.shapedBlocks<<layout->counters.placements<<layout->counters.translations;
        d.undo(); cold.reset(d.clone()); cold->setDocumentLayout(new rt::RichTextLayout(cold.data())); compare(*cold,d,true);
    }
    void painterAndHitTest() {
        QTextDocument qt; prepare(qt); qt.setPlainText("alpha beta    \nsecond paragraph");
        QScopedPointer<QTextDocument> custom(qt.clone()); custom->setDocumentLayout(new rt::RichTextLayout(custom.data()));
        auto render=[](QTextDocument &d) {
            QImage image(300,140,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
            QPainter p(&image); QAbstractTextDocumentLayout::PaintContext ctx;
            QTextCursor selection(&d); selection.setPosition(2); selection.setPosition(17,QTextCursor::KeepAnchor);
            QTextCharFormat format; format.setBackground(Qt::blue); format.setForeground(Qt::white);
            ctx.selections.append({selection,format}); d.documentLayout()->draw(&p,ctx); return image;
        };
        const auto customImage=render(*custom), qtImage=render(qt);
        customImage.save(".scratch/6-painter-custom.png"); qtImage.save(".scratch/6-painter-qt.png");
        QCOMPARE(customImage,qtImage);
        for (auto b=custom->begin(); b.isValid(); b=b.next()) {
            auto *l=b.layout();
            for (int i=0;i<l->lineCount();++i) {
                auto line=l->lineAt(i);
                for (int offset=line.textStart();offset<=line.textStart()+line.textLength();++offset) {
                    const QPointF p=l->position()+QPointF(line.cursorToX(offset),line.y()+line.height()/2);
                    QCOMPARE(custom->documentLayout()->hitTest(p,Qt::FuzzyHit),b.position()+offset);
                }
            }
        }
        QCOMPARE(custom->documentLayout()->hitTest({5,135},Qt::ExactHit),-1);
    }
    void painterClippingAcrossPageGaps() {
        QTextDocument d; prepare(d); d.setPlainText(QString("Selected words spanning several pages. ").repeated(60));
        d.setDocumentLayout(new rt::RichTextLayout(&d));
        const QSize size=d.documentLayout()->documentSize().toSize();
        QImage full(size,QImage::Format_ARGB32), tiled(size,QImage::Format_ARGB32); full.fill(Qt::white); tiled.fill(Qt::white);
        QAbstractTextDocumentLayout::PaintContext context;
        QTextCursor c(&d); c.select(QTextCursor::Document); QTextCharFormat format; format.setBackground(Qt::blue); format.setForeground(Qt::white);
        context.selections.append({c,format});
        { QPainter p(&full); d.documentLayout()->draw(&p,context); }
        { QPainter p(&tiled); for(int y=0;y<size.height();y+=137) { context.clip=QRectF(0,y,size.width(),137); d.documentLayout()->draw(&p,context); } }
        QCOMPARE(tiled,full);
        for(int page=0;page<d.pageCount();++page) for(int y=page*140+130;y<qMin(size.height(),(page+1)*140+10);++y)
            for(int x=0;x<size.width();++x) QCOMPARE(full.pixelColor(x,y),QColor(Qt::white));
    }
    void bidiGraphemeHits() {
        QTextDocument d; prepare(d); d.setPlainText(QString::fromUtf8("العربية 123 שלום abc é 😀 ffi\nsecond line"));
        QScopedPointer<QTextDocument> reference(d.clone()); reference->documentLayout()->documentSize();
        d.setDocumentLayout(new rt::RichTextLayout(&d));
        for(auto b=d.begin();b.isValid();b=b.next()) {
            auto *layout=b.layout();
            for(int offset=0;offset<b.length();++offset) {
                if(!layout->isValidCursorPosition(offset)) continue;
                auto line=layout->lineForTextPosition(offset); QVERIFY(line.isValid());
                const qreal x=line.cursorToX(offset);
                const QPointF p=layout->position()+QPointF(x,line.y()+line.height()/2);
                const int hit=d.documentLayout()->hitTest(p,Qt::FuzzyHit)-b.position();
                QVERIFY(layout->isValidCursorPosition(hit));
                const int qtHit=reference->documentLayout()->hitTest(p,Qt::FuzzyHit)-b.position();
                QVERIFY2(qAbs(line.cursorToX(hit)-x)<=.5 || qAbs(line.cursorToX(hit,QTextLine::Trailing)-x)<=.5 || hit==qtHit,
                    qPrintable(QString("offset %1 hit %2 Qt hit %3 x %4 hit-leading %5 trailing %6").arg(offset).arg(hit).arg(qtHit).arg(x).arg(line.cursorToX(hit)).arg(line.cursorToX(hit,QTextLine::Trailing))));
            }
        }
    }
    void bookParagraphParity() {
        if (qEnvironmentVariableIsEmpty("RICHTEXT_BOOK")) QSKIP("Set RICHTEXT_BOOK");
        QTextDocument qt; QString error; QVERIFY2(bookProjection(qt,&error),qPrintable(error));
        QScopedPointer<QTextDocument> custom(qt.clone()); custom->setDocumentLayout(new rt::RichTextLayout(custom.data()));
        compare(qt,*custom);
        qInfo() << "BOOK projection blocks/chars/pages" << qt.blockCount() << qt.characterCount() << qt.pageCount();
    }
    void bookBenchmark() {
        if (qEnvironmentVariableIsEmpty("LAYOUT_BENCHMARK")) QSKIP("Opt-in release benchmark");
        QuickView view; view.edit->setWidth(794); view.window.resize(900,600);
        RichTextHandler handler; handler.setDocument(view.edit->textDocument());
        QTextDocument &d=*view.doc(); QString error;
        d.setLayoutEnabled(false);
        QVERIFY2(bookProjection(d,&error),qPrintable(error));
        QElapsedTimer opening; opening.start();
        handler.setLayoutEngine(qEnvironmentVariable("LAYOUT_BENCHMARK")=="qt" ? "qt" : "custom");
        handler.configurePages(794,1118,32,{94,113,94,113});
        d.setLayoutEnabled(true); d.documentLayout()->documentSize();
        view.edit->setHeight(d.documentLayout()->documentSize().height());
        view.edit->setCursorVisible(false); view.frame();
        qInfo()<<"OPEN layout and first frame ms"<<opening.nsecsElapsed()/1e6;
        auto *layout=dynamic_cast<rt::RichTextLayout *>(d.documentLayout());
        auto read=[](const char *name) { QFile f(QString::fromLatin1(name)); if (!f.open(QIODevice::ReadOnly)) return QByteArray(); return f.readAll(); };
        qInfo().noquote() << "LOAD" << read("/proc/loadavg").trimmed();
        QVERIFY2(read("/proc/loadavg").split(' ').first().toDouble()<6,"Invalid benchmark load");
        qInfo().noquote() << "PSS" << read("/proc/self/smaps_rollup");
        for (int scenario=0;scenario<6;++scenario) {
            QList<double> times;
            for (int i=0;i<7;++i) {
                QTextCursor c(&d);
                c.setPosition(scenario==0 ? 10 : scenario==2 ? d.characterCount()-2 : d.characterCount()/2);
                if (scenario==5) c.movePosition(QTextCursor::StartOfBlock);
                view.edit->setCursorPosition(c.position());
                if (layout) layout->counters={};
                QElapsedTimer timer; timer.start();
                if (scenario==3) c.insertBlock();
                else if (scenario==4) c.insertText("A pasted paragraph with several words.\nAnother paragraph.");
                else if (scenario==5) c.deletePreviousChar();
                else c.insertText("x");
                d.documentLayout()->documentSize();
                times.append(timer.nsecsElapsed()/1e6);
                if (layout) qInfo() << "COUNTERS" << scenario << layout->counters.shapedBlocks << layout->counters.shapedCharacters << layout->counters.placements << layout->counters.translations << layout->counters.convergences;
                d.undo(); d.documentLayout()->documentSize();
            }
            std::sort(times.begin(),times.end()); qInfo() << "BENCH" << scenario << "median" << times[3] << "p95" << times.last();
        }
        view.edit->setCursorVisible(false);
        const auto render=[&] { QCoreApplication::processEvents(); return view.window.grabWindow(); };
        view.edit->setCursorPosition(d.characterCount()/2);
        view.edit->setY(-view.edit->cursorRectangle().y()+250); handler.refreshViewport();
        const auto before=render();
        QTextCursor c(&d); c.setPosition(d.characterCount()/2);
        QElapsedTimer timer; timer.start(); c.insertText("x"); const auto after=render();
        qInfo()<<"EDIT through rendered frame ms"<<timer.nsecsElapsed()/1e6;
        QVERIFY(before!=after); d.undo();
        timer.restart();
        const qreal start=view.edit->y();
        for(int i=0;i<100;++i) { view.edit->setY(start-i*5); handler.refreshViewport(); render(); }
        qInfo()<<"SCROLL 100 small frames ms"<<timer.nsecsElapsed()/1e6;
        timer.restart();
        for(int i=0;i<10;++i) { view.edit->setY(start-i*600); handler.refreshViewport(); render(); }
        qInfo()<<"SCROLL 10 page-down frames ms"<<timer.nsecsElapsed()/1e6;
        qInfo().noquote() << "LOAD END" << read("/proc/loadavg").trimmed();
        QVERIFY2(read("/proc/loadavg").split(' ').first().toDouble()<6,"Benchmark load rose above gate");
    }
    void unsupportedStructures() {
        QTextDocument d; QVERIFY(rt::RichTextLayout::supports(&d));
        QTextCursor c(&d); QTextTableFormat plain; plain.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        auto *table=c.insertTable(2,2,plain);
        QVERIFY(rt::RichTextLayout::supports(&d));
        auto format=table->format(); format.setHeaderRowCount(1); table->setFormat(format);
        QVERIFY(!rt::RichTextLayout::supports(&d));
        format.setHeaderRowCount(0); table->setFormat(format);
        table->cellAt(0,0).firstCursorPosition().insertTable(1,1);
        QVERIFY(!rt::RichTextLayout::supports(&d));
        QTextDocument hidden; hidden.begin().setVisible(false);
        QVERIFY(!rt::RichTextLayout::supports(&hidden));
        QTextDocument background; auto root=background.rootFrame()->frameFormat(); root.setBackground(Qt::yellow);
        background.rootFrame()->setFrameFormat(root);
        QVERIFY(!rt::RichTextLayout::supports(&background));
    }
};
QTEST_MAIN(RichTextLayoutTests)
#include "richtext_layout_test.moc"
