#include "shims/richtext_layout.h"
#include "shims/richtext_odf.h"
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
        if(kind==9 || kind==11) {
            auto format=cell.format().toTableCellFormat();
            format.setVerticalAlignment(kind==9 ? QTextCharFormat::AlignMiddle : QTextCharFormat::AlignBottom);
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
        const char *names[]={"unequal","fixed","percentage","separate","auto","unbreakable","colspan","rowspan","long-rowspan","middle","simultaneous-long","bottom","boundary","empty","padding","oversized"};
        for(int k=0;k<16;++k) QTest::newRow(names[k])<<k;
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
                if(kind==9 || kind==11) {
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
