// Black-box evidence for the phase-2 acceptance conflict; no custom engine here.
#include <QTest>
#include <QTextDocument>
#include <QTextTable>
#include <QTextCursor>
#include <QTextLayout>
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
class TablePhase2Tests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QFile load("/proc/loadavg"); QVERIFY(load.open(QIODevice::ReadOnly));
        qInfo().noquote()<<"LOAD"<<load.readAll().trimmed();
    }
    void qtMiddleAlignmentGapWitness() {
        for(int lines=1;lines<=8;++lines) {
            QTextDocument doc; auto *table=makeFixture(doc,lines);
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
            QCOMPARE(rect.y()+line.y(),115.);
            QCOMPARE(line.height(),19.);
            QVERIFY(ink>0);
            artifact(image,"7-table-qt-painter.png");
            // Opt-in deliberately fails the approved acceptance requirement. The
            // default test records the Qt behavior, NOT a phase-2 parity pass.
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
        quickDoc->setTextDocument(doc); doc->setPageSize({300,stride});
        window.setColor(Qt::white); window.resize(qCeil(300*zoom),qCeil(280*zoom)); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents();
        const auto image=window.grabWindow(); QVERIFY(!image.isNull());
        const auto block=table->cellAt(0,1).firstCursorPosition().block();
        const auto rect=doc->documentLayout()->blockBoundingRect(block);
        const int ink=redPixels(image,qCeil(sheet*zoom),qFloor(stride*zoom));
        qInfo()<<"QUICK zoom/pages/origin/gap-pixels"<<zoom<<doc->pageCount()<<rect.topLeft()<<ink;
        QCOMPARE(doc->pageCount(),2); QCOMPARE(rect.y(),115.);
        QVERIFY(ink>0);
        artifact(image,QString("7-table-qt-quick-%1.png").arg(zoom));
        if(qEnvironmentVariableIsSet("TABLE_REQUIRE_CLEAR_GAPS")) QCOMPARE(ink,0);
    }
};
QTEST_MAIN(TablePhase2Tests)
#include "table_phase2_test.moc"
