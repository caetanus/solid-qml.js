#include "shims/richtext_layout.h"
#include "shims/richtext_table_decoration.h"
#include "shims/richtext_odf.h"
#include "shims/richtext.h"
#include "shims/richtext_qt_p.h"
// Public-API Qt oracle and custom table acceptance tests.
#include <QTest>
#include <QTextDocument>
#include <QTextTable>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextImageFormat>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickTextDocument>
#include <QSGRendererInterface>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QRandomGenerator>

namespace {
constexpr int sheet = 120, gap = 20, stride = sheet + gap;
QTextTable *makeFixture(QTextDocument &d, int lines) {
    QFont font("DejaVu Sans"); font.setPixelSize(16); d.setDefaultFont(font);
    d.setDocumentMargin(10); d.setPageSize({300,stride});
    auto root=d.rootFrame()->frameFormat(); root.setBottomMargin(10+gap); d.rootFrame()->setFrameFormat(root);
    QTextTableFormat format;
    format.setWidth(QTextLength(QTextLength::PercentageLength,100));
    format.setBorder(1); format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    format.setCellPadding(4); format.setCellSpacing(0); format.setBorderCollapse(true);
    QTextCursor cursor(&d); auto *table=cursor.insertTable(1,2,format);
    // Seven ordinary soft breaks produce eight font-height lines, no objects.
    table->cellAt(0,0).firstCursorPosition().insertText(QString(lines-1,QChar::LineSeparator));
    auto cell=table->cellAt(0,1);
    auto cellFormat=cell.format().toTableCellFormat();
    cellFormat.setVerticalAlignment(QTextCharFormat::AlignMiddle); cell.setFormat(cellFormat);
    QTextCharFormat red; red.setForeground(Qt::red);
    cell.firstCursorPosition().insertText("X",red);
    return table;
}
int redPixels(const QImage &image, int first, int last) {
    int count=0;
    for(int y=qMax(0,first);y<qMin(last,image.height());++y)
        for(int x=0;x<image.width();++x) {
            const QColor c=image.pixelColor(x,y);
            count+=c.red()>c.green()+2 && c.red()>c.blue()+2;
        }
    return count;
}
void artifact(const QImage &image, const QString &name) {
    const auto dir=qEnvironmentVariable("TABLE_ARTIFACT_DIR");
    if(!dir.isEmpty()) QVERIFY(image.save(QDir(dir).filePath(name)));
}
}
static QTextTable *fixture(QTextDocument &d, int kind) {
    QFont f("DejaVu Sans"); f.setPixelSize(16); d.setDefaultFont(f);
    d.setDocumentMargin(10); d.setPageSize({300,140});
    QTextCursor c(&d);
    QTextTableFormat tf;
    tf.setWidth(QTextLength(QTextLength::PercentageLength,100));
    tf.setBorder(1); tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    tf.setCellPadding(4); tf.setCellSpacing(0); tf.setBorderCollapse(true);
    if (kind==1) tf.setColumnWidthConstraints({QTextLength(QTextLength::FixedLength,80),QTextLength(QTextLength::VariableLength,0)});
    if (kind==2) tf.setColumnWidthConstraints({QTextLength(QTextLength::PercentageLength,30),QTextLength(QTextLength::PercentageLength,70)});
    if (kind==3) { tf.setBorderCollapse(false); tf.setCellSpacing(3); }
    if (kind==4) tf.setWidth(QTextLength(QTextLength::VariableLength,0));
    if (kind==12) c.insertText("before\nbefore\nbefore\nbefore");
    auto *t=c.insertTable(kind==6 || kind==7 || kind==8 ? 2 : 1,2,tf);
    if (kind==6) t->mergeCells(0,0,1,2);
    if (kind==7 || kind==8) t->mergeCells(0,0,2,1);
    for(int r=0;r<t->rows();++r) for(int col=0;col<t->columns();++col) {
        const auto cell=t->cellAt(r,col);
        if(cell.row()!=r || cell.column()!=col) continue;
        QString text=col ? "unequal unbreakablelongword content" : "i";
        if(kind==5) text=col ? "short" : QString("W").repeated(60);
        if(kind==8 || kind==9 || kind==10 || kind==11) text=QString("cell words ").repeated(col && kind!=10 ? 3 : 55);
        if(kind==13) text.clear();
        cell.firstCursorPosition().insertText(text);
        if(kind==9 || kind==11 || kind==16 || kind==17) {
            auto format=cell.format().toTableCellFormat();
            format.setVerticalAlignment(kind==9 || kind==16 ? QTextCharFormat::AlignMiddle : QTextCharFormat::AlignBottom);
            auto writable=cell; writable.setFormat(format);
        }
        if(kind==14) {
            auto format=cell.format().toTableCellFormat(); format.setLeftPadding(12); format.setBottomPadding(9);
            auto writable=cell; writable.setFormat(format);
        }
    }
    if(kind==15) {
        QImage image(30,190,QImage::Format_ARGB32); image.fill(Qt::red);
        d.addResource(QTextDocument::ImageResource,QUrl("image:oversized"),image);
        t->cellAt(0,0).firstCursorPosition().insertImage("image:oversized");
    }
    return t;
}


