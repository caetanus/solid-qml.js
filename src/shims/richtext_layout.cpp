#include "richtext_layout.h"
#include "richtext_blocks.h"
#include "richtext_odf.h"

#include <QPainter>
#include <QFontMetricsF>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QTextList>
#include <QtMath>
#include <limits>
#include <algorithm>
#include <vector>

namespace rt {
// A Cartesian tree in document order. Each node owns one already ordered line
// run. Subtree bounds also cover out-of-order table cells, so spatial pruning
// does not depend on document order matching visual order.
struct RichTextLayout::HitNode {
    HitNode *left=nullptr, *right=nullptr, *parent=nullptr;
    quint64 lifetime=0, priority=0, version=0, update=0;
    int position=0, pendingPosition=0;
    qreal pendingY=0;
    QPointF origin;
    QRectF bounds, ownBounds;
    std::vector<QRectF> lines;
    std::vector<qreal> bottoms;
    ~HitNode() { delete left; delete right; }
    static void shift(HitNode *n,int positions,qreal y) {
        if (!n) return;
        n->position+=positions; n->pendingPosition+=positions; n->pendingY+=y;
        n->origin.ry()+=y; n->bounds.translate(0,y); n->ownBounds.translate(0,y);
    }
    static void push(HitNode *n) {
        if (!n) return;
        shift(n->left,n->pendingPosition,n->pendingY); shift(n->right,n->pendingPosition,n->pendingY);
        n->pendingPosition=0; n->pendingY=0;
    }
    static void expose(HitNode *n) {
        if(n->parent) expose(n->parent);
        push(n);
    }
    static QRectF united(const QRectF &a,const QRectF &b) {
        // QRectF::united drops zero-width empty-paragraph rectangles.
        return QRectF(QPointF(qMin(a.left(),b.left()),qMin(a.top(),b.top())),
                      QPointF(qMax(a.right(),b.right()),qMax(a.bottom(),b.bottom())));
    }
    static void pull(HitNode *n) {
        n->bounds=n->ownBounds;
        for(auto *child:{n->left,n->right}) if(child) {
            child->parent=n; n->bounds=united(n->bounds,child->bounds);
        }
    }
    static HitNode *merge(HitNode *a,HitNode *b) {
        if(!a || !b) { auto *n=a ? a : b; if(n) n->parent=nullptr; return n; }
        if(a->priority>b->priority) {
            push(a); a->right=merge(a->right,b); pull(a); a->parent=nullptr; return a;
        }
        push(b); b->left=merge(a,b->left); pull(b); b->parent=nullptr; return b;
    }
    static void split(HitNode *n,int position,HitNode *&a,HitNode *&b) {
        if(!n) { a=b=nullptr; return; }
        push(n);
        if(n->position<position) { a=n; split(n->right,position,n->right,b); pull(a); a->parent=nullptr; }
        else { b=n; split(n->left,position,a,n->left); pull(b); b->parent=nullptr; }
    }
    static HitNode *erase(HitNode *n,int position,HitNode *&removed) {
        if(!n) return nullptr;
        push(n);
        if(n->position==position) {
            auto *joined=merge(n->left,n->right); n->left=n->right=n->parent=nullptr; removed=n; return joined;
        }
        if(position<n->position) n->left=erase(n->left,position,removed);
        else n->right=erase(n->right,position,removed);
        pull(n); n->parent=nullptr; return n;
    }
};

// The public oracle rounds format lengths to 1/64 px before width allocation.
static qreal quantizedLength(qreal value) { return qint64(value*64)/64.; }
RichTextLayout::RichTextLayout(QTextDocument *d) : QAbstractTextDocumentLayout(d) {}
RichTextLayout::~RichTextLayout()
{
    delete m_hitRoot;
    for (auto b = document()->begin(); b.isValid(); b = b.next())
        if (auto *info = blockInfo(b); info && info->layoutOwner == this) info->layoutOwner = nullptr;
}
bool RichTextLayout::supports(const QTextDocument *d)
{
    if (!d) return false;
    for (auto *frame : d->rootFrame()->childFrames()) {
        auto *table=qobject_cast<QTextTable *>(frame);
        if (!table || !table->childFrames().isEmpty()) return false;
        const auto format=table->format();
        if (format.headerRowCount() || format.position()!=QTextFrameFormat::InFlow) return false;
        if (format.border()>0 && format.borderStyle()!=QTextFrameFormat::BorderStyle_Solid) return false;
    }
    const auto root = d->rootFrame()->frameFormat();
    if (root.border() != 0 || root.padding() != 0 || root.background().style() != Qt::NoBrush) return false;
    for (auto b = d->begin(); b.isValid(); b = b.next())
        if (!b.isVisible() || b.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) return false;
    return true;
}
void RichTextLayout::forgetBlock(quint64 generation)
{
    removeHitBlock(generation);
    const auto it = m_blocks.constFind(generation);
    if (it != m_blocks.cend() && it->right > m_page.width() && it->right >= m_size.width()) m_widthDirty = true;
    m_blocks.remove(generation);
}
QRectF RichTextLayout::frameBoundingRect(QTextFrame *frame) const
{
    return frame == document()->rootFrame() ? QRectF({}, m_size) : m_tables.value(qobject_cast<QTextTable *>(frame));
}
QRectF RichTextLayout::blockBoundingRect(const QTextBlock &b) const
{
    if (!b.isValid()) return {};
    const auto *info = blockInfo(b);
    if (!info) return {};
    const auto it = m_blocks.constFind(info->lifetime);
    return it == m_blocks.cend() ? QRectF() : it->rect;
}
void RichTextLayout::drawInlineObject(QPainter *painter, const QRectF &rect, QTextInlineObject object, int position, const QTextFormat &format)
{
    if (handlerForObject(format.objectType())) {
        QAbstractTextDocumentLayout::drawInlineObject(painter, rect, object, position, format);
    } else if (format.isImageFormat()) {
        const auto resource = document()->resource(QTextDocument::ImageResource, QUrl(format.toImageFormat().name()));
        painter->drawImage(rect, qvariant_cast<QImage>(resource));
    }
}
void RichTextLayout::resizeInlineObject(QTextInlineObject object, int position, const QTextFormat &format)
{
    QSizeF size;
    if (auto *handler = handlerForObject(format.objectType())) {
        size = handler->intrinsicSize(document(), position, format);
    } else if (format.isImageFormat()) {
        const auto imageFormat = format.toImageFormat();
        const auto resource = document()->resource(QTextDocument::ImageResource, QUrl(imageFormat.name()));
        const QImage image = qvariant_cast<QImage>(resource);
        size = image.size() / image.devicePixelRatio();
        if (imageFormat.width() > 0) size.setWidth(imageFormat.width());
        if (imageFormat.height() > 0) size.setHeight(imageFormat.height());
    }
    const qreal height = qMax(0., size.height());
    qreal ascent = height, descent = 0;
    const auto character = format.toCharFormat();
    // Public Qt oracle: baseline objects share the font descent; middle objects
    // split around half-height plus one quarter of the font's x-height.
    if (character.verticalAlignment() == QTextCharFormat::AlignBaseline
        || character.verticalAlignment() == QTextCharFormat::AlignMiddle) {
        const QFontMetricsF metrics(character.font().resolve(document()->defaultFont()));
        if (character.verticalAlignment() == QTextCharFormat::AlignBaseline) {
            descent = metrics.descent(); ascent = height - descent;
        } else {
            ascent = height/2 + metrics.xHeight()/4; descent = height - ascent;
        }
    }
    object.setWidth(qMax(0., size.width()));
    object.setAscent(ascent);
    object.setDescent(descent);
}
bool RichTextLayout::layoutBlock(QTextBlock b, Flow &flow, const Box &box, Pass &pass, bool first)
{
    auto *doc = document();
    const qreal stride = m_page.height() > 0 ? m_page.height() : 1e12;
    const qreal top = box.top, bottom = box.bottom;
    auto *info = blockInfo(b);
    if (!info) { info = new BlockInfo(QString()); b.setUserData(info); }
    info->layoutOwner = this;
    auto it = m_blocks.find(info->lifetime);
    const bool known = it != m_blocks.end() && it->id == info->id;
    const Cache old = known ? *it : Cache();
    auto *layout = b.layout();
    const bool dirty = pass.full || old.width != box.width || !known || !layout->lineCount() || old.revision != b.revision()
        || (b.position() <= pass.from + pass.added && b.position() + b.length() > pass.from);
    const auto format = b.blockFormat();
    const auto direction = b.textDirection();
    const auto *list = b.textList();
    const int indent = format.indent() + (list ? list->format().indent() : 0);
    const qreal left = box.left;
    const qreal available = qMax(1., box.width - quantizedLength(format.leftMargin()) - quantizedLength(format.rightMargin()) - indent * doc->indentWidth());
    if (dirty) {
        ++counters.shapedBlocks; counters.shapedCharacters += b.length();
        auto option = doc->defaultTextOption();
        if (format.nonBreakableLines()) option.setWrapMode(QTextOption::NoWrap);
        auto alignment = format.alignment();
        if (!alignment.testFlag(Qt::AlignAbsolute) && direction == Qt::RightToLeft) {
            if (alignment.testFlag(Qt::AlignLeft)) {
                alignment.setFlag(Qt::AlignLeft, false); alignment.setFlag(Qt::AlignRight);
            } else if (alignment.testFlag(Qt::AlignRight)) {
                alignment.setFlag(Qt::AlignRight, false); alignment.setFlag(Qt::AlignLeft);
            }
        }
        option.setAlignment(alignment | Qt::AlignAbsolute);
        option.setTextDirection(direction);
        if (format.hasProperty(QTextFormat::TabPositions)) option.setTabs(format.tabPositions());
        layout->setTextOption(option);
        // Keep line records; let Qt release glyph buffers after drawing.
        layout->setCacheEnabled(false);
        layout->beginLayout();
        for (int index = 0;; ++index) {
            auto line = layout->createLine(); if (!line.isValid()) break;
            line.setLineWidth(qMax(1., available - (index == 0 ? format.textIndent() : 0)));
            if (format.nonBreakableLines() && line.naturalTextWidth() > line.width())
                line.setLineWidth(line.naturalTextWidth());
        }
        layout->endLayout(); b.setLineCount(layout->lineCount());
    }
    ++counters.placements;
    Cache next; next.id = info->id; next.revision = b.revision(); next.incoming = flow;
    if (flow.pageBreak)
        flow.y = (qFloor(flow.y / stride) + 1) * stride + top;
    if (format.pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysBefore))
        flow.y = (qFloor(flow.y / stride) + 1) * stride + top;
    if (!first) {
        const qreal beforeSpacing = flow.y;
        if (m_root.boolProperty(kDocumentSpacingProperty)) {
            flow.y += flow.bottom + quantizedLength(format.topMargin());
            // Natural page starts discard inter-paragraph space, including a gap
            // large enough to cross the page boundary by itself.
            if (!flow.pageBreak && !format.pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysBefore)
                && qFloor(flow.y / stride) > qFloor(beforeSpacing / stride))
                flow.y = (qFloor(beforeSpacing / stride) + 1) * stride + top;
        } else {
            flow.y += qMax(flow.bottom, quantizedLength(format.topMargin()));
        }
    }
    const qreal origin = flow.y;
    bool sameLines = known && !dirty;
    next.hitRects.reserve(layout->lineCount());
    for (int index = 0; index < layout->lineCount(); ++index) {
        auto line = layout->lineAt(index);
        const qreal natural = line.height();
        const qreal advance = format.lineHeight(natural + (format.lineHeightType() == QTextBlockFormat::MinimumHeight ? line.leading() : 0), 1);
        qreal offset = 0;
        if (format.lineHeightType() == QTextBlockFormat::FixedHeight) offset = advance * .8 - line.ascent();
        else if (format.lineHeightType() == QTextBlockFormat::MinimumHeight) offset = advance - natural;
        const qreal pageTop = qFloor(flow.y / stride) * stride + top;
        flow.y = qMax(flow.y, pageTop);
        if (flow.y + (format.lineHeightType() == QTextBlockFormat::LineDistanceHeight ? natural : qMax(advance, natural)) > qFloor(flow.y / stride)*stride + stride-bottom && flow.y > pageTop)
            flow.y = (qFloor(flow.y / stride)+1)*stride + top;
        const QPointF position(quantizedLength(format.leftMargin()) + (direction == Qt::RightToLeft ? 0 : indent * doc->indentWidth() + (index == 0 ? format.textIndent() : 0)), flow.y-origin+offset);
        sameLines &= line.position() == position;
        line.setPosition(position);
        next.hitRects.append(line.naturalTextRect());
        flow.y += advance;
    }
    layout->setPosition({left, origin});
    flow.bottom = quantizedLength(format.bottomMargin());
    flow.pageBreak = format.pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysAfter);
    // Qt publishes the layout origin plus its bounding *size*: a first-line
    // page jump is not included in that size. Painting uses the translated
    // line bounds instead, so this compatibility rectangle never clips ink.
    next.outgoing = flow; next.rect = QRectF(QPointF(left, origin), layout->boundingRect().size());
    next.right = left + layout->boundingRect().right() + m_root.rightMargin();
    next.width = box.width;
    next.minimum = dirty ? layout->minimumWidth() : old.minimum;
    next.preferred = dirty ? layout->maximumWidth() : old.preferred;
    if (known && old.right >= m_size.width() && next.right < old.right) m_widthDirty = true;
    next.hitVersion=old.hitVersion+(!known || dirty || !sameLines);
    m_blocks.insert(info->lifetime, next);
    pass.changed.append(b);
    return sameLines;
}
void RichTextLayout::layoutTable(QTextTable *table, Flow &flow, Pass &pass)
{
    ++counters.tableSolves;
    struct Cell { QTextTableCell cell; qreal preferred = 0, minimum = 0, height = 0, left = 0, right = 0, top = 0, bottom = 0; };
    const auto format = table->format();
    const qreal stride = m_page.height();
    const qreal padding = format.cellPadding(), border = format.border();
    const qreal spacing = format.borderCollapse() ? 0 : format.cellSpacing();
    const qreal edge = 2*border + padding + (format.borderCollapse() ? border/2 : spacing);
    const qreal between = 2*padding + (format.borderCollapse() ? border : 2*border+spacing);
    const qreal outerWidth = m_page.width()-m_root.leftMargin()-m_root.rightMargin();
    const int columns = table->columns(), rows = table->rows();
    QList<Cell> cells;
    QVector<qreal> preferred(columns), minimum(columns), widths(columns), heights(rows);
    for (int row=0;row<rows;++row) for (int column=0;column<columns;++column) {
        const auto cell = table->cellAt(row,column);
        if (cell.row()!=row || cell.column()!=column) continue;
        ++counters.cellReplays;
        Cell entry{cell};
        for (auto it=cell.begin();it!=cell.end();++it) {
            auto b=it.currentBlock(); if (!b.isValid()) continue;
            const auto *info=blockInfo(b);
            const auto cached=info ? m_blocks.constFind(info->lifetime) : m_blocks.cend();
            if (pass.full || cached==m_blocks.cend() || cached->revision!=b.revision() || !b.layout()->lineCount()
                || (b.position()<=pass.from+pass.added && b.position()+b.length()>pass.from)) {
                Flow measure;
                layoutBlock(b,measure,{0,1e8,0,0},pass,true);
            }
            const auto bf=b.blockFormat();
            const qreal inset=quantizedLength(bf.leftMargin())+quantizedLength(bf.rightMargin())+quantizedLength(bf.textIndent())+bf.indent()*document()->indentWidth();
            const auto &metrics=m_blocks[blockInfo(b)->lifetime];
            entry.preferred=qMax(entry.preferred,qCeil(metrics.preferred+inset)*1.);
            entry.minimum=qMax(entry.minimum,qCeil(metrics.minimum+inset)*1.);
        }
        const auto cf=cell.format().toTableCellFormat();
        entry.left=cf.hasProperty(QTextFormat::TableCellLeftPadding) ? cf.leftPadding()-padding : 0;
        entry.right=cf.hasProperty(QTextFormat::TableCellRightPadding) ? cf.rightPadding()-padding : 0;
        entry.top=cf.hasProperty(QTextFormat::TableCellTopPadding) ? cf.topPadding()-padding : 0;
        entry.bottom=cf.hasProperty(QTextFormat::TableCellBottomPadding) ? cf.bottomPadding()-padding : 0;
        entry.preferred+=entry.left+entry.right; entry.minimum+=entry.left+entry.right;
        cells.append(entry);
        if (cell.columnSpan()==1) {
            preferred[column]=qMax(preferred[column],entry.preferred);
            minimum[column]=qMax(minimum[column],entry.minimum);
        }
    }
    for (const auto &entry:cells) if (entry.cell.columnSpan()>1) {
        const int first=entry.cell.column(), count=entry.cell.columnSpan();
        qreal havePreferred=(count-1)*between, haveMinimum=havePreferred;
        for (int col=first;col<first+count;++col) { havePreferred+=preferred[col]; haveMinimum+=minimum[col]; }
        for (int col=first;col<first+count;++col) {
            preferred[col]+=qMax(0.,entry.preferred-havePreferred)/count;
            minimum[col]+=qMax(0.,entry.minimum-haveMinimum)/count;
        }
    }
    qreal intrinsic=2*edge+(columns-1)*between;
    for (qreal value:preferred) intrinsic+=value;
    qreal tableWidth=format.width().type()==QTextLength::VariableLength ? qMin(outerWidth,intrinsic) : format.width().value(outerWidth);
    const qreal contentWidth=qMax(1.,tableWidth-2*edge-(columns-1)*between);
    const auto constraints=format.columnWidthConstraints();
    QVector<bool> fixed(columns,false);
    qreal remaining=contentWidth; int flexible=columns;
    for (int col=0;col<columns && col<constraints.size();++col) {
        if (constraints[col].type()==QTextLength::VariableLength) continue;
        widths[col]=qMax(minimum[col],qRound(constraints[col].value(contentWidth+columns*between))-between);
        fixed[col]=true; remaining-=widths[col]; --flexible;
    }
    qreal preferredSum=0;
    for (int col=0;col<columns;++col) if(!fixed[col]) preferredSum+=preferred[col];
    if (preferredSum<=remaining && flexible) {
        const qreal extra=(remaining-preferredSum)/flexible;
        qreal boundary=0;
        for (int col=0;col<columns;++col) {
            if (fixed[col]) { boundary+=widths[col]; continue; }
            const qreal previous=qRound(boundary);
            boundary+=preferred[col]+extra;
            widths[col]=qRound(boundary)-previous; fixed[col]=true;
            remaining-=widths[col]; --flexible;
        }
    }
    for (bool progress=true; progress && flexible; ) {
        progress=false;
        for (int col=0;col<columns;++col) if (!fixed[col] && preferred[col]<=remaining/flexible) {
            widths[col]=qMax(minimum[col],preferred[col]); fixed[col]=true;
            remaining-=widths[col]; --flexible; progress=true;
        }
    }
    for (int col=0;col<columns;++col) if (!fixed[col]) {
        widths[col]=qMax(minimum[col],qFloor(remaining/flexible)*1.);
        remaining-=widths[col]; --flexible;
    }
    Pass settled{false,-1,-1,pass.changed};
    // Measure at solved widths before assigning row/span lower bounds.
    for (auto &entry:cells) {
        const auto cell=entry.cell;
        qreal cellWidth=(cell.columnSpan()-1)*between;
        for (int col=cell.column();col<cell.column()+cell.columnSpan();++col) cellWidth+=widths[col];
        Flow measure;
        bool first=true;
        for (auto it=cell.begin();it!=cell.end();++it) {
            auto b=it.currentBlock(); if (!b.isValid()) continue;
            layoutBlock(b,measure,{0,cellWidth-entry.left-entry.right,0,-1e9},settled,first); first=false;
        }
        entry.height=measure.y+entry.top+entry.bottom;
        if (cell.rowSpan()==1) heights[cell.row()]=qMax(heights[cell.row()],entry.height);
    }
    for (const auto &entry:cells) if (entry.cell.rowSpan()>1) {
        const auto cell=entry.cell; qreal have=(cell.rowSpan()-1)*between;
        for (int row=cell.row();row<cell.row()+cell.rowSpan();++row) have+=heights[row];
        heights[cell.row()+cell.rowSpan()-1]+=qMax(0.,entry.height-have);
    }
    QVector<qreal> spanEnds(rows), rowStarts(rows), rowEnds(rows);
    qreal y=flow.y+edge;
    qreal tableTop=y;
    for (int row=0;row<rows;++row) {
        const qreal textTop=qFloor(y/stride)*stride+m_root.topMargin()+edge;
        const qreal textBottom=(qFloor(y/stride)+1)*stride-m_root.bottomMargin()-edge;
        const qreal capacity=stride-m_root.topMargin()-m_root.bottomMargin()-2*edge;
        if (heights[row]<=capacity && y+heights[row]>textBottom && y>textTop) {
            if (row>0) rowEnds[row-1]=qMax(rowEnds[row-1],textBottom);
            y=(qFloor(y/stride)+1)*stride+m_root.topMargin()+edge;
        }
        if (row==0) tableTop=y;
        rowStarts[row]=y;
        qreal end=y+heights[row];
        for (const auto &entry:cells) if (entry.cell.row()==row) {
            const auto cell=entry.cell;
            qreal x=m_root.leftMargin()+edge, cellWidth=(cell.columnSpan()-1)*between;
            for (int col=0;col<cell.column();++col) x+=widths[col]+between;
            for (int col=cell.column();col<cell.column()+cell.columnSpan();++col) cellWidth+=widths[col];
            Flow cellFlow{y+entry.top,0,false}; bool first=true;
            for (auto it=cell.begin();it!=cell.end();++it) {
                auto b=it.currentBlock(); if (!b.isValid()) continue;
                layoutBlock(b,cellFlow,{x+entry.left,cellWidth-entry.left-entry.right,m_root.topMargin()+edge,m_root.bottomMargin()+edge},settled,first); first=false;
            }
            if(cell.rowSpan()==1) end=qMax(end,cellFlow.y+entry.bottom);
            else spanEnds[row+cell.rowSpan()-1]=qMax(spanEnds[row+cell.rowSpan()-1],cellFlow.y+entry.bottom);
        }
        end=qMax(end,spanEnds[row]);
        // A split row deliberately ignores middle/bottom alignment.
        if (qFloor(y/stride)==qFloor((end-.01)/stride)) {
            for (const auto &entry:cells) if (entry.cell.row()==row && entry.cell.rowSpan()==1) {
                const auto alignment=entry.cell.format().verticalAlignment();
                qreal delta=alignment==QTextCharFormat::AlignMiddle ? (end-y-entry.height)/2
                    : alignment==QTextCharFormat::AlignBottom ? end-y-entry.height : 0;
                if (delta==0) continue;
                for (auto it=entry.cell.begin();it!=entry.cell.end();++it) {
                    auto b=it.currentBlock(); if (!b.isValid()) continue;
                    auto &cache=m_blocks[blockInfo(b)->lifetime]; cache.rect.translate(0,delta);
                    b.layout()->setPosition(cache.rect.topLeft());
                }
            }
        }
        rowEnds[row]=end;
        y=end+(row+1<rows ? between : edge);
    }
    if (!m_tables.contains(table)) connect(table,&QObject::destroyed,this,[this,table] {
        m_tables.remove(table); m_tableFragments.remove(table);
    });
    // Qt exposes the first cell's aligned content origin, with the outer size.
    const auto firstBlock=cells.first().cell.firstCursorPosition().block();
    m_tables.insert(table,QRectF(firstBlock.layout()->position(),QSizeF(tableWidth,y-(tableTop-edge))));
    auto &fragments=m_tableFragments[table]; fragments.clear();
    QPen pen(format.borderBrush(),border);
    if (border<=0 || format.borderStyle()==QTextFrameFormat::BorderStyle_None) pen.setStyle(Qt::NoPen);
    if (!format.borderCollapse()) {
        fragments.append({{m_root.leftMargin(),tableTop-edge,tableWidth-border,y-(tableTop-edge)-border},format.background(),pen,true,true,false});
    }
    for (const auto &entry:cells) {
        const auto cell=entry.cell;
        qreal x=m_root.leftMargin()+edge-padding-border, cellWidth=(cell.columnSpan()-1)*between+2*padding+border;
        for (int col=0;col<cell.column();++col) x+=widths[col]+between;
        for (int col=cell.column();col<cell.column()+cell.columnSpan();++col) cellWidth+=widths[col];
        const qreal first=rowStarts[cell.row()]-padding-border;
        const qreal last=rowEnds[cell.row()+cell.rowSpan()-1]+padding;
        for (int page=qFloor(first/stride);page<=qFloor(last/stride);++page) {
            const qreal pageTop=page*stride+m_root.topMargin()+border;
            const qreal pageBottom=(page+1)*stride-m_root.bottomMargin()-border;
            const qreal fragmentTop=qMax(first,pageTop), fragmentBottom=qMin(last,pageBottom);
            if (fragmentBottom<fragmentTop) continue;
            const auto background=cell.format().background().style()==Qt::NoBrush ? format.background() : cell.format().background();
            fragments.append({{x,fragmentTop,cellWidth,fragmentBottom-fragmentTop},background,pen,first>=pageTop,last<=pageBottom,format.borderCollapse(),table,cell.row(),cell.column()});
        }
    }
    flow={y,format.bottomMargin(),false};
}
void RichTextLayout::documentChanged(int from, int removed, int added)
{
    ++counters.callbacks;
    auto *doc = document();
    // setDocumentLayout can notify during a suspended load. The enabling
    // transaction will provide the complete dirty range and publish geometry once.
    if (!doc->isLayoutEnabled()) return;
    const auto page = doc->pageSize();
    auto root = doc->rootFrame()->frameFormat();
    root.setTopMargin(qRound(root.topMargin())); root.setBottomMargin(qRound(root.bottomMargin()));
    root.setLeftMargin(qRound(root.leftMargin())); root.setRightMargin(qRound(root.rightMargin()));
    const bool full = m_blocks.isEmpty() || page != m_page || root != m_root
        || (from == 0 && added >= doc->characterCount()-1);
    m_page = page; m_root = root;
    const qreal stride = page.height() > 0 ? page.height() : 1e12;
    const qreal width = page.width() > 0 ? page.width() : 640;
    const qreal top = root.topMargin(), bottom = root.bottomMargin();
    auto b = full ? doc->begin() : doc->findBlock(from);
    if (!b.isValid()) b = doc->lastBlock();
    if (!full) {
        if (auto *table=QTextCursor(b).currentTable()) b=doc->findBlock(table->firstPosition());
        if (b.previous().isValid()) b=b.previous();
        if (auto *table=QTextCursor(b).currentTable()) b=doc->findBlock(table->firstPosition());
    }
    Flow flow{top, 0, false};
    if (b.previous().isValid()) {
        const auto *previous = blockInfo(b.previous());
        const auto it = previous ? m_blocks.constFind(previous->lifetime) : m_blocks.cend();
        if (it != m_blocks.cend()) flow = it->outgoing;
        else b = doc->begin();
    }
    qreal documentWidth = full ? width : qMax(width, m_size.width());
    const QSizeF oldSize = m_size;
    const int oldPages = m_pages;
    QList<QTextBlock> changed;
    int translatedFrom=-1; qreal translatedY=0;
    qsizetype hitChangedCount=-1;
    Pass pass{full, from, added, changed}; // Handles are transaction-local.
    for (; b.isValid(); b = b.next()) {
        if (auto *table=QTextCursor(b).currentTable()) {
            layoutTable(table,flow,pass);
            b=doc->findBlock(table->lastPosition());
            continue;
        }
        auto *info = blockInfo(b);
        const auto previous = info ? m_blocks.constFind(info->lifetime) : m_blocks.cend();
        const bool known = previous != m_blocks.cend();
        const Cache old = known ? *previous : Cache();
        const Flow beforeBlock = flow;
        const bool sameLines = layoutBlock(b, flow, {root.leftMargin(), width-root.leftMargin()-root.rightMargin(), top, bottom}, pass, b == doc->begin() || (b.previous().isValid() && QTextCursor(b.previous()).currentTable()));
        info = blockInfo(b);
        if (b.text().isEmpty() && b.previous().isValid() && QTextCursor(b.previous()).currentTable()) {
            auto &cache=m_blocks[info->lifetime];
            cache.rect.translate(width-root.rightMargin()-cache.rect.x(),-b.layout()->lineAt(0).height());
            b.layout()->setPosition(cache.rect.topLeft());
            flow=beforeBlock; cache.outgoing=flow;
        }
        Cache next = m_blocks.value(info->lifetime);
        documentWidth = qMax(documentWidth, next.right);
        if (b.text().isEmpty() && b.next().isValid() && QTextCursor(b.next()).currentTable()) {
            flow.y=next.rect.y(); next.outgoing=flow;
            m_blocks[info->lifetime]=next;
        }

        const qreal delta = next.rect.y() - old.rect.y();
        const bool wholePage = delta == qRound64(delta/stride)*stride;
        if (!full && b.position() > from+added && sameLines && wholePage
            && next.incoming.y-old.incoming.y == delta && next.outgoing.y-old.outgoing.y == delta
            && next.incoming.bottom == old.incoming.bottom && next.incoming.pageBreak == old.incoming.pageBreak
            && next.outgoing.bottom == old.outgoing.bottom && next.outgoing.pageBreak == old.outgoing.pageBreak
            && next.rect.x() == old.rect.x() && next.rect.size() == old.rect.size()) {
            ++counters.convergences;
            if (delta != 0) {
                if(b.next().isValid()) {
                    translatedFrom=b.next().position(); translatedY=delta;
                    hitChangedCount=changed.size();
                }
                for (auto tail = b.next(); tail.isValid(); tail = tail.next()) {
                    auto *tailInfo = blockInfo(tail);
                    auto &cache = m_blocks[tailInfo->lifetime];
                    cache.rect.translate(0, delta); cache.incoming.y += delta; cache.outgoing.y += delta;
                    tail.layout()->setPosition(cache.rect.topLeft());
                    ++counters.translations; pass.changed.append(tail);
                }
            }
            if (delta!=0) for (auto *frame:doc->rootFrame()->childFrames()) {
                auto *table=qobject_cast<QTextTable *>(frame);
                if (!table || table->firstPosition()<=b.position()) continue;
                m_tables[table].translate(0,delta);
                for (auto &fragment:m_tableFragments[table]) fragment.rect.translate(0,delta);
            }
            if (auto *last = blockInfo(doc->lastBlock())) flow = m_blocks.value(last->lifetime).outgoing;
            break;
        }
    }
    Q_UNUSED(removed);
    const qreal end = flow.pageBreak ? (qFloor(flow.y/stride)+1)*stride+top : flow.y;
    m_pages = qMax(1, int(qCeil((end+bottom)/stride)));
    if (m_widthDirty) {
        documentWidth = width;
        for (auto it = m_blocks.cbegin(); it != m_blocks.cend(); ++it) documentWidth = qMax(documentWidth, it->right);
        m_widthDirty = false;
    }
    m_size = QSizeF(documentWidth, page.height() > 0 ? m_pages*stride : flow.y+bottom);
    m_fragments.clear();
    for (auto *frame:doc->rootFrame()->childFrames()) {
        auto *table=qobject_cast<QTextTable *>(frame);
        const auto found=m_tableFragments.constFind(table);
        if (found!=m_tableFragments.cend()) m_fragments.append(*found);
    }
    if (full) rebuildHitIndex();
    else updateHitIndex({changed.constData(),size_t(hitChangedCount<0 ? changed.size() : hitChangedCount)},
                        from,removed,added,translatedFrom,translatedY);
    // Publish only after every queried rectangle and live layout agrees.
    for (const auto &block : pass.changed) emit updateBlock(block);
    if (m_size != oldSize) emit documentSizeChanged(m_size);
    if (m_pages != oldPages) emit pageCountChanged(m_pages);
    emit update();
}
void RichTextLayout::removeHitBlock(quint64 lifetime)
{
    auto it=m_hitBlocks.find(lifetime);
    if(it==m_hitBlocks.end()) return;
    auto *node=*it; HitNode::expose(node);
    HitNode *removed=nullptr;
    m_hitRoot=HitNode::erase(m_hitRoot,node->position,removed); m_hitBlocks.erase(it); delete removed;
}
void RichTextLayout::insertHitBlock(const QTextBlock &block,HitNode *reuse)
{
    const auto lifetime=blockInfo(block)->lifetime;
    const auto &cache=*m_blocks.constFind(lifetime);
    auto *node=reuse ? reuse : new HitNode;
    const auto oldOrigin=node->origin;
    node->lifetime=lifetime; node->position=block.position(); node->origin=cache.rect.topLeft();
    // SplitMix64 gives stable, well distributed priorities without global RNG state.
    quint64 priority=lifetime+0x9e3779b97f4a7c15ULL;
    priority=(priority^(priority>>30))*0xbf58476d1ce4e5b9ULL;
    priority=(priority^(priority>>27))*0x94d049bb133111ebULL;
    node->priority=priority^(priority>>31);
    if(node->version!=cache.hitVersion) {
        node->lines.clear(); node->bottoms.clear();
        node->lines.reserve(cache.hitRects.size());
        node->bottoms.reserve(cache.hitRects.size());
        for(const auto &rect:cache.hitRects) {
            const auto absolute=rect.translated(node->origin);
            node->ownBounds=node->lines.empty() ? absolute : HitNode::united(node->ownBounds,absolute);
            node->lines.push_back(rect);
            node->bottoms.push_back(node->bottoms.empty() ? rect.bottom() : qMax(node->bottoms.back(),rect.bottom()));
            ++counters.hitEntries;
        }
        node->version=cache.hitVersion;
    } else node->ownBounds.translate(node->origin-oldOrigin);
    node->bounds=node->ownBounds;
    HitNode *left,*right; HitNode::split(m_hitRoot,node->position,left,right);
    m_hitRoot=HitNode::merge(HitNode::merge(left,node),right); m_hitBlocks.insert(lifetime,node);
}
void RichTextLayout::updateHitIndex(std::span<const QTextBlock> changed,int from,int removed,int added,int translatedFrom,qreal translatedY)
{
    ++m_hitTransaction;
    // Detach affected runs before shifting suffix positions. Table solving may
    // replay a block, so mark nodes with the transaction number to visit once.
    std::vector<std::pair<QTextBlock,HitNode *>> updates;
    updates.reserve(changed.size());
    for(const auto &block:changed) {
        const auto lifetime=blockInfo(block)->lifetime;
        auto *node=m_hitBlocks.value(lifetime,nullptr);
        if(node && node->update==m_hitTransaction) continue;
        if(node) {
            HitNode::expose(node);
            HitNode *detached=nullptr;
            m_hitRoot=HitNode::erase(m_hitRoot,node->position,detached);
        } else {
            node=new HitNode; m_hitBlocks.insert(lifetime,node);
        }
        node->update=m_hitTransaction;
        updates.emplace_back(block,node);
    }
    HitNode *left,*right;
    HitNode::split(m_hitRoot,from+removed,left,right);
    HitNode::shift(right,added-removed,0); m_hitRoot=HitNode::merge(left,right);
    if(translatedFrom>=0) {
        HitNode::split(m_hitRoot,translatedFrom,left,right);
        HitNode::shift(right,0,translatedY); m_hitRoot=HitNode::merge(left,right);
    }
    for(const auto &[block,node]:updates) insertHitBlock(block,node);
}
void RichTextLayout::rebuildHitIndex()
{
    ++counters.hitRebuilds;
    delete m_hitRoot; m_hitRoot=nullptr; m_hitBlocks.clear();
    for(auto block=document()->begin();block.isValid();block=block.next()) insertHitBlock(block);
}
QList<QPair<int, QRectF>> RichTextLayout::hitIndexSnapshot() const
{
    QList<QPair<int, QRectF>> result;
    const auto visit=[&](auto &&self,const HitNode *node,int position,qreal y)->void {
        if(!node) return;
        self(self,node->left,position+node->pendingPosition,y+node->pendingY);
        for(const auto &line:node->lines)
            result.append({node->position+position,line.translated(node->origin+QPointF(0,y))});
        self(self,node->right,position+node->pendingPosition,y+node->pendingY);
    };
    visit(visit,m_hitRoot,0,0);
    return result;
}
int RichTextLayout::hitTest(const QPointF &point,Qt::HitTestAccuracy accuracy) const
{
    int first=0,last=document()->characterCount();
    // Cell fragments are already page-local rectangles. Restrict the search to
    // the containing cell before considering distance to text (including padding).
    for(const auto &fragment:m_fragments) if(fragment.table && fragment.rect.contains(point)) {
        const auto cell=fragment.table->cellAt(fragment.row,fragment.column);
        first=cell.firstPosition(); last=cell.lastPosition()+1; break;
    }
    qreal best=std::numeric_limits<qreal>::infinity();
    int result=-1,resultLine=0;
    QPointF resultOrigin;
    const auto distance=[&](const QRectF &rect) {
        const qreal dx=std::max({rect.left()-point.x(),0.,point.x()-rect.right()});
        const qreal dy=std::max({rect.top()-point.y(),0.,point.y()-rect.bottom()});
        return dx*dx+dy*dy;
    };
    const auto visit=[&](auto &&self,const HitNode *node,int offset,qreal y)->void {
        if(!node) return;
        const auto bounds=node->bounds.translated(0,y);
        const qreal lower=distance(bounds);
        if(lower>best || (accuracy==Qt::ExactHit && lower>0)) return;
        const int position=node->position+offset;
        const int childOffset=offset+node->pendingPosition;
        const qreal childY=y+node->pendingY;
        const auto origin=node->origin+QPointF(0,y);
        if(position>=first && position<last) {
            const auto candidate=[&](int line) {
                ++counters.hitCandidates;
                const auto rect=node->lines[line].translated(origin);
                if(accuracy==Qt::ExactHit && !rect.contains(point)) return;
                const qreal d=distance(rect);
                if(d<best || (d==best && (result<0 || position<result || (position==result && line<resultLine)))) {
                    best=d; result=position; resultLine=line; resultOrigin=origin;
                }
            };
            const qreal localY=point.y()-origin.y();
            const int upper=std::upper_bound(node->lines.cbegin(),node->lines.cend(),localY,
                [](qreal y,const QRectF &rect) { return y<rect.top(); })-node->lines.cbegin();
            for(int line=upper-1;line>=0;--line) {
                const qreal dy=qMax(0.,localY-node->bottoms[line]);
                if(dy*dy>best || (accuracy==Qt::ExactHit && dy>0)) break;
                candidate(line);
            }
            for(int line=upper;line<int(node->lines.size());++line) {
                const qreal dy=qMax(0.,node->lines[line].top()-localY);
                if(dy*dy>best || (accuracy==Qt::ExactHit && dy>0)) break;
                candidate(line);
            }
        }
        const auto *a=position>=first ? node->left : nullptr;
        const auto *b=position<last ? node->right : nullptr;
        if(a && b && distance(a->bounds.translated(0,childY))>distance(b->bounds.translated(0,childY))) std::swap(a,b);
        self(self,a,childOffset,childY); self(self,b,childOffset,childY);
    };
    visit(visit,m_hitRoot,0,0);
    if(result<0) return -1;
    const auto block=document()->findBlock(result);
    return block.position()+qMin(block.length()-1,block.layout()->lineAt(resultLine).xToCursor(point.x()-resultOrigin.x()));
}
void RichTextLayout::drawTableDecorations(QPainter *painter, const QRectF &clip, bool quick) const
{
    painter->save();
    if (!clip.isEmpty()) painter->setClipRect(clip,Qt::IntersectClip);
    for (const auto &fragment:m_fragments) {
        if (!clip.isEmpty() && !fragment.rect.adjusted(-1,-1,1,1).intersects(clip)) continue;
        if (quick) {
            painter->fillRect(fragment.rect,fragment.background);
            if (fragment.border.style()==Qt::NoPen) continue;
            const qreal width=fragment.border.widthF();
            const qreal inset=fragment.collapsed ? -width/2 : 0;
            const auto rect=fragment.rect.translated(inset,inset);
            const auto edge=[&](const QRectF &band) {
                painter->fillRect(band.toAlignedRect(),fragment.border.brush());
            };
            edge({rect.left(),rect.top(),width,rect.height()+width});
            edge({rect.right(),rect.top(),width,rect.height()+width});
            if (fragment.top) edge({rect.left(),rect.top(),rect.width()+width,width});
            if (fragment.bottom) edge({rect.left(),rect.bottom(),rect.width()+width,width});
            continue;
        }
        painter->setPen(fragment.border); painter->setBrush(fragment.background);
        if (fragment.top && fragment.bottom) painter->drawRect(fragment.rect);
        else {
            painter->fillRect(fragment.rect,fragment.background);
            painter->drawLine(fragment.rect.topLeft(),fragment.rect.bottomLeft());
            painter->drawLine(fragment.rect.topRight(),fragment.rect.bottomRight());
            if (fragment.top) painter->drawLine(fragment.rect.topLeft(),fragment.rect.topRight());
            if (fragment.bottom) painter->drawLine(fragment.rect.bottomLeft(),fragment.rect.bottomRight());
        }
    }
    painter->restore();
}
void RichTextLayout::draw(QPainter *painter, const PaintContext &context)
{
    painter->save();
    if (!context.clip.isEmpty()) painter->setClipRect(context.clip, Qt::IntersectClip);
    drawTableDecorations(painter,context.clip);
    painter->setPen(context.palette.color(QPalette::Text));
    for (auto b = document()->begin(); b.isValid(); b = b.next()) {
        const auto rect = blockBoundingRect(b);
        auto *layout = b.layout();
        if (!context.clip.isEmpty() && !context.clip.intersects(layout->boundingRect().translated(layout->position()))) continue;
        QList<QTextLayout::FormatRange> selections;
        for (const auto &selection : context.selections) {
            const int start = qMax(b.position(), selection.cursor.selectionStart());
            const int end = qMin(b.position()+b.length(), selection.cursor.selectionEnd());
            if (start < end) selections.append({start-b.position(), end-start, selection.format});
        }
        if (b.blockFormat().background().style() != Qt::NoBrush)
            for (int i=0; i<layout->lineCount(); ++i)
                painter->fillRect(layout->lineAt(i).rect().translated(layout->position()), b.blockFormat().background());
        if (auto *list = b.textList(); list && layout->lineCount()) {
            painter->save(); painter->setFont(b.charFormat().font().resolve(document()->defaultFont()));
            const auto line = layout->lineAt(0);
            QString marker = list->itemText(b);
            if (list->format().style() == QTextListFormat::ListCircle) marker = QChar(0x25e6);
            else if (list->format().style() == QTextListFormat::ListSquare) marker = QChar(0x25aa);
            else if (marker.isEmpty()) marker = QChar(0x2022);
            if (b.blockFormat().marker() == QTextBlockFormat::MarkerType::Checked) marker = QChar(0x2612);
            else if (b.blockFormat().marker() == QTextBlockFormat::MarkerType::Unchecked) marker = QChar(0x2610);
            const QFontMetricsF metrics(painter->font());
            const auto textRect = line.naturalTextRect().translated(layout->position());
            const qreal x = b.textDirection() == Qt::RightToLeft
                ? textRect.right() + metrics.horizontalAdvance(QLatin1Char(' '))
                : textRect.left() - metrics.horizontalAdvance(marker) - metrics.horizontalAdvance(QLatin1Char(' '));
            if (b.charFormat().foreground().style() != Qt::NoBrush) painter->setPen(b.charFormat().foreground().color());
            painter->drawText(QPointF(x, rect.y()+line.y()+line.ascent()), marker);
            painter->restore();
        }
        layout->draw(painter, QPointF(), selections, context.clip);
        if (context.cursorPosition >= b.position() && context.cursorPosition < b.position()+b.length())
            layout->drawCursor(painter, QPointF(), context.cursorPosition-b.position());
    }
    painter->restore();
}
} // namespace rt
