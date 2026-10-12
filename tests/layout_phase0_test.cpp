// Phase 0 feasibility experiments only. No production layout or attachment changes.
#include "phase0_layout.h"
#include "shims/richtext.h"
#include "shims/richtext_qt_p.h"
#include "shims/richtext_odf.h"
#include <QTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickTextDocument>
#include <QSGRendererInterface>
#include <QTextCursor>
#include <QTextTable>
#include <QSignalSpy>
#include <QQuickPaintedItem>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QStyleHints>
#include <QTextStream>
#include <QInputMethodEvent>
#include <private/qobject_p_p.h>
#include <private/qmetaobject_p.h>

// QObject exposes dumpObjectInfo but hides receiver names for typed connections.
// Read-only test instrumentation; never used for attachment or layout.
static void connections(QObject *sender) {
    auto *data = QObjectPrivate::get(sender)->connections.loadRelaxed();
    if (!data) return;
    for(int i=0;i<data->signalVectorCount();++i) {
        for(auto *c=data->connectionsForSignal(i).first.loadRelaxed();c;c=c->nextConnectionList.loadRelaxed()) {
            auto *receiver=c->receiver.loadRelaxed(); if(!receiver) continue;
            qInfo().noquote() << sender->objectName() << QMetaObjectPrivate::signal(sender->metaObject(),i).methodSignature()
                << "->" << receiver->metaObject()->className() << receiver->objectName()
                << (c->isSlotObject ? QByteArray("[typed slot; target confirmed in consumer source]") : receiver->metaObject()->method(c->method()).methodSignature());
        }
    }
}
static QByteArray loadAverage() { QFile f("/proc/loadavg"); if(!f.open(QIODevice::ReadOnly)) return "unavailable"; return f.readAll().trimmed(); }
static int changedPixels(const QImage &a,const QImage &b) {
    if(a.size()!=b.size()) return -1;
    int count=0; for(int y=0;y<a.height();++y) for(int x=0;x<a.width();++x) count+=a.pixel(x,y)!=b.pixel(x,y);
    return count;
}
#include <private/qquicktextedit_p_p.h> // Observation only: identify the control in connection dumps.
#include <private/qquicktextcontrol_p.h>

struct View {
    QQmlEngine engine;
    QQuickWindow window;
    QQuickTextEdit *edit = nullptr;
    View() {
        QQmlComponent c(&engine);
        c.setData("import QtQuick\nTextEdit { width: 300; height: 1200; textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap; color: 'black'; selectionColor: 'blue'; focus: true }", QUrl());
        edit = qobject_cast<QQuickTextEdit *>(c.create());
        Q_ASSERT(edit);
        edit->setParentItem(window.contentItem());
        window.resize(320, 240); window.setColor(Qt::white); window.show();
        edit->forceActiveFocus();
        QTest::qWait(30);
    }
    ~View() { delete edit; }
    void publicRepaint() {
        const auto color=edit->property("color");
        edit->setProperty("color",QColor("#010101")); edit->setProperty("color",color);
    }
    QImage frame() { QTest::qWait(30); return window.grabWindow(); }
    QTextDocument *doc() { return edit->textDocument()->textDocument(); }
    void inventory(const char *name) {
        qInfo() << "CONNECTION INVENTORY" << name;
        doc()->setObjectName("document"); doc()->documentLayout()->setObjectName("layout");
        edit->setObjectName("edit");
        auto *control = QQuickTextEditPrivate::get(edit)->control;
        control->setObjectName("control");
        connections(doc()); connections(doc()->documentLayout()); connections(control);
        qInfo() << "ImageObject handler" << dynamic_cast<QObject *>(doc()->documentLayout()->handlerForObject(QTextFormat::ImageObject));
    }
};

// Public companion item: the immutable rectangles come from the layout producer.
class TableDecoration : public QQuickPaintedItem {
public:
    QList<QRectF> fragments;
    explicit TableDecoration(QQuickItem *parent) : QQuickPaintedItem(parent) { setAcceptedMouseButtons(Qt::NoButton); }
    void paint(QPainter *p) override { p->setPen(QPen(Qt::darkBlue,2)); p->setBrush(QColor("#cceeff")); for(const auto &r:fragments) p->drawRect(r); }
};

