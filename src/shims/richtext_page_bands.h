#pragma once

#include <QColor>
#include <QFont>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

class QTextDocument;
class QPainter;
namespace rt {
// Internal page decorations; never inserted into the editable body or its undo stack.
struct PageBandLine {
    QString text;
    QFont font;
    QColor color;
    QPointF baseline;
    QRectF rect;
    int area = 0;
    bool operator==(const PageBandLine &) const = default;
};
QList<PageBandLine> pageBandLines(QTextDocument *doc, int page, int pages);
void drawPageBandLines(QPainter *painter, const QList<PageBandLine> &lines);
void drawPageBands(QTextDocument *doc, QPainter *painter, const QRectF &clip);
}
