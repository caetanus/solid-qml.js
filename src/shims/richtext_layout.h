#pragma once

#include <QAbstractTextDocumentLayout>
#include <QHash>
#include <QFont>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>
#include <QPen>
#include <QPointer>
#include <span>

namespace rt {
class RichTextLayout final : public QAbstractTextDocumentLayout {
public:
    struct Counters { quint64 callbacks = 0, shapedBlocks = 0, shapedCharacters = 0,
                     placements = 0, translations = 0, convergences = 0, tableSolves = 0, cellReplays = 0, hitCandidates = 0, hitEntries = 0, hitRebuilds = 0; };
    mutable Counters counters;
    // Diagnostic snapshot in document/line order; never used by layout or queries.
    QList<QPair<int, QRectF>> hitIndexSnapshot() const;
    struct TableFragment {
        QRectF rect;
        QBrush background;
        QPen border;
        bool top = true, bottom = true, collapsed = true;
        QPointer<QTextTable> table;
        int row = 0, column = 0;
    };
    const QList<TableFragment> &tableFragments() const { return m_fragments; }
    void drawTableDecorations(QPainter *, const QRectF &clip, bool quick = false) const;
    explicit RichTextLayout(QTextDocument *document);
    ~RichTextLayout() override;
    static bool supports(const QTextDocument *document);
    void forgetBlock(quint64 generation);
    void draw(QPainter *, const PaintContext &) override;
    // Visible (block position, line index) pairs, including empty paragraphs and table cells.
    QList<QPair<int,int>> visibleLines(qreal top, qreal bottom) const;
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
        QList<QRectF> hitRects;
        QList<qreal> naturalHeights;
        qreal right = 0, width = 0, minimum = 0, preferred = 0;
        int revision = -1;
        quint64 hitVersion = 0;
    };
    struct Pass { bool full; int from, added; QList<QTextBlock> &changed; };
    struct Box { qreal left, width, top, bottom; };
    bool layoutBlock(QTextBlock, Flow &, const Box &, Pass &, bool first = false);
    void layoutTable(QTextTable *, Flow &, Pass &);
    QHash<QTextTable *, QRectF> m_tables;
    QHash<QTextTable *, QList<TableFragment>> m_tableFragments;
    QList<TableFragment> m_fragments;
    struct HitNode;
    HitNode *m_hitRoot = nullptr;
    quint64 m_hitTransaction = 0;
    QHash<quint64, HitNode *> m_hitBlocks;
    void removeHitBlock(quint64 lifetime);
    void insertHitBlock(const QTextBlock &, HitNode *reuse = nullptr);
    void updateHitIndex(std::span<const QTextBlock>, int from, int removed, int added,
                        int translatedFrom, qreal translatedY);
    void rebuildHitIndex();
    QHash<quint64, Cache> m_blocks;
    QHash<QFont, qreal> m_fontHeights;
    qreal documentFontHeight(const QFont &);
    QSizeF m_size;
    QSizeF m_page;
    QTextFrameFormat m_root;
    bool m_widthDirty = false;
    qreal m_inlineWidth = 0; // Current paragraph/cell width while shaping inline objects.
    int m_pages = 1;
};
}