// Public property invalidation experiment. A production QML wrapper would need to
// preserve declarative color/alignment bindings; this fixture owns both properties.
// This is deliberately a broad repaint, not the proposed incremental integration.
class PublicPulseAdapter : public QObject {
public:
    bool queued=false, geometry=false, pulsing=false;
    QQuickItem *edit;
    PublicPulseAdapter(QQuickItem *item,QTextDocument *doc) : QObject(item),edit(item) {
        connect(doc->documentLayout(),&QAbstractTextDocumentLayout::update,this,[this] {
            if(pulsing) return;
            geometry=true;
            if(queued) return;
            queued=true;
            QTimer::singleShot(0,this,[this] {
                queued=false; pulsing=true;
                const QVariant color=edit->property("color");
                edit->setProperty("color",QColor("#010101")); edit->setProperty("color",color);
                if(geometry) {
                    const QVariant alignment=edit->property("verticalAlignment");
                    edit->setProperty("verticalAlignment",alignment.toInt()==Qt::AlignBottom ? Qt::AlignTop : Qt::AlignBottom);
                    edit->setProperty("verticalAlignment",alignment);
                }
                geometry=false; pulsing=false;
            });
        });
        // Ordinary content changes already trigger TextEdit's size/caret update path.
        connect(doc,&QTextDocument::contentsChanged,this,[this] { geometry=false; });
        QObject::connect(doc,SIGNAL(undoAvailable(bool)),edit,SIGNAL(canUndoChanged()));
        QObject::connect(doc,SIGNAL(redoAvailable(bool)),edit,SIGNAL(canRedoChanged()));
    }
};
static void installAdapter(QQuickItem *edit, QTextDocument *doc, bool preinstalled) {
    if (QByteArray(QTest::currentDataTag()).contains("rewire"))
        QVERIFY(rt::attachCustomLayoutToTextEdit(edit, doc, preinstalled));
    else
        new PublicPulseAdapter(edit, doc);
}
static void routeData(bool variants=false) {
    QTest::addColumn<bool>("preinstalled"); QTest::addColumn<bool>("adaptive");
    QTest::newRow("preinstalled") << true << false; QTest::newRow("in-place") << false << false;
    if(variants) {
        QTest::newRow("preinstalled-rewire") << true << true;
        QTest::newRow("in-place-rewire") << false << true;
        QTest::newRow("preinstalled-public-adapter") << true << true;
        QTest::newRow("in-place-public-adapter") << false << true;
    }
}

// Deliberately lacks moveCursorDelegate: a changed Qt slot name must fail soft.
class RenamedSlotEditor : public QObject {
    Q_OBJECT
public:
    int calls = 0;
signals:
    void canUndoChanged();
    void canRedoChanged();
public slots:
    void invalidateBlock(QTextBlock) { ++calls; }
    void renamedCursorDelegate() { ++calls; }
    void updateSize() { ++calls; }
    void q_contentsChange(int, int, int) { ++calls; }
};