struct BookView {
    QQmlEngine engine;
    QQuickWindow window;
    QScopedPointer<QQuickItem> edit;
    RichTextHandler handler;
    QTextDocument *doc=nullptr;
    rt::RichTextLayout *layout=nullptr;
    bool initialize(const QTextDocument &source, bool custom, qreal zoom, bool culled) {
        window.resize(qCeil(794*zoom),qCeil(700*zoom)); window.setColor(Qt::white);
        window.contentItem()->setClip(true);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap; cursorVisible: false; color: 'black' }",QUrl());
        edit.reset(qobject_cast<QQuickItem *>(component.create())); if(!edit) return false;
        edit->setParentItem(window.contentItem()); edit->setWidth(794*zoom);
        doc=source.clone(&engine);
        auto *quick=qvariant_cast<QQuickTextDocument *>(edit->property("textDocument"));
        quick->setTextDocument(doc); handler.setDocument(quick); handler.setLayoutEngine("qt");
        handler.setZoom(zoom); handler.configurePages(794*zoom,1118*zoom,32*zoom,{94*zoom,113*zoom,94*zoom,113*zoom});
        if(custom) {
            handler.setLayoutEngine("custom");
            layout=dynamic_cast<rt::RichTextLayout *>(doc->documentLayout());
            if(!layout) return false;
            if(qEnvironmentVariable("TABLE_DECORATION")=="painted") {
                delete edit->findChild<QQuickItem *>("tableDecoration");
                new rt::TableDecoration(layout,edit.data());
            }
        }
        edit->setHeight(doc->documentLayout()->documentSize().height());
        edit->setFlag(QQuickItem::ItemObservesViewport,culled);
        window.show(); return QTest::qWaitForWindowExposed(&window);
    }
    QImage frame(qreal y, bool culled) {
        edit->setY(-y); edit->setFlag(QQuickItem::ItemObservesViewport,culled);
        if(culled) handler.refreshViewport();
        QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents();
        return window.grabWindow();
    }
};

