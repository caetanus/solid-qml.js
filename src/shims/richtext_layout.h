#pragma once

#include <QAbstractTextDocumentLayout>
#include <QHash>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>

namespace rt {
class RichTextLayout final : public QAbstractTextDocumentLayout {
public:
    struct Counters { quint64 callbacks = 0, shapedBlocks = 0, shapedCharacters = 0,
                     placements = 0, translations = 0, convergences = 0; } counters;
    explicit RichTextLayout(QTextDocument *document);
    ~RichTextLayout() override;
    static bool supports(const QTextDocument *document);
    void forgetBlock(quint64 generation);
    void draw(QPainter *, const PaintContext &) override;
    int hitTest(const QPointF &, Qt::HitTestAccuracy) const override;
    int pageCount() const override { return m_pages; }
    QSizeF documentSize() const override { return m_size; }
    QRectF frameBoundingRect(QTextFrame *) const override;
    QRectF blockBoundingRect(const QTextBlock &) const override;
protected:
    void documentChanged(int from, int removed, int added) override;
    void drawInlineObject(QPainter *, const QRectF &, QTextInlineObject, int, const QTextFormat &) override;
    void resizeInlineObject(QTextInlineObject, int, const QTextFormat &) override;
private:
    struct Flow { qreal y = 0, bottom = 0; bool pageBreak = false; };
    struct Cache {
        QString id;
        Flow incoming, outgoing;
        QRectF rect;
        qreal right = 0, width = 0, minimum = 0, preferred = 0;
        int revision = -1;
    };
    struct Pass { bool full; int from, added; QList<QTextBlock> &changed; };
    struct Box { qreal left, width, top, bottom; };
    bool layoutBlock(QTextBlock, Flow &, const Box &, Pass &, bool first = false);
    void layoutTable(QTextTable *, Flow &, Pass &);
    QHash<QTextTable *, QRectF> m_tables;
    QHash<quint64, Cache> m_blocks;
    QSizeF m_size;
    QSizeF m_page;
    QTextFrameFormat m_root;
    bool m_widthDirty = false;
    int m_pages = 1;
};
}
