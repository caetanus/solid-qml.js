#include "richtext_page_bands.h"
#include "richtext_odf.h"
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QtMath>

namespace rt {
QList<PageBandLine> pageBandLines(QTextDocument *doc, int page, int pages)
{
    QList<PageBandLine> result;
    const auto root = doc->rootFrame()->frameFormat();
    const auto bands = root.property(kDocumentPageBandsProperty).toList();
    const qreal stride = doc->pageSize().height();
    if (bands.isEmpty() || stride <= 0 || page < 0 || page >= pages) return result;
    const auto reserved = root.property(kDocumentReservedSpaceProperty).toList();
    const auto gaps = root.property(kDocumentBandGapsProperty).toList();
    const qreal zoom = root.hasProperty(kDocumentBandZoomProperty) ? root.doubleProperty(kDocumentBandZoomProperty) : 1.;
    for (int area = 0; area < bands.size(); ++area) {
        qreal y = page * stride + (area == 0
            ? root.topMargin() - reserved.value(0).toReal() * zoom
            : stride - root.bottomMargin() + gaps.value(1).toReal() * zoom);
        for (const auto &value : bands[area].toList()) {
            const auto paragraph = value.toMap();
            QString text;
            for (const auto &run : paragraph.value("runs").toList()) {
                const auto token = run.toMap();
                const auto field = token.value("field").toString();
                text += field.isEmpty() ? token.value("text").toString()
                    : QString::number(field == QLatin1String("page-number") ? page + 1 : pages);
            }
            const auto block = scaledLengths(paragraph.value("block").value<QTextBlockFormat>(), zoom);
            const auto chars = scaledLengths(paragraph.value("char").value<QTextCharFormat>(), zoom);
            const QFont font = chars.font().resolve(doc->defaultFont());
            const QColor color = chars.foreground().style() == Qt::NoBrush ? QColor(Qt::black) : chars.foreground().color();
            const qreal x = root.leftMargin() + block.leftMargin();
            const qreal width = qMax(1., doc->pageSize().width() - root.rightMargin() - x - block.rightMargin());
            y += block.topMargin();
            QTextLayout layout(text, font);
            QTextOption option; option.setAlignment(block.alignment()); option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout.setTextOption(option);
            layout.beginLayout();
            while (true) {
                auto line = layout.createLine();
                if (!line.isValid()) break;
                line.setLineWidth(width);
                const qreal indent = line.textStart() == 0 ? block.textIndent() : 0.;
                const qreal left = x + indent + line.naturalTextRect().x();
                result.append({text.mid(line.textStart(), line.textLength()), font, color,
                    QPointF(left, y + line.ascent()), QRectF(left, y, line.naturalTextWidth(), line.height()), area});
                y += block.lineHeight(line.height(), 1.);
            }
            layout.endLayout();
            y += block.bottomMargin();
        }
    }
    return result;
}
void drawPageBandLines(QPainter *painter, const QList<PageBandLine> &lines)
{
    painter->save();
    painter->setRenderHint(QPainter::TextAntialiasing);
    for (const auto &line : lines) {
        painter->setFont(line.font); painter->setPen(line.color);
        painter->drawText(line.baseline, line.text);
    }
    painter->restore();
}
void drawPageBands(QTextDocument *doc, QPainter *painter, const QRectF &clip)
{
    if (!doc->rootFrame()->frameFormat().hasProperty(kDocumentPageBandsProperty) || doc->pageSize().height() <= 0) return;
    const int pages = doc->documentLayout()->pageCount();
    const qreal stride = doc->pageSize().height();
    const int first = clip.isEmpty() ? 0 : qMax(0, qFloor(clip.top() / stride));
    const int last = clip.isEmpty() ? pages - 1 : qMin(pages - 1, qFloor(clip.bottom() / stride));
    for (int page = first; page <= last; ++page) drawPageBandLines(painter, pageBandLines(doc, page, pages));
}
}