class Phase0Tests : public QObject {
    Q_OBJECT
private slots:
    void missingSlotRetainsQt() {
        QTextDocument doc;
        doc.setPlainText("working document");
        auto *qt = doc.documentLayout();
        RenamedSlotEditor editor;
        QTest::ignoreMessage(QtWarningMsg, "RichText: custom layout attachment missing update(QRectF) -> moveCursorDelegate(); retaining Qt");
        QVERIFY(!rt::attachCustomLayoutToTextEdit(&editor, &doc));
        QCOMPARE(doc.documentLayout(), qt);
        QTextCursor c(&doc); c.insertText("new ");
        QVERIFY(qt->documentSize().height() > 0);
        QCOMPARE(doc.toPlainText(), QString("new working document"));
        doc.undo(); QCOMPARE(doc.toPlainText(), QString("working document"));
        doc.redo(); QCOMPARE(doc.toPlainText(), QString("new working document"));
        QCOMPARE(editor.calls, 0);
    }
    void attachmentFirst() {
        View v; v.inventory("fresh Qt");
        auto *doc = new QTextDocument(&v.engine);
        doc->setPlainText("first paragraph\nsecond paragraph");
        auto *layout = new ProbeLayout(doc);
        doc->setDocumentLayout(layout); // BEFORE TextEdit sees the document.
        v.edit->textDocument()->setTextDocument(doc);
        v.inventory("preinstalled custom");
        QVERIFY(layout->blockBoundingRect(doc->begin()).height() > 0);
        QVERIFY(!v.frame().isNull());
    }
    void inPlaceInventory() {
        View v; auto *doc=v.doc();
        auto *handler=dynamic_cast<QObject *>(doc->documentLayout()->handlerForObject(QTextFormat::ImageObject));
        QVERIFY(handler);
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        layout->registerHandler(QTextFormat::ImageObject,handler);
        v.inventory("in-place custom; publicly retained image handler");
        QCOMPARE(dynamic_cast<QObject *>(layout->handlerForObject(QTextFormat::ImageObject)),handler);
    }
    void renderedAttachment_data() { routeData(true); }
    void renderedAttachment() {
        QFETCH(bool,preinstalled); QFETCH(bool,adaptive);
        View v;
        auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc();
        auto *handler=dynamic_cast<QObject *>(v.doc()->documentLayout()->handlerForObject(QTextFormat::ImageObject));
        // Public lifetime transfer must happen BEFORE setTextDocument destroys the old document.
        handler->setParent(doc);
        doc->setPlainText("alpha beta gamma delta\nsecond paragraph\nthird paragraph");
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        layout->registerHandler(QTextFormat::ImageObject,handler);
        if(preinstalled) v.edit->textDocument()->setTextDocument(doc);
        if(adaptive) installAdapter(v.edit,doc,preinstalled);
        v.edit->setCursorVisible(false);
        auto first=v.frame(); QVERIFY(!first.isNull());
        layout->counts={};
        v.edit->setCursorPosition(4); QTest::keyClick(&v.window,'X');
        QCOMPARE(doc->toPlainText(),QString("alphXa beta gamma delta\nsecond paragraph\nthird paragraph"));
        qInfo() << "KEY counters callbacks/blocks/chars/placements/size/rect" << layout->counts.callbacks << layout->counts.blocks << layout->counts.characters << layout->counts.placements << layout->counts.sizes << layout->counts.rects;
        auto typed=v.frame(); QVERIFY(changedPixels(first,typed)>0); qInfo() << "typing changed pixels" << changedPixels(first,typed);
        const QString typedText=doc->toPlainText(); v.edit->undo(); QVERIFY(doc->toPlainText()!=typedText); v.edit->redo(); QCOMPARE(doc->toPlainText(),typedText);
        QTextCursor cursor(doc); cursor.select(QTextCursor::Document);
        QTextCharFormat color; color.setForeground(Qt::red); cursor.mergeCharFormat(color);
        auto colored=v.frame(); QVERIFY(changedPixels(typed,colored)>0); qInfo() << "format-only changed pixels" << changedPixels(typed,colored);
        v.edit->setCursorPosition(doc->begin().length()-1);
        const auto caretBeforeGeometry=v.edit->cursorRectangle();
        QSignalSpy caretNotifications(v.edit,SIGNAL(cursorRectangleChanged()));
        QSignalSpy heightNotifications(v.edit,SIGNAL(contentSizeChanged()));
        const qreal oldContentHeight=v.edit->contentHeight();
        layout->configure(140,120,30);
        auto geometry=v.frame();
        if(!adaptive) { QEXPECT_FAIL("","Raw attachment loses geometry-only node invalidation",Continue); }
        QVERIFY(changedPixels(colored,geometry)>0);
        qInfo() << "geometry changed pixels" << changedPixels(colored,geometry);
        v.edit->update(); v.edit->polish(); auto scheduled=v.frame();
        qInfo() << "public update/polish pixels" << changedPixels(geometry,scheduled);
        v.publicRepaint(); auto pulse=v.frame();
        qInfo() << "public color pulse pixels" << changedPixels(scheduled,pulse);
        layout->counts={};
        QVERIFY(QMetaObject::invokeMethod(v.edit,"updateWholeDocument")); auto reference=v.frame();
        qInfo() << "private rebuild vs public pulse pixels" << changedPixels(pulse,reference);
        QCOMPARE(changedPixels(pulse,reference),0);
        QVERIFY(caretBeforeGeometry!=v.edit->cursorRectangle());
        if(!adaptive) { QEXPECT_FAIL("","Geometry changes lose caret rectangle notifications",Continue); }
        QVERIFY(caretNotifications.count()>0);
        if(!adaptive) { QEXPECT_FAIL("","Color-only pulse does not refresh TextEdit content height",Continue); }
        QCOMPARE(v.edit->contentHeight(),layout->documentSize().height());
        qInfo() << "geometry cursor notifications/content height/expected" << caretNotifications.count() << oldContentHeight << v.edit->contentHeight() << layout->documentSize().height();
        v.edit->select(0,8); auto selected=v.frame();
        QVERIFY(changedPixels(geometry,selected)>0);
        qInfo() << "selection changed pixels" << changedPixels(geometry,selected);
        v.edit->deselect(); v.edit->setCursorPosition(5);
        const auto caret=v.edit->cursorRectangle(); QVERIFY(caret.height()>0);
        QCOMPARE(layout->hitTest(caret.center(),Qt::FuzzyHit),5);
        qInfo() << "caret" << caret << "hit" << layout->hitTest(caret.center(),Qt::FuzzyHit);
        v.edit->setCursorVisible(true); auto on=v.frame();
        QTest::qWait(QGuiApplication::styleHints()->cursorFlashTime()/2+50); auto off=v.frame();
        QVERIFY(changedPixels(on,off)>0);
        qInfo() << "blink changed pixels" << changedPixels(on,off);
        layout->counts={};
        for(int i=0;i<100;++i) { layout->documentSize(); layout->pageCount(); layout->blockBoundingRect(doc->lastBlock()); }
        v.frame();
        qInfo() << "QUERY counters callbacks/blocks/chars/placements/size/rect" << layout->counts.callbacks << layout->counts.blocks << layout->counts.characters << layout->counts.placements << layout->counts.sizes << layout->counts.rects;
        QCOMPARE(layout->counts.blocks,0); QCOMPARE(layout->counts.callbacks,0);
    }
    void images_data() { renderedAttachment_data(); }
    void images() {
        QFETCH(bool,preinstalled); QFETCH(bool,adaptive);
        View v; auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc();
        QImage red(24,18,QImage::Format_ARGB32); red.fill(Qt::red);
        doc->addResource(QTextDocument::ImageResource,QUrl("spike:red"),red);
        QTextCursor c(doc); c.insertImage("spike:red"); c.insertText(" before "); c.insertImage("spike:late");
        auto *handler=dynamic_cast<QObject *>(v.doc()->documentLayout()->handlerForObject(QTextFormat::ImageObject));
        handler->setParent(doc);
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        layout->registerHandler(QTextFormat::ImageObject,handler);
        doc->markContentsDirty(0,doc->characterCount());
        if(preinstalled) v.edit->textDocument()->setTextDocument(doc);
        if(adaptive) installAdapter(v.edit,doc,preinstalled);
        v.edit->setCursorVisible(false);
        auto before=v.frame();
        c.movePosition(QTextCursor::End); c.insertImage("spike:red");
        auto after=v.frame(); qInfo() << "image inserted after pixels" << changedPixels(before,after);
        auto redPixels=[](const QImage &im) { int n=0; for(int y=0;y<im.height();++y) for(int x=0;x<im.width();++x) { auto rgb=im.pixelColor(x,y); n+=rgb.red()>200 && rgb.green()<70 && rgb.blue()<70; } return n; };
        qInfo() << "red image pixels before/after" << redPixels(before) << redPixels(after);
        QVERIFY(redPixels(before)>=24*18);
        // Delayed resource completion, public resource API, explicit metric invalidation.
        QTimer::singleShot(40,doc,[doc] {
            QImage image(48,36,QImage::Format_ARGB32); image.fill(Qt::green);
            doc->addResource(QTextDocument::ImageResource,QUrl("spike:red"),image);
            doc->addResource(QTextDocument::ImageResource,QUrl("spike:late"),image);
            doc->markContentsDirty(0,doc->characterCount());
        });
        QTest::qWait(70); auto delayed=v.frame();
        if(!adaptive) { QEXPECT_FAIL("","Raw attachment loses delayed resource node invalidation",Continue); }
        QVERIFY(changedPixels(after,delayed)>0);
        qInfo() << "delayed image resize pixels" << changedPixels(after,delayed) << "line height" << doc->begin().layout()->lineAt(0).height();
        QVERIFY(doc->begin().layout()->lineAt(0).height()>=36);
        v.publicRepaint(); auto publicRebuilt=v.frame();
        qInfo() << "image public color pulse pixels" << changedPixels(delayed,publicRebuilt);
        if(adaptive) QCOMPARE(changedPixels(delayed,publicRebuilt),0);
        else QVERIFY(changedPixels(delayed,publicRebuilt)>0);
        // Diagnostic only: compare public result with a forced private full node rebuild.
        QVERIFY(QMetaObject::invokeMethod(v.edit,"q_invalidate"));
        auto rebuilt=v.frame(); qInfo() << "image stale pixels after private diagnostic rebuild" << changedPixels(publicRebuilt,rebuilt);
        QCOMPARE(changedPixels(publicRebuilt,rebuilt),0);
        QVERIFY(QObject::connect(layout,SIGNAL(updateBlock(QTextBlock)),v.edit,SLOT(invalidateBlock(QTextBlock))));
        QTimer::singleShot(40,doc,[doc] {
            QImage image(36,24,QImage::Format_ARGB32); image.fill(Qt::blue);
            doc->addResource(QTextDocument::ImageResource,QUrl("spike:red"),image);
            doc->addResource(QTextDocument::ImageResource,QUrl("spike:late"),image);
            doc->markContentsDirty(0,doc->characterCount());
        });
        QTest::qWait(70); auto connected=v.frame();
        QVERIFY(changedPixels(rebuilt,connected)>0);
        qInfo() << "image private connection automatic repaint pixels" << changedPixels(rebuilt,connected);

    }
    void preeditAndModels_data() { renderedAttachment_data(); }
    void preeditAndModels() {
        QFETCH(bool,preinstalled); QFETCH(bool,adaptive);
        View v; auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc(); doc->setPlainText("heading words\nbody words");
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        if(preinstalled) v.edit->textDocument()->setTextDocument(doc);
        if(adaptive) installAdapter(v.edit,doc,preinstalled);
        RichTextHandler handler; handler.setDocument(v.edit->textDocument());
        QCOMPARE(handler.wordCount(),4);
        QTextCursor c(doc); QTextBlockFormat bf; bf.setHeadingLevel(2); c.mergeBlockFormat(bf);
        QCOMPARE(handler.headings().size(),1);
        v.edit->setCursorVisible(false); auto beforeMark=v.frame();
        const QString id=handler.blockIdAt(0);
        QVERIFY(!handler.addMark("spike",id,0,7).isEmpty());
        QVERIFY(handler.markStyle("spike",{{"background","#ffff00"}}).isEmpty());
        auto marks=handler.markSegments(); QVERIFY(!marks.isEmpty());
        auto marked=v.frame(); QVERIFY(changedPixels(beforeMark,marked)>0);
        qInfo() << "mark overlay changed pixels" << changedPixels(beforeMark,marked);
        qInfo() << "mark rect" << marks.first().rect << "block" << layout->blockBoundingRect(doc->begin());
        QVERIFY(layout->blockBoundingRect(doc->begin()).intersects(marks.first().rect));
        c.movePosition(QTextCursor::End); c.insertText(" added"); QCOMPARE(handler.wordCount(),5);
        v.edit->setCursorPosition(2);
        QInputMethodEvent event(QString::fromUtf8("é漢"),{});
        QGuiApplication::sendEvent(v.edit,&event);
        QCOMPARE(doc->begin().layout()->preeditAreaText(),QString::fromUtf8("é漢"));
        QVERIFY(doc->begin().layout()->lineCount()>0);
        layout->configure(200,160,20);
        QCOMPARE(doc->begin().layout()->preeditAreaText(),QString::fromUtf8("é漢"));
        QInputMethodEvent commit; commit.setCommitString("ok"); QGuiApplication::sendEvent(v.edit,&commit);
        QVERIFY(doc->toPlainText().contains("heokading"));
        c=QTextCursor(doc); c.setPosition(2); c.insertBlock(); doc->undo(); doc->redo();
        QVERIFY(handler.blockIds().size()==doc->blockCount());
    }
    void runtimeSwitch() {
        View v; auto *qtDoc=new QTextDocument(&v.engine); qtDoc->setHtml("<p><b>preserved</b> text</p>");
        auto *customDoc=qtDoc->clone(&v.engine);
        customDoc->setDocumentLayout(new ProbeLayout(customDoc));
        const auto html=qtDoc->toHtml(); QCOMPARE(customDoc->toHtml(),html);
        v.edit->textDocument()->setTextDocument(customDoc); QCOMPARE(v.doc(),customDoc);
        v.edit->textDocument()->setTextDocument(qtDoc); QCOMPARE(v.doc(),qtDoc); QCOMPARE(v.doc()->toHtml(),html);
        v.edit->textDocument()->setTextDocument(customDoc); QCOMPARE(v.doc()->toHtml(),html);
        // Phase 1 MUST add: paste unsupported HTML into custom document; choose Qt before
        // applying edit, preserve resources, selection, undo/redo and original structure.
    }
    void tableDecoration() {
        View v; auto *doc=new QTextDocument(&v.engine); QTextCursor c(doc);
        auto *table=c.insertTable(1,2);
        table->cellAt(0,0).firstCursorPosition().insertText(QString("left cell words ").repeated(25));
        table->cellAt(0,1).firstCursorPosition().insertText(QString("right cell words ").repeated(24));
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout); layout->configure(300,100,20);
        v.edit->textDocument()->setTextDocument(doc); v.edit->setCursorVisible(false);
        QVERIFY(layout->tableFragments.size()>=4);
        auto plain=v.frame();
        TableDecoration decoration(v.edit); decoration.setZ(-1); decoration.setWidth(300); decoration.setHeight(1200);
        decoration.fragments=layout->tableFragments; decoration.update();
        auto painted=v.frame();
        QVERIFY(changedPixels(plain,painted)>1000);
        qInfo() << "TABLE fragments" << layout->tableFragments.size() << "rendered changed pixels" << changedPixels(plain,painted);
        if(QDir(QStringLiteral(PHASE0_SOURCE_DIR)+"/.scratch").exists())
            QVERIFY(painted.save(QStringLiteral(PHASE0_SOURCE_DIR)+"/.scratch/5-table.png"));
        QVERIFY(layout->tableFragments[0].bottom()<layout->tableFragments[2].top());
    }
    void viewportFrames_data() { renderedAttachment_data(); }
    void viewportFrames() {
        QFETCH(bool,preinstalled); QFETCH(bool,adaptive);
        View v; auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc();
        doc->setPlainText(QString("paragraph words with enough text to wrap into several lines 0123456789\n").repeated(300));
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        if(preinstalled) v.edit->textDocument()->setTextDocument(doc);
        if(adaptive) installAdapter(v.edit,doc,preinstalled);
        v.edit->setCursorVisible(false); v.edit->setHeight(layout->documentSize().height()); v.window.contentItem()->setClip(true);
        RichTextHandler handler; handler.setDocument(v.edit->textDocument());
        const auto refresh = [&] {
            if (QByteArray(QTest::currentDataTag()).contains("rewire")) handler.refreshViewport();
            else v.publicRepaint();
        };
        v.frame();
        for(qreal zoom : {1.0,1.5}) {
            QFont font=doc->defaultFont(); font.setPointSizeF(12*zoom); doc->setDefaultFont(font);
            layout->configure(300*zoom,200*zoom,20*zoom);
            v.edit->setHeight(layout->documentSize().height());
            View oracle; auto *oracleDoc=doc->clone(&oracle.engine); auto *oracleLayout=new ProbeLayout(oracleDoc);
            oracleLayout->width=layout->width; oracleLayout->sheet=layout->sheet; oracleLayout->gap=layout->gap;
            oracleDoc->setDocumentLayout(oracleLayout); oracle.edit->textDocument()->setTextDocument(oracleDoc);
            oracle.edit->setCursorVisible(false); oracle.edit->setHeight(oracleLayout->documentSize().height());
            oracle.edit->setFlag(QQuickItem::ItemObservesViewport,false); oracle.frame();
            for(int offset : {0,170,850,1600,850,170}) {
                v.edit->setY(-offset);
                oracle.edit->setY(-offset); auto full=oracle.frame();
                v.edit->setFlag(QQuickItem::ItemObservesViewport,true); refresh(); auto culled=v.frame();
                int changed=changedPixels(full,culled);
                qInfo() << "CULL zoom/offset/diff" << zoom << offset << changed;
                if(adaptive) QCOMPARE(changed,0);
                else handler.refreshViewport();
                auto refreshed=v.frame();
                qInfo() << "CULL final diff" << changedPixels(full,refreshed);
                QCOMPARE(changedPixels(full,refreshed),0);
                // Change the middle block without moving the cursor: exercise clean scene nodes.
                QTextCursor c(doc); c.setPosition(doc->characterCount()/2); c.insertText("x");
                refresh(); v.frame(); doc->undo(); refresh();
            }
        }
        layout->counts={}; v.frame();
        QTextCursor c(doc); c.setPosition(doc->characterCount()/2); c.insertText("x"); v.frame();
        qInfo() << "POST-FRAME edit callbacks/shaped/chars/replays" << layout->counts.callbacks << layout->counts.blocks << layout->counts.characters << layout->counts.placements;
        QCOMPARE(layout->counts.blocks,1);
        // Insert a line near a page boundary: the early blocks move nonuniformly, then the
        // suffix returns to its old coordinates. Compare the repaired public frame to a
        // fresh custom layout with identical content/configuration, not merely rectangles.
        c=QTextCursor(doc); c.setPosition(20); c.insertText(QString("wide ").repeated(10));
        refresh(); v.edit->setY(0); if(!adaptive) handler.refreshViewport(); auto incremental=v.frame();
        View fresh; auto *copy=doc->clone(&fresh.engine); auto *freshLayout=new ProbeLayout(copy);
        freshLayout->width=layout->width; freshLayout->sheet=layout->sheet; freshLayout->gap=layout->gap;
        copy->setDocumentLayout(freshLayout); fresh.edit->textDocument()->setTextDocument(copy); fresh.edit->setCursorVisible(false);
        auto rebuilt=fresh.frame();
        qInfo() << "NONUNIFORM incremental/fresh pixels" << changedPixels(incremental,rebuilt);
        QCOMPARE(changedPixels(incremental,rebuilt),0);
    }
    void publicAdapter_data() { routeData(); }
    void publicAdapter() {
        QFETCH(bool,preinstalled);
        View v; auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc();
        doc->setPlainText(QString("text with several words to wrap on each line\n").repeated(100));
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        if(preinstalled) v.edit->textDocument()->setTextDocument(doc);
        new PublicPulseAdapter(v.edit,doc);
        v.edit->setCursorVisible(false); v.frame();
        layout->counts={}; QTextCursor c(doc); c.setPosition(2); c.insertText("x"); auto typed=v.frame();
        qInfo() << "PUBLIC adapter typing callbacks/blocks/chars/replays" << layout->counts.callbacks << layout->counts.blocks << layout->counts.characters << layout->counts.placements;
        QCOMPARE(layout->counts.blocks,1);
        v.edit->setCursorPosition(doc->begin().length()-1);
        QSignalSpy caret(v.edit,SIGNAL(cursorRectangleChanged()));
        layout->counts={}; layout->configure(140,120,30); auto geometry=v.frame();
        qInfo() << "PUBLIC adapter geometry callbacks/blocks/replays" << layout->counts.callbacks << layout->counts.blocks << layout->counts.placements;
        QVERIFY(changedPixels(typed,geometry)>0); QCOMPARE(v.edit->contentHeight(),layout->documentSize().height()); QVERIFY(caret.count()>0);
        const auto html=doc->toHtml(); const auto undo=doc->availableUndoSteps();
        layout->counts={};
        for(int i=0;i<100;++i) { v.edit->contentHeight(); layout->documentSize(); layout->pageCount(); }
        v.frame(); QCOMPARE(layout->counts.blocks,0); QCOMPARE(layout->counts.callbacks,0);
        QCOMPARE(doc->toHtml(),html); QCOMPARE(doc->availableUndoSteps(),undo);
    }
    void diagnosticRewire_data() { routeData(); }
    void diagnosticRewire() {
        QFETCH(bool,preinstalled);
        View v; auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc();
        doc->setPlainText("paragraph words wrap at narrow width\nsecond block");
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        if(preinstalled) v.edit->textDocument()->setTextDocument(doc);
        v.edit->setCursorVisible(false); auto before=v.frame();
        v.edit->setCursorPosition(doc->begin().length()-1);
        QSignalSpy caretNotifications(v.edit,SIGNAL(cursorRectangleChanged()));
        // This is a private-touchpoint candidate, confined to the diagnostic executable.
        // No production connection is installed by phase 0.
        QVERIFY(QObject::connect(layout,SIGNAL(updateBlock(QTextBlock)),v.edit,SLOT(invalidateBlock(QTextBlock))));
        QVERIFY(QObject::connect(layout,SIGNAL(update(QRectF)),v.edit,SLOT(moveCursorDelegate())));
        if(preinstalled) {
            QVERIFY(QObject::connect(doc,SIGNAL(contentsChange(int,int,int)),v.edit,SLOT(q_contentsChange(int,int,int))));
            QVERIFY(QObject::connect(doc,SIGNAL(undoAvailable(bool)),v.edit,SIGNAL(canUndoChanged())));
            QVERIFY(QObject::connect(doc,SIGNAL(redoAvailable(bool)),v.edit,SIGNAL(canRedoChanged())));
        }
        layout->configure(140,120,30);
        QVERIFY(rt::refreshTextEditSize(v.edit)); // Existing production boundary, not a new one.
        auto after=v.frame(); QVERIFY(changedPixels(before,after)>0);
        QCOMPARE(v.edit->contentHeight(),layout->documentSize().height());
        QVERIFY(caretNotifications.count()>0);
        qInfo() << "PRIVATE candidate geometry pixels/contentHeight" << changedPixels(before,after) << v.edit->contentHeight();
        layout->counts={}; QTextCursor c(doc); c.setPosition(2); c.insertText("x"); v.frame();
        QCOMPARE(layout->counts.blocks,1);
        qInfo() << "PRIVATE candidate callbacks/blocks/chars/replays" << layout->counts.callbacks << layout->counts.blocks << layout->counts.characters << layout->counts.placements;
    }
    void nonuniformNodes_data() { renderedAttachment_data(); }
    void nonuniformNodes() {
        QFETCH(bool,preinstalled); QFETCH(bool,adaptive);
        View v; auto *doc=preinstalled ? new QTextDocument(&v.engine) : v.doc();
        QTextCursor c(doc);
        for(int px : {100,42,86,86}) { if(c.position()) c.insertBlock(); QTextCharFormat f; QFont font("DejaVu Sans"); font.setPixelSize(px); f.setFont(font); c.insertText("X",f); }
        auto *layout=new ProbeLayout(doc); doc->setDocumentLayout(layout);
        if(preinstalled) v.edit->textDocument()->setTextDocument(doc);
        if(adaptive) installAdapter(v.edit,doc,preinstalled);
        v.edit->setCursorVisible(false); v.frame();
        const qreal secondBefore=layout->blockBoundingRect(doc->findBlockByNumber(1)).y();
        const qreal tailBefore=layout->blockBoundingRect(doc->lastBlock()).y();
        c=QTextCursor(doc); c.setPosition(0); c.setPosition(1,QTextCursor::KeepAnchor);
        QTextCharFormat f; QFont font("DejaVu Sans"); font.setPixelSize(120); f.setFont(font); c.mergeCharFormat(f);
        const qreal secondAfter=layout->blockBoundingRect(doc->findBlockByNumber(1)).y();
        const qreal tailAfter=layout->blockBoundingRect(doc->lastBlock()).y();
        qInfo() << "NONUNIFORM second before/after tail before/after" << secondBefore << secondAfter << tailBefore << tailAfter;
        QVERIFY(secondBefore<200 && secondAfter>=220); QCOMPARE(tailAfter,tailBefore);
        if (!QByteArray(QTest::currentDataTag()).contains("rewire")) v.publicRepaint();
        auto incremental=v.frame();
        View fresh; auto *copy=doc->clone(&fresh.engine); copy->setDocumentLayout(new ProbeLayout(copy));
        fresh.edit->textDocument()->setTextDocument(copy); fresh.edit->setCursorVisible(false); auto reference=fresh.frame();
        QCOMPARE(changedPixels(incremental,reference),0);
    }
    void oracleFixtures() {
        QString result; QTextStream out(&result);
        QString differential; QTextStream diff(&differential);
        auto compareSameDocument=[&diff](QTextDocument &qtDoc,const QString &label) {
            QScopedPointer<QTextDocument> custom(qtDoc.clone());
            auto *layout=new ProbeLayout(custom.data()); layout->sheet=140; layout->gap=0;
            custom->setDocumentLayout(layout);
            for(QTextDocument *d : {&qtDoc,custom.data()}) {
                diff << label << (d==&qtDoc ? " Qt" : " prototype") << " pages=" << d->documentLayout()->pageCount() << '\n';
                for(auto b=d->begin();b.isValid();b=b.next()) {
                    const auto rect=d->documentLayout()->blockBoundingRect(b);
                    diff << " block=" << b.blockNumber() << " x=" << rect.x() << " y=" << rect.y();
                    for(int i=0;i<b.layout()->lineCount();++i) {
                        const auto line=b.layout()->lineAt(i);
                        diff << " [start=" << line.textStart() << " length=" << line.textLength() << " width=" << line.width()
                             << " baseline=" << rect.y()+line.y()+line.ascent() << ']';
                    }
                    diff << '\n';
                }
            }
        };
        auto prepare=[](QTextDocument &d) { QFont f("DejaVu Sans"); f.setPixelSize(16); d.setDefaultFont(f); d.setDocumentMargin(10); d.setPageSize({300,140}); };
        for(int bottom : {-8,0,12}) for(int top : {-6,0,18}) {
            QTextDocument d; prepare(d); QTextCursor c(&d); QTextBlockFormat a; a.setBottomMargin(bottom); c.setBlockFormat(a); c.insertText("one");
            QTextBlockFormat b; b.setTopMargin(top); c.insertBlock(b); c.insertText("two");
            d.documentLayout()->documentSize();
            auto first=d.begin(),second=first.next();
            out << "margin " << bottom << ',' << top << " y=" << d.documentLayout()->blockBoundingRect(first).y() << ',' << d.documentLayout()->blockBoundingRect(second).y() << '\n';
            compareSameDocument(d,QString("margin %1,%2").arg(bottom).arg(top));
        }
        for(bool empty : {false,true}) {
            QTextDocument d; prepare(d); QTextCursor c(&d); QTextBlockFormat b;
            b.setTopMargin(18); b.setBottomMargin(12); c.setBlockFormat(b); if(!empty) c.insertText("first");
            c.insertBlock(b); if(!empty) c.insertText("last");
            const auto size=d.documentLayout()->documentSize();
            out << "outer-empty " << empty << " first=" << d.documentLayout()->blockBoundingRect(d.begin()).y() << " last=" << d.documentLayout()->blockBoundingRect(d.lastBlock()).y() << " size=" << size.height() << '\n';
            compareSameDocument(d,QString("outer-empty %1").arg(empty));
        }
        for(bool forced : {false,true}) {
            QTextDocument d; prepare(d); QTextCursor c(&d); c.insertText(QString("line\n").repeated(5));
            QTextBlockFormat b; b.setTopMargin(18); if(forced) b.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore); c.setBlockFormat(b); c.insertText("page top");
            d.documentLayout()->documentSize(); auto block=d.lastBlock();
            out << "page-top " << forced << " block=" << d.documentLayout()->blockBoundingRect(block).y() << " line=" << block.layout()->lineAt(0).y() << '\n';
            compareSameDocument(d,QString("page-top %1").arg(forced));
        }
        for(auto mode : {QTextBlockFormat::ProportionalHeight,QTextBlockFormat::FixedHeight,QTextBlockFormat::MinimumHeight}) {
            QTextDocument d; prepare(d); QTextCursor c(&d); QTextBlockFormat b; b.setLineHeight(mode==QTextBlockFormat::ProportionalHeight ? 150 : 25,mode); c.setBlockFormat(b); c.insertText("one\u2028two");
            d.documentLayout()->documentSize(); auto block=d.begin(); auto *l=block.layout();
            out << "line-height " << int(mode);
            for(int i=0;i<l->lineCount();++i) { auto line=l->lineAt(i); out << " y=" << line.y() << " baseline=" << l->position().y()+line.y()+line.ascent() << " height=" << line.height(); } out << '\n';
            compareSameDocument(d,QString("line-height %1").arg(int(mode)));
        }
        {
            QTextDocument d; prepare(d); QTextCursor c(&d); QTextTableFormat f; f.setWidth(QTextLength(QTextLength::PercentageLength,100)); f.setCellPadding(4); f.setCellSpacing(0);
            auto *table=c.insertTable(1,2,f); table->cellAt(0,0).firstCursorPosition().insertText("i"); table->cellAt(0,1).firstCursorPosition().insertText("unequal unbreakablelongword content");
            d.documentLayout()->documentSize();
            for(int col=0;col<2;++col) { auto b=table->cellAt(0,col).firstCursorPosition().block(); out << "column " << col << " x=" << d.documentLayout()->blockBoundingRect(b).x() << " width=" << b.layout()->lineAt(0).width() << '\n'; }
            compareSameDocument(d,"table unequal");
        }
        QFile differentialFile(QStringLiteral(PHASE0_SOURCE_DIR)+"/.scratch/5-oracle-diff.txt");
        if(QDir(QStringLiteral(PHASE0_SOURCE_DIR)+"/.scratch").exists()) {
            QVERIFY(differentialFile.open(QIODevice::WriteOnly)); differentialFile.write(differential.toUtf8());
        }
        qInfo().noquote() << "ORACLE\n" << result;
        QFile actual(QStringLiteral(PHASE0_SOURCE_DIR)+"/.scratch/5-oracle-actual.txt"); if(QDir(QStringLiteral(PHASE0_SOURCE_DIR)+"/.scratch").exists()) {
            QVERIFY(actual.open(QIODevice::WriteOnly)); actual.write(result.toUtf8()); actual.close();
        }
        QFile expected(QStringLiteral(PHASE0_SOURCE_DIR)+"/tests/phase0-oracle.txt"); QVERIFY(expected.open(QIODevice::ReadOnly));
        QCOMPARE(result.toUtf8(),expected.readAll());
    }
    void bookTiming() {
        QTextDocument book; rt::StyleSheet styles; QString error;
        const QString bookPath=qEnvironmentVariable("RICHTEXT_BOOK","/home/caetano/lab/.wt/office-samples/livro-300p.odt");
        if(!QFile::exists(bookPath)) QSKIP("Set RICHTEXT_BOOK to run the optional book timing probe");
        QVERIFY2(rt::readOdt(bookPath,&book,&styles,&error),qPrintable(error));
        book.setPageSize({794,1150}); book.documentLayout()->documentSize();
        // The real book contains tables: runtime eligibility must retain Qt. This paragraph
        // projection measures shaping cost only and is explicitly NOT the actual-book gate.
        QTextDocument paragraphs; paragraphs.setDefaultFont(book.defaultFont()); paragraphs.setPlainText(book.toPlainText());
        auto *probe=new ProbeLayout(&paragraphs); paragraphs.setDocumentLayout(probe); probe->configure(794,1123,27);
        qInfo() << "PERF load start" << loadAverage() << "book blocks" << book.blockCount() << "projection blocks" << paragraphs.blockCount();
        for(QTextDocument *doc : {&book,&paragraphs}) {
            QList<double> times;
            for(int i=0;i<5;++i) {
                QTextCursor c(doc); c.setPosition(doc->characterCount()/2);
                if(doc==&paragraphs) probe->counts={};
                QElapsedTimer timer; timer.start(); c.insertText("x"); doc->documentLayout()->documentSize(); times.append(timer.nsecsElapsed()/1e6);
                if(doc==&paragraphs) qInfo() << "PROJECTION counters" << probe->counts.callbacks << probe->counts.blocks << probe->counts.characters << probe->counts.placements;
                doc->undo();
            }
            std::sort(times.begin(),times.end()); qInfo() << "PERF" << (doc==&book ? "actual book Qt fallback" : "book plain-text projection prototype") << "median ms" << times[2];
        }
        qInfo() << "PERF load end" << loadAverage();
    }
    void layoutContract() {
        QTextDocument doc; doc.setPlainText("one\ntwo\nthree");
        auto *layout = new ProbeLayout(&doc); doc.setDocumentLayout(layout);
        QVERIFY(layout->documentSize().height() > 0);
        QCOMPARE(doc.begin().layout()->lineCount(), 1);
    }
};
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    Phase0Tests t; return QTest::qExec(&t, argc, argv);
}
#include "layout_phase0_test.moc"
