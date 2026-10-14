// Repeatable 96 dpi render through the same handler and custom engine as DocumentEditor.
#include "shims/richtext.h"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <private/qquicktextedit_p.h>
#include <QQuickTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextLayout>
#include <QPainter>
#include <QFontMetricsF>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc < 3) qFatal("usage: render-fidelity input.odt output-directory [--a4]");
    QQmlEngine engine; QQmlComponent component(&engine);
    component.setData("import QtQuick\nTextEdit { width: 794; textFormat: TextEdit.RichText; wrapMode: TextEdit.Wrap; color: 'black' }", {});
    QScopedPointer<QQuickTextEdit> edit(qobject_cast<QQuickTextEdit *>(component.create()));
    if (!edit) qFatal("TextEdit creation failed");
    RichTextHandler handler; handler.setDocument(edit->textDocument()); handler.setLayoutEngine("custom");
    if (!handler.loadOdf(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])))) qFatal("ODF load failed");
    qreal width=794, height=1123; QVariantList margins{94,113,94,113};
    if (argc < 4 && handler.property("documentPageWidth").toReal()>0) {
        width=handler.property("documentPageWidth").toReal(); height=handler.property("documentPageHeight").toReal();
        margins=handler.property("documentPageMargins").toList();
    }
    edit->setWidth(width); handler.configurePages(width,height,0,margins);
    auto *doc=edit->textDocument()->textDocument(); auto *layout=doc->documentLayout();
    const QFontMetricsF fontMetrics(doc->defaultFont());
    layout->documentSize(); const int pages=layout->pageCount();
    QDir output(QString::fromLocal8Bit(argv[2])); QDir().mkpath(output.path());
    QJsonArray lines;
    for (auto block=doc->begin(); block.isValid(); block=block.next()) {
        const auto origin=layout->blockBoundingRect(block).topLeft();
        for (int i=0;i<block.layout()->lineCount();++i) {
            const auto line=block.layout()->lineAt(i); const auto pos=origin+line.position();
            const int page=int(pos.y()/height);
            lines.append(QJsonObject{{"page",page+1},{"block",block.blockNumber()},{"x",pos.x()},
                {"y",pos.y()-page*height},{"baseline",pos.y()+line.ascent()-page*height},
                {"height",line.height()},{"ascent",line.ascent()},{"descent",line.descent()},{"leading",line.leading()},
                {"text",block.text().mid(line.textStart(),line.textLength())}});
        }
    }
    for (int page=0;page<pages;++page) {
        QImage image(qCeil(width),qCeil(height),QImage::Format_ARGB32_Premultiplied);
        image.setDotsPerMeterX(3780); image.setDotsPerMeterY(3780); image.fill(Qt::white);
        QPainter painter(&image); painter.translate(0,-page*height);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QAbstractTextDocumentLayout::PaintContext ctx; ctx.clip=QRectF(0,page*height,width,height);
        layout->draw(&painter,ctx); painter.end();
        if (!image.save(output.filePath(QString("office-%1.png").arg(page+1)))) qFatal("PNG write failed");
    }
    QFile file(output.filePath("office.json")); if (!file.open(QIODevice::WriteOnly)) return 1;
    file.write(QJsonDocument(QJsonObject{{"pages",pages},{"width",width},{"height",height},{"lines",lines},
        {"fontHeight",fontMetrics.height()},{"fontLeading",fontMetrics.leading()},{"fontSpacing",fontMetrics.lineSpacing()}}).toJson());
}