class TablePhase2Tests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QFile load("/proc/loadavg"); QVERIFY(load.open(QIODevice::ReadOnly));
        qInfo().noquote()<<"LOAD"<<load.readAll().trimmed();
    }
    void odfCellAlignmentRoundTrip() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QTextDocument doc; auto *table=makeFixture(doc,8);
        auto left=table->cellAt(0,0); auto format=left.format().toTableCellFormat();
        format.setVerticalAlignment(QTextCharFormat::AlignBottom); left.setFormat(format);
        rt::StyleSheet styles; QString error; const auto path=dir.filePath("alignment.odt");
        QVERIFY2(rt::writeOdt(path,&doc,styles,1,&error),qPrintable(error));
        if(qEnvironmentVariableIsSet("TABLE_DUMP")) QFile::copy(path,".scratch/8-alignment.odt");
        QTextDocument loaded;
        QVERIFY2(rt::readOdt(path,&loaded,&styles,&error),qPrintable(error));
        const auto frames=loaded.rootFrame()->childFrames(); QCOMPARE(frames.size(),1);
        auto *copy=qobject_cast<QTextTable *>(frames.first()); QVERIFY(copy);
        QCOMPARE(copy->cellAt(0,0).format().verticalAlignment(),QTextCharFormat::AlignBottom);
        QCOMPARE(copy->cellAt(0,1).format().verticalAlignment(),QTextCharFormat::AlignMiddle);
    }
    void cellHitCursorSelection() {
        QTextDocument source; makeFixture(source,80);
        BookView view; QVERIFY(view.initialize(source,true,1,true));
        auto *table=qobject_cast<QTextTable *>(view.doc->rootFrame()->childFrames().first()); QVERIFY(table);
        auto left=table->cellAt(0,0),right=table->cellAt(0,1);
        const auto lb=left.firstCursorPosition().block(),rb=right.firstCursorPosition().block();
        const auto last=lb.layout()->lineAt(lb.layout()->lineCount()-1);
        const QPointF emptyContinuation(rb.layout()->position().x()+1,lb.layout()->position().y()+last.y()+last.height()/2);
        const int hit=view.layout->hitTest(emptyContinuation,Qt::FuzzyHit);
        QVERIFY2(hit>=right.firstPosition() && hit<=right.lastPosition(),"Empty continuation must retain the clicked cell");
        view.layout->counters={}; int queries=0;
        for(int col=0;col<2;++col) {
            const auto cell=table->cellAt(0,col);
            for(auto it=cell.begin();it!=cell.end();++it) {
                auto b=it.currentBlock(); if(!b.isValid()) continue;
                for(int i=0;i<b.layout()->lineCount();++i) {
                    const auto line=b.layout()->lineAt(i);
                    const int position=b.position()+line.textStart();
                    QRectF caret;
                    QVERIFY(QMetaObject::invokeMethod(view.edit.data(),"positionToRectangle",Q_RETURN_ARG(QRectF,caret),Q_ARG(int,position)));
                    int actual=-1;
                    QVERIFY(QMetaObject::invokeMethod(view.edit.data(),"positionAt",Q_RETURN_ARG(int,actual),Q_ARG(qreal,caret.center().x()),Q_ARG(qreal,caret.center().y())));
                    QCOMPARE(actual,position); ++queries;
                    view.edit->setProperty("cursorPosition",position);
                    QCOMPARE(view.edit->property("cursorRectangle").toRectF(),caret);
                    const qreal page=qFloor(caret.y()/1150);
                    QVERIFY(caret.y()>=page*1150+94 && caret.bottom()<=page*1150+1024);
                }
            }
            QVERIFY(QMetaObject::invokeMethod(view.edit.data(),"select",Q_ARG(int,cell.firstPosition()),Q_ARG(int,cell.lastPosition())));
            QCOMPARE(view.edit->property("selectionStart").toInt(),cell.firstPosition());
            QCOMPARE(view.edit->property("selectionEnd").toInt(),cell.lastPosition());
        }
        qInfo()<<"HIT QUERIES/CANDIDATES"<<queries<<view.layout->counters.hitCandidates;
        QVERIFY(view.layout->counters.hitCandidates>0);
        QVERIFY(view.layout->counters.hitCandidates<=quint64(queries*16));
        QCOMPARE(view.layout->counters.callbacks,quint64(0));
        QAbstractTextDocumentLayout::PaintContext context;
        QAbstractTextDocumentLayout::Selection selection;
        selection.cursor=left.firstCursorPosition(); selection.cursor.setPosition(left.lastPosition(),QTextCursor::KeepAnchor);
        selection.format.setBackground(Qt::blue); selection.format.setForeground(Qt::white); context.selections.append(selection);
        QImage image(794,view.doc->pageCount()*1150,QImage::Format_RGB32); image.fill(Qt::white);
        { QPainter painter(&image); view.layout->draw(&painter,context); }
        for(int y=0;y<image.height();++y) if(y%1150<94 || y%1150>=1024) {
            const auto *pixels=reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for(int x=0;x<image.width();++x) QVERIFY(pixels[x]!=qRgb(0,0,255));
        }
    }
    void bookPixels_data() {
        QTest::addColumn<qreal>("zoom"); QTest::newRow("zoom-1")<<qreal(1); QTest::newRow("zoom-1.5")<<qreal(1.5);
    }
    void bookPixels() {
        if(!qEnvironmentVariableIsSet("RICHTEXT_BOOK")) QSKIP("Set RICHTEXT_BOOK");
        QFETCH(qreal,zoom);
        QTextDocument source; rt::StyleSheet styles; QString error;
        QVERIFY2(rt::readOdt(qEnvironmentVariable("RICHTEXT_BOOK"),&source,&styles,&error),qPrintable(error));
        BookView qt,qtFull,custom,customFull;
        QVERIFY(qt.initialize(source,false,zoom,true)); QVERIFY(qtFull.initialize(source,false,zoom,false));
        QVERIFY(custom.initialize(source,true,zoom,true)); QVERIFY(customFull.initialize(source,true,zoom,false));
        if(qEnvironmentVariableIsSet("TABLE_DUMP")) {
            auto a=qt.doc->begin(),b=custom.doc->begin();
            for(int i=0;i<12;++i,a=a.next(),b=b.next()) {
                qInfo()<<"ZOOM GEOMETRY"<<zoom<<i<<qt.doc->documentLayout()->blockBoundingRect(a)<<custom.layout->blockBoundingRect(b);
                for(int n=0;n<qMin(a.layout()->lineCount(),b.layout()->lineCount());++n)
                    qInfo()<<"ZOOM LINE"<<n<<a.layout()->lineAt(n).position()<<b.layout()->lineAt(n).position()<<a.layout()->lineAt(n).height()<<b.layout()->lineAt(n).height();
            }
        }
        QCOMPARE(custom.doc->pageCount(),qt.doc->pageCount());
        for(auto a=qt.doc->begin(),b=custom.doc->begin();a.isValid();a=a.next(),b=b.next()) {
            const auto ar=qt.doc->documentLayout()->blockBoundingRect(a),br=custom.layout->blockBoundingRect(b);
            QCOMPARE(a.layout()->lineCount(),b.layout()->lineCount());
            for(int n=0;n<a.layout()->lineCount();++n) {
                const auto al=a.layout()->lineAt(n),bl=b.layout()->lineAt(n);
                QCOMPARE(al.textStart(),bl.textStart()); QCOMPARE(al.textLength(),bl.textLength());
                const auto ap=ar.topLeft()+al.position(),bp=br.topLeft()+bl.position();
                QCOMPARE(qFloor(ap.y()/(1150*zoom)),qFloor(bp.y()/(1150*zoom)));
                if(QTextCursor(b).currentTable()) {
                    int local=bl.textStart()+bl.textLength()/2;
                    while(local>bl.textStart() && !b.layout()->isValidCursorPosition(local)) --local;
                    const int position=b.position()+local;
                    QRectF caret; int hit=-1;
                    QVERIFY(QMetaObject::invokeMethod(custom.edit.data(),"positionToRectangle",Q_RETURN_ARG(QRectF,caret),Q_ARG(int,position)));
                    QVERIFY(QMetaObject::invokeMethod(custom.edit.data(),"positionAt",Q_RETURN_ARG(int,hit),Q_ARG(qreal,caret.center().x()),Q_ARG(qreal,caret.center().y())));
                    QCOMPARE(hit,position);
                }
                QVERIFY2(qAbs(ap.x()-bp.x())<=.5 && qAbs(ap.y()-bp.y())<=.5 && qAbs(al.width()-bl.width())<=.5,
                         qPrintable(QString("zoom %1 block %2 line %3 Qt %4,%5 custom %6,%7").arg(zoom).arg(a.blockNumber()).arg(n).arg(ap.x()).arg(ap.y()).arg(bp.x()).arg(bp.y())));
            }
        }
        const auto tables=qt.doc->rootFrame()->childFrames(); QVERIFY(tables.size()>2);
        const auto tableY=[&](int n) { return qt.doc->documentLayout()->frameBoundingRect(tables[n]).y(); };
        const QList<qreal> offsets={0,qMax(0.,tableY(0)-50*zoom),tableY(tables.size()/2)-50*zoom,tableY(tables.size()-1)-50*zoom,qt.doc->documentLayout()->documentSize().height()/3,tableY(0)-200*zoom};
        for(int i=0;i<offsets.size();++i) {
            const auto expected=qt.frame(offsets[i],true), full=qtFull.frame(offsets[i],false);
            const auto actual=custom.frame(offsets[i],true), unculled=customFull.frame(offsets[i],false);
            qInfo()<<"BOOK PIXELS"<<zoom<<i<<offsets[i];
            if(expected!=full || actual!=unculled || actual!=expected) {
                artifact(expected,QString("8-book-qt-%1-%2.png").arg(zoom).arg(i));
                artifact(actual,QString("8-book-custom-%1-%2.png").arg(zoom).arg(i));
                artifact(full,QString("8-book-qt-full-%1-%2.png").arg(zoom).arg(i));
                artifact(unculled,QString("8-book-custom-full-%1-%2.png").arg(zoom).arg(i));
            }
            QCOMPARE(expected,full); QCOMPARE(actual,unculled);
            QCOMPARE(actual,expected);
        }
    }
    void quickTableDecoration() {
        const auto render=[&](bool custom) {
            QQmlEngine engine; QQuickWindow window; window.resize(300,140); window.setColor(Qt::white);
            QQmlComponent component(&engine);
            component.setData("import QtQuick\nTextEdit { width: 300; height: 280; textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap; cursorVisible: false; color: 'black' }",QUrl());
            QScopedPointer<QQuickItem> edit(qobject_cast<QQuickItem *>(component.create()));
            if(!edit) return QImage();
            edit->setParentItem(window.contentItem());
            auto *doc=new QTextDocument(&engine); fixture(*doc,0);
            rt::RichTextLayout *layout=nullptr;
            if(custom) { layout=new rt::RichTextLayout(doc); doc->setDocumentLayout(layout); }
            auto *quickDoc=qvariant_cast<QQuickTextDocument *>(edit->property("textDocument"));
            quickDoc->setTextDocument(doc); doc->setPageSize({300,140});
            if(custom) qEnvironmentVariable("TABLE_DECORATION")=="painted"
                ? static_cast<QQuickItem *>(new rt::TableDecoration(layout,edit.data()))
                : static_cast<QQuickItem *>(new rt::NodeTableDecoration(layout,edit.data()));
            window.show(); if(!QTest::qWaitForWindowExposed(&window)) return QImage();
            QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents();
            return window.grabWindow();
        };
        const auto expected=render(false),actual=render(true);
        QVERIFY(!expected.isNull()); QVERIFY(!actual.isNull());
        artifact(expected,"8-quick-decoration-qt.png"); artifact(actual,"8-quick-decoration-custom.png");
        QCOMPARE(actual,expected);
    }
    void tablePainterDecoration_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("collapsed")<<0;
        QTest::newRow("separate")<<3;
        QTest::newRow("background")<<13;
    }
    void tablePainterDecoration() {
        QFETCH(int,kind); QTextDocument qt; auto *table=fixture(qt,kind);
        if(kind==13) { auto cell=table->cellAt(0,1); auto format=cell.format(); format.setBackground(QColor("#d8f0ff")); cell.setFormat(format); }
        qt.documentLayout()->documentSize();
        QScopedPointer<QTextDocument> custom(qt.clone()); custom->setDocumentLayout(new rt::RichTextLayout(custom.data()));
        QImage expected(300,280,QImage::Format_ARGB32_Premultiplied); expected.fill(Qt::white);
        QImage actual=expected;
        { QPainter p(&expected); qt.drawContents(&p); }
        { QPainter p(&actual); custom->drawContents(&p); }
        artifact(expected,QString("8-decoration-qt-%1.png").arg(kind));
        artifact(actual,QString("8-decoration-custom-%1.png").arg(kind));
        QCOMPARE(actual,expected);
    }
    void incrementalTables() {
        QTextDocument doc; auto *table=fixture(doc,10);
        auto root=doc.rootFrame()->frameFormat(); root.setBottomMargin(30); doc.rootFrame()->setFrameFormat(root);
        auto *engine=new rt::RichTextLayout(&doc); doc.setDocumentLayout(engine);
        QRandomGenerator random(808);
        for(int step=0;step<60;++step) {
            if(step%7==6) doc.undo();
            else {
                auto cursor=table->cellAt(0,random.bounded(2)).firstCursorPosition();
                cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::MoveAnchor,random.bounded(12));
                if(step%7==0) cursor.insertBlock();
                else if(step%7==1) cursor.deleteChar();
                else if(step%7==2) { QTextCharFormat format; format.setFontPointSize(10+random.bounded(10)); cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor,3); cursor.mergeCharFormat(format); }
                else cursor.insertText("test words ");
            }
            QScopedPointer<QTextDocument> cold(doc.clone()); cold->setDocumentLayout(new rt::RichTextLayout(cold.data()));
            if(qEnvironmentVariableIsSet("TABLE_DUMP")) qInfo()<<"EDIT"<<step<<doc.pageCount()<<cold->pageCount();
            QCOMPARE(doc.pageCount(),cold->pageCount());
            auto a=doc.begin(),b=cold->begin();
            for(;a.isValid();a=a.next(),b=b.next()) {
                QCOMPARE(engine->blockBoundingRect(a),cold->documentLayout()->blockBoundingRect(b));
                QCOMPARE(a.layout()->lineCount(),b.layout()->lineCount());
                for(int i=0;i<a.layout()->lineCount();++i) {
                    auto al=a.layout()->lineAt(i),bl=b.layout()->lineAt(i);
                    QCOMPARE(al.textStart(),bl.textStart()); QCOMPARE(al.textLength(),bl.textLength());
                    QCOMPARE(al.position(),bl.position()); QCOMPARE(al.width(),bl.width());
                }
            }
        }
    }
    void realBookIncremental() {
        if(!qEnvironmentVariableIsSet("RICHTEXT_BOOK")) QSKIP("Set RICHTEXT_BOOK");
        QTextDocument source; rt::StyleSheet styles; QString error;
        QVERIFY2(rt::readOdt(qEnvironmentVariable("RICHTEXT_BOOK"),&source,&styles,&error),qPrintable(error));
        BookView view; QVERIFY(view.initialize(source,true,1,true)); auto &doc=*view.doc;
        QRandomGenerator random(808);
        for(int step=0;step<12;++step) {
            QTextCursor cursor(&doc);
            const auto frames=doc.rootFrame()->childFrames();
            auto *table=qobject_cast<QTextTable *>(frames[random.bounded(frames.size())]); QVERIFY(table);
            if(step%3==0) cursor=table->cellAt(random.bounded(table->rows()),random.bounded(table->columns())).firstCursorPosition();
            else if(step%3==1) cursor.setPosition(table->lastPosition()+1);
            else cursor.setPosition(random.bounded(doc.characterCount()-1));
            if(step%4==0) cursor.insertBlock();
            else if(step%4==1) cursor.insertText("seeded table edit ");
            else if(step%4==2) cursor.deleteChar();
            else doc.undo();
            QScopedPointer<QTextDocument> cold(doc.clone()); cold->setDocumentLayout(new rt::RichTextLayout(cold.data()));
            QCOMPARE(doc.pageCount(),cold->pageCount());
            auto a=doc.begin(),b=cold->begin();
            for(;a.isValid();a=a.next(),b=b.next()) {
                QCOMPARE(doc.documentLayout()->blockBoundingRect(a),cold->documentLayout()->blockBoundingRect(b));
                QCOMPARE(a.layout()->lineCount(),b.layout()->lineCount());
                for(int i=0;i<a.layout()->lineCount();++i) {
                    const auto al=a.layout()->lineAt(i),bl=b.layout()->lineAt(i);
                    QCOMPARE(al.textStart(),bl.textStart()); QCOMPARE(al.textLength(),bl.textLength());
                    QCOMPARE(al.position(),bl.position()); QCOMPARE(al.width(),bl.width());
                    const QPointF point=al.naturalTextRect().center()+a.layout()->position();
                    QCOMPARE(doc.documentLayout()->hitTest(point,Qt::FuzzyHit),cold->documentLayout()->hitTest(point,Qt::FuzzyHit));
                }
            }
        }
    }
    void bookOracle() {
        if(!qEnvironmentVariableIsSet("RICHTEXT_BOOK")) QSKIP("Set RICHTEXT_BOOK");
        QTextDocument doc; rt::StyleSheet styles; QString error;
        QVERIFY2(rt::readOdt(qEnvironmentVariable("RICHTEXT_BOOK"),&doc,&styles,&error),qPrintable(error));
        doc.setPageSize({794,1150});
        auto root=doc.rootFrame()->frameFormat(); root.setTopMargin(94); root.setRightMargin(113);
        root.setBottomMargin(126); root.setLeftMargin(113); doc.rootFrame()->setFrameFormat(root);
        doc.documentLayout()->documentSize();
        qInfo()<<"BOOK"<<doc.blockCount()<<doc.characterCount()<<doc.pageCount();
        for (auto *frame:doc.rootFrame()->childFrames()) {
            auto *table=qobject_cast<QTextTable *>(frame); QVERIFY(table);
            for(int row=0;row<table->rows();++row) {
                qreal first=1e20,last=0;
                for(int col=0;col<table->columns();++col) {
                    const auto cell=table->cellAt(row,col);
                    for(auto it=cell.begin();it!=cell.end();++it) {
                        auto b=it.currentBlock(); if(!b.isValid()) continue;
                        const auto rect=doc.documentLayout()->blockBoundingRect(b);
                        for(int i=0;i<b.layout()->lineCount();++i) {
                            const auto l=b.layout()->lineAt(i); const qreal y=rect.y()+l.y();
                            first=qMin(first,y); last=qMax(last,y+l.height());
                            if(y<qFloor(y/1150)*1150+94 || y+l.height()>qFloor(y/1150)*1150+1024)
                                qInfo()<<"OUTSIDE"<<table->firstPosition()<<row<<col<<b.blockNumber()<<i<<y<<l.height();
                        }
                    }
                }
                if(qEnvironmentVariableIsSet("TABLE_DUMP")) qInfo()<<"ROW"<<table->firstPosition()<<row<<first<<last;
            }
        }
        QScopedPointer<QTextDocument> custom(doc.clone());
        auto *imageHandler=dynamic_cast<QObject *>(custom->documentLayout()->handlerForObject(QTextFormat::ImageObject));
        if(imageHandler) imageHandler->setParent(custom.data());
        auto *engine=new rt::RichTextLayout(custom.data());
        custom->setDocumentLayout(engine);
        if(imageHandler) engine->registerHandler(QTextFormat::ImageObject,imageHandler);
        custom->markContentsDirty(0,custom->characterCount());
        qInfo()<<"CUSTOM BOOK PAGES"<<custom->pageCount();
        if(qEnvironmentVariableIsSet("TABLE_DUMP")) {
            for(auto b=doc.findBlockByNumber(qEnvironmentVariableIntValue("TABLE_BLOCK"));b.isValid() && b.blockNumber()<qEnvironmentVariableIntValue("TABLE_BLOCK")+20;b=b.next()) {
                auto c=custom->findBlockByNumber(b.blockNumber());
                qInfo()<<"CELL"<<b.blockNumber()<<b.text()<<b.layout()->minimumWidth()<<b.layout()->maximumWidth()<<c.layout()->maximumWidth()<<doc.documentLayout()->blockBoundingRect(b)<<custom->documentLayout()->blockBoundingRect(c);
            }
        }
        auto a=doc.begin(),b=custom->begin();
        for(;a.isValid();a=a.next(),b=b.next()) {
            const auto ar=doc.documentLayout()->blockBoundingRect(a),br=custom->documentLayout()->blockBoundingRect(b);
            if(qEnvironmentVariableIsSet("TABLE_DUMP") && a.blockNumber()<36) {
                const auto f=a.blockFormat();
                qInfo()<<"COMPARE"<<a.blockNumber()<<a.text()<<"Qt"<<ar<<"custom"<<br<<"margins"<<f.leftMargin()<<f.rightMargin()<<f.textIndent()<<f.indent()<<"minmax"<<a.layout()->minimumWidth()<<a.layout()->maximumWidth();
            }
            QCOMPARE(b.layout()->lineCount(),a.layout()->lineCount());
            for(int i=0;i<a.layout()->lineCount();++i) {
                const auto al=a.layout()->lineAt(i),bl=b.layout()->lineAt(i);
                QCOMPARE(bl.textStart(),al.textStart()); QCOMPARE(bl.textLength(),al.textLength());
                const auto ap=ar.topLeft()+al.position(),bp=br.topLeft()+bl.position();
                QVERIFY2(qAbs(bp.y()-ap.y())<=.5 && qAbs(bp.x()-ap.x())<=.5 && qAbs(bl.width()-al.width())<=.5,
                         qPrintable(QString("block %1 line %2 Qt %3,%4 w%5 custom %6,%7 w%8").arg(a.blockNumber()).arg(i).arg(ap.x()).arg(ap.y()).arg(al.width()).arg(bp.x()).arg(bp.y()).arg(bl.width())));
            }
        }
        QCOMPARE(custom->pageCount(),doc.pageCount());
    }
    void customTableMatrix_data() {
        QTest::addColumn<int>("kind");
        const char *names[]={"unequal","fixed","percentage","separate","auto","unbreakable","colspan","rowspan","long-rowspan","middle","simultaneous-long","bottom","boundary","empty","padding","oversized","nonsplit-middle","nonsplit-bottom"};
        for(int k=0;k<18;++k) QTest::newRow(names[k])<<k;
    }
    void customTableMatrix() {
        QFETCH(int,kind);
        QTextDocument qt; fixture(qt,kind);
        auto root=qt.rootFrame()->frameFormat(); root.setBottomMargin(30); qt.rootFrame()->setFrameFormat(root);
        qt.documentLayout()->documentSize();
        QScopedPointer<QTextDocument> custom(qt.clone());
        auto *imageHandler=dynamic_cast<QObject *>(custom->documentLayout()->handlerForObject(QTextFormat::ImageObject));
        if(imageHandler) imageHandler->setParent(custom.data());
        auto *engine=new rt::RichTextLayout(custom.data());
        custom->setDocumentLayout(engine);
        if(imageHandler) engine->registerHandler(QTextFormat::ImageObject,imageHandler);
        custom->markContentsDirty(0,custom->characterCount());
        if(qEnvironmentVariableIsSet("TABLE_DUMP")) {
            for(auto b=custom->begin();b.isValid();b=b.next()) {
                for(auto it=b.begin();!it.atEnd();++it) { auto f=it.fragment(); qInfo()<<"FORMAT"<<f.text()<<f.charFormat().objectType()<<f.charFormat().toImageFormat().name(); }
                qInfo()<<"CUSTOM"<<b.blockNumber()<<custom->documentLayout()->blockBoundingRect(b);
                for(int i=0;i<b.layout()->lineCount();++i) { auto l=b.layout()->lineAt(i); qInfo()<<"LINE"<<l.textStart()<<l.textLength()<<l.position()<<l.height()<<l.width(); }
            }
        }
        const auto qtFrames=qt.rootFrame()->childFrames(), customFrames=custom->rootFrame()->childFrames();
        QCOMPARE(qtFrames.size(),customFrames.size());
        for(int i=0;i<qtFrames.size();++i) {
            const auto a=qt.documentLayout()->frameBoundingRect(qtFrames[i]);
            const auto b=custom->documentLayout()->frameBoundingRect(customFrames[i]);
            QVERIFY2(qAbs(a.x()-b.x())<=.5 && qAbs(a.y()-b.y())<=.5 && qAbs(a.width()-b.width())<=.5 && qAbs(a.height()-b.height())<=.5,
                     qPrintable(QString("frame Qt %1,%2 %3x%4 custom %5,%6 %7x%8").arg(a.x()).arg(a.y()).arg(a.width()).arg(a.height()).arg(b.x()).arg(b.y()).arg(b.width()).arg(b.height())));
        }
        QCOMPARE(custom->pageCount(),qt.pageCount());
        auto a=qt.begin(), b=custom->begin();
        for(;a.isValid();a=a.next(),b=b.next()) {
            const auto ar=qt.documentLayout()->blockBoundingRect(a),br=custom->documentLayout()->blockBoundingRect(b);
            if(qEnvironmentVariableIsSet("TABLE_DUMP") && a.blockNumber()<36) {
                const auto f=a.blockFormat();
                qInfo()<<"COMPARE"<<a.blockNumber()<<a.text()<<"Qt"<<ar<<"custom"<<br<<"margins"<<f.leftMargin()<<f.rightMargin()<<f.textIndent()<<f.indent()<<"minmax"<<a.layout()->minimumWidth()<<a.layout()->maximumWidth();
            }
            QCOMPARE(b.layout()->lineCount(),a.layout()->lineCount());
            for(int i=0;i<a.layout()->lineCount();++i) {
                const auto al=a.layout()->lineAt(i),bl=b.layout()->lineAt(i);
                QCOMPARE(bl.textStart(),al.textStart()); QCOMPARE(bl.textLength(),al.textLength());
                const auto ap=ar.topLeft()+al.position(),bp=br.topLeft()+bl.position();
                // Long middle/bottom rows use the maintainer's invariant oracle.
                if(kind==9 || kind==11 || kind==16 || kind==17) {
                    const auto page=qFloor(bp.y()/stride);
                    QVERIFY(bp.y()>=page*stride+10);
                    QVERIFY(bp.y()+bl.height()<=page*stride+110);
                } else {
                    QCOMPARE(int(bp.y()/stride),int(ap.y()/stride));
                    QVERIFY2(qAbs(bp.y()-ap.y())<=.5 && qAbs(bp.x()-ap.x())<=.5 && qAbs(bl.width()-al.width())<=.5,
                             qPrintable(QString("block %1 line %2 Qt %3,%4 w%5 custom %6,%7 w%8").arg(a.blockNumber()).arg(i).arg(ap.x()).arg(ap.y()).arg(al.width()).arg(bp.x()).arg(bp.y()).arg(bl.width())));
                }
            }
        }
    }
    void tableOracleMatrix() {
        if(!qEnvironmentVariableIsSet("TABLE_DUMP")) QSKIP("Opt-in oracle diagnostics");
        for (int kind=0;kind<16;++kind) {
            QTextDocument d; auto *table=fixture(d,kind);
            auto root=d.rootFrame()->frameFormat(); root.setBottomMargin(30); d.rootFrame()->setFrameFormat(root);
            d.documentLayout()->documentSize();
            qInfo()<<"TABLE"<<kind<<"rect"<<d.documentLayout()->frameBoundingRect(table)<<"pages"<<d.pageCount();
            for(auto b=d.begin(); b.isValid(); b=b.next()) {
                qInfo()<<"BLOCK"<<b.blockNumber()<<"rect"<<d.documentLayout()->blockBoundingRect(b)<<"pos"<<b.layout()->position()<<"min/max"<<b.layout()->minimumWidth()<<b.layout()->maximumWidth();
                for(int i=0;i<b.layout()->lineCount();++i) {
                    auto l=b.layout()->lineAt(i);
                    qInfo()<<"LINE"<<l.textStart()<<l.textLength()<<l.position()+b.layout()->position()<<l.width()<<l.height();
                }
            }
        }
    }
    void qtMiddleAlignmentGapWitness() {
        for(int lines=1;lines<=8;++lines) {
            QTextDocument doc; auto *table=makeFixture(doc,lines);
            const bool custom=qEnvironmentVariableIsSet("TABLE_REQUIRE_CLEAR_GAPS");
            if(custom) doc.setDocumentLayout(new rt::RichTextLayout(&doc));
            doc.documentLayout()->documentSize();
            const auto block=table->cellAt(0,1).firstCursorPosition().block();
            const auto rect=doc.documentLayout()->blockBoundingRect(block);
            const auto line=block.layout()->lineAt(0);
            QImage image(300,2*stride,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
            QPainter painter(&image); doc.drawContents(&painter); painter.end();
            const int ink=redPixels(image,sheet,stride);
            qInfo()<<"QPAINTER lines/pages/origin/height/gap-pixels"<<lines<<doc.pageCount()<<rect.topLeft()+line.position()<<line.height()<<ink;
            if(lines<8) { QCOMPARE(ink,0); continue; }
            QCOMPARE(doc.pageCount(),2);
            QCOMPARE(rect.y()+line.y(),custom ? 16.5 : 115.);
            QCOMPARE(line.height(),19.);
            if(!custom) QVERIFY(ink>0);
            artifact(image,"7-table-qt-painter.png");
            // Keep the Qt bug as evidence; the acceptance mode exercises our engine.
            if(qEnvironmentVariableIsSet("TABLE_REQUIRE_CLEAR_GAPS"))
                QCOMPARE(ink,0);
        }
    }
    void qtQuickMiddleAlignmentGapWitness_data() {
        QTest::addColumn<qreal>("zoom");
        QTest::newRow("zoom-1")<<qreal(1);
        QTest::newRow("zoom-1.5")<<qreal(1.5);
    }
    void qtQuickMiddleAlignmentGapWitness() {
        QFETCH(qreal,zoom);
        QQmlEngine engine; QQuickWindow window;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit { width: 300; height: 280; textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap; cursorVisible: false; color: 'black' }",QUrl());
        QScopedPointer<QQuickItem> edit(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(edit,qPrintable(component.errorString()));
        edit->setParentItem(window.contentItem()); edit->setScale(zoom);
        edit->setTransformOrigin(QQuickItem::TopLeft);
        auto *quickDoc=qvariant_cast<QQuickTextDocument *>(edit->property("textDocument")); QVERIFY(quickDoc);
        auto *doc=new QTextDocument(&engine); auto *table=makeFixture(*doc,8);
        const bool custom=qEnvironmentVariableIsSet("TABLE_REQUIRE_CLEAR_GAPS");
        if(custom) doc->setDocumentLayout(new rt::RichTextLayout(doc));
        quickDoc->setTextDocument(doc); doc->setPageSize({300,stride});
        window.setColor(Qt::white); window.resize(qCeil(300*zoom),qCeil(280*zoom)); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents();
        const auto image=window.grabWindow(); QVERIFY(!image.isNull());
        const auto block=table->cellAt(0,1).firstCursorPosition().block();
        const auto rect=doc->documentLayout()->blockBoundingRect(block);
        const int ink=redPixels(image,qCeil(sheet*zoom),qFloor(stride*zoom));
        qInfo()<<"QUICK zoom/pages/origin/gap-pixels"<<zoom<<doc->pageCount()<<rect.topLeft()<<ink;
        QCOMPARE(doc->pageCount(),2); QCOMPARE(rect.y(),custom ? 16.5 : 115.);
        if(!custom) QVERIFY(ink>0);
        artifact(image,QString("7-table-qt-quick-%1.png").arg(zoom));
        if(qEnvironmentVariableIsSet("TABLE_REQUIRE_CLEAR_GAPS")) QCOMPARE(ink,0);
    }
};
QTEST_MAIN(TablePhase2Tests)
#include "table_phase2_test.moc"
