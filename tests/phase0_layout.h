#pragma once
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextTable>
#include <QTextCursor>
#include <QTextLayout>
#include <QPainter>
#include <QtMath>
// Original, deliberately incomplete paragraph feasibility spike. No general table flow solver,
// compatibility margin rules, convergence engine, or production integration.
class ProbeLayout : public QAbstractTextDocumentLayout {
public:
    struct Counts { int callbacks=0, blocks=0, characters=0, placements=0; mutable int sizes=0, rects=0; } counts;
    qreal width=300, sheet=200, gap=20, margin=10;
    bool full=true;
    QList<QRectF> tableFragments; // test-only, one two-cell row
    QSizeF extent;
    int pages=1;
    explicit ProbeLayout(QTextDocument *d) : QAbstractTextDocumentLayout(d) {}
    void configure(qreal w, qreal h, qreal g) {
        width=w; sheet=h; gap=g; full=true;
        document()->markContentsDirty(0, document()->characterCount());
    }
    QSizeF documentSize() const override { ++counts.sizes; return extent; }
    int pageCount() const override { return pages; }
    QRectF blockBoundingRect(const QTextBlock &b) const override {
        ++counts.rects;
        return b.isValid() ? b.layout()->boundingRect().translated(b.layout()->position()) : QRectF();
    }
    QRectF frameBoundingRect(QTextFrame *) const override { return QRectF({},extent); }
    int hitTest(const QPointF &p, Qt::HitTestAccuracy accuracy) const override {
        qreal nearest=1e30; int result=-1;
        for (auto b=document()->begin(); b.isValid(); b=b.next()) {
            auto *l=b.layout();
            for (int i=0;i<l->lineCount();++i) {
                auto line=l->lineAt(i); auto r=line.rect().translated(l->position());
                if (accuracy==Qt::ExactHit && !r.contains(p)) continue;
                qreal distance=qAbs(r.center().y()-p.y());
                if(distance<nearest) { nearest=distance; result=b.position()+line.xToCursor(p.x()-l->position().x()); }
            }
        }
        return result;
    }
    void draw(QPainter *p, const PaintContext &ctx) override {
        for(auto b=document()->begin();b.isValid();b=b.next()) {
            if(ctx.clip.isEmpty() || ctx.clip.intersects(blockBoundingRect(b))) b.layout()->draw(p,b.layout()->position());
        }
    }
protected:
    void resizeInlineObject(QTextInlineObject obj, int pos, const QTextFormat &f) override {
        if(auto *handler=handlerForObject(f.objectType())) {
            const auto size=handler->intrinsicSize(document(),pos,f);
            obj.setWidth(size.width()); obj.setAscent(size.height()); obj.setDescent(0);
        }
    }
    void documentChanged(int from, int removed, int added) override {
        ++counts.callbacks;
        qreal y=margin;
        QTextTable *row=nullptr;
        for(auto *frame:document()->rootFrame()->childFrames()) {
            auto *table=qobject_cast<QTextTable *>(frame);
            if(table && table->rows()==1 && table->columns()==2) row=table;
        }
        qreal rowEnd=margin;
        tableFragments.clear();
        for(auto b=document()->begin();b.isValid();b=b.next()) {
            auto *l=b.layout();
            const bool cell=row && QTextCursor(b).currentTable()==row;
            const int column=cell ? row->cellAt(b.position()).column() : 0;
            if(cell) y=margin;
            else if(row && b.position()>row->lastPosition()) y=rowEnd;
            const qreal available=(width-2*margin)/(cell ? 2 : 1);
            bool dirty=full || !l->lineCount() || (b.position()<=from+qMax(removed,added) && b.position()+b.length()>from);
            if(dirty) {
                ++counts.blocks; counts.characters+=b.length();
                auto option=document()->defaultTextOption();
                option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
                option.setAlignment(b.blockFormat().alignment()); option.setTextDirection(b.textDirection());
                l->setTextOption(option); l->setCacheEnabled(true);
                l->beginLayout();
                while(true) { auto line=l->createLine(); if(!line.isValid()) break; line.setLineWidth(available); }
                l->endLayout(); b.setLineCount(l->lineCount());
            }
            // Intentionally count an O(blocks) placement replay. Phase 0 makes no convergence claim.
            ++counts.placements;
            qreal origin=y;
            for(int i=0;i<l->lineCount();++i) {
                auto line=l->lineAt(i);
                int page=qFloor(y/(sheet+gap));
                if(y+line.height()>page*(sheet+gap)+sheet-margin) y=(page+1)*(sheet+gap)+margin;
                if(i==0) origin=y;
                line.setPosition({0,y-origin}); y+=line.height();
            }
            l->setPosition({margin+column*available,origin});
            if(cell) rowEnd=qMax(rowEnd,y);
        }
        if(row) {
            const qreal half=(width-2*margin)/2;
            for(qreal top=margin;top<rowEnd;top+=sheet+gap) {
                const qreal bottom=qMin(rowEnd,top+sheet-2*margin);
                for(int col=0;col<2;++col) tableFragments.append(QRectF(margin+col*half,top,half,bottom-top));
            }
        }
        full=false;
        pages=qMax(1,int(qCeil((y+margin)/(sheet+gap))));
        const QSizeF next(width,pages*(sheet+gap));
        const bool resized=next!=extent; extent=next;
        // Conservative invalidation is intentional here; no bounded repaint claim.
        for(auto b=document()->begin();b.isValid();b=b.next()) emit updateBlock(b);
        emit update();
        if(resized) { emit documentSizeChanged(extent); emit pageCountChanged(pages); }
    }
};
