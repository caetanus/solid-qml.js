#include "richtext_layout.h"
#include "richtext_blocks.h"

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

namespace rt {
RichTextLayout::RichTextLayout(QTextDocument *d) : QAbstractTextDocumentLayout(d) {}
RichTextLayout::~RichTextLayout()
{
    for (auto b = document()->begin(); b.isValid(); b = b.next())
        if (auto *info = blockInfo(b); info && info->layoutOwner == this) info->layoutOwner = nullptr;
}
bool RichTextLayout::supports(const QTextDocument *d)
{
    if (!d || !d->rootFrame()->childFrames().isEmpty()) return false;
    const auto root = d->rootFrame()->frameFormat();
    if (root.border() != 0 || root.padding() != 0 || root.background().style() != Qt::NoBrush) return false;
    for (auto b = d->begin(); b.isValid(); b = b.next())
        if (!b.isVisible() || b.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) return false;
    return true;
}
void RichTextLayout::forgetBlock(quint64 generation)
{
    const auto it = m_blocks.constFind(generation);
    if (it != m_blocks.cend() && it->right > m_page.width() && it->right >= m_size.width()) m_widthDirty = true;
    m_blocks.remove(generation);
}
QRectF RichTextLayout::frameBoundingRect(QTextFrame *frame) const
{
    return frame == document()->rootFrame() ? QRectF({}, m_size) : QRectF();
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
void RichTextLayout::documentChanged(int from, int removed, int added)
{
    ++counters.callbacks;
    auto *doc = document();
    // setDocumentLayout can notify during a suspended load. The enabling
    // transaction will provide the complete dirty range and publish geometry once.
    if (!doc->isLayoutEnabled()) return;
    const auto page = doc->pageSize();
    const auto root = doc->rootFrame()->frameFormat();
    const bool full = m_blocks.isEmpty() || page != m_page || root != m_root
        || (from == 0 && added >= doc->characterCount()-1);
    m_page = page; m_root = root;
    const qreal stride = page.height() > 0 ? page.height() : 1e12;
    const qreal width = page.width() > 0 ? page.width() : 640;
    const qreal top = root.topMargin(), bottom = root.bottomMargin();
    auto b = full ? doc->begin() : doc->findBlock(from);
    if (!b.isValid()) b = doc->lastBlock();
    if (!full && b.previous().isValid()) b = b.previous();
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
    QList<QTextBlock> changed; // Transaction-local handles, never retained across edits.
    for (; b.isValid(); b = b.next()) {
        auto *info = blockInfo(b);
        if (!info) { info = new BlockInfo(QString()); b.setUserData(info); }
        info->layoutOwner = this;
        auto it = m_blocks.find(info->lifetime);
        const bool known = it != m_blocks.end() && it->id == info->id;
        const Cache old = known ? *it : Cache();
        auto *layout = b.layout();
        const bool dirty = full || !known || !layout->lineCount() || old.revision != b.revision()
            || (b.position() <= from + added && b.position() + b.length() > from);
        const auto format = b.blockFormat();
        const auto direction = b.textDirection();
        const auto *list = b.textList();
        const int indent = format.indent() + (list ? list->format().indent() : 0);
        const qreal left = root.leftMargin();
        const qreal available = qMax(1., width - left - root.rightMargin() - format.leftMargin() - format.rightMargin() - indent * doc->indentWidth());
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
        if (b != doc->begin()) flow.y += qMax(flow.bottom, format.topMargin());
        const qreal origin = flow.y;
        bool sameLines = known && !dirty;
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
            const QPointF position(format.leftMargin() + (direction == Qt::RightToLeft ? 0 : indent * doc->indentWidth() + (index == 0 ? format.textIndent() : 0)), flow.y-origin+offset);
            sameLines &= line.position() == position;
            line.setPosition(position);
            flow.y += advance;
        }
        layout->setPosition({left, origin});
        flow.bottom = format.bottomMargin();
        flow.pageBreak = format.pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysAfter);
        // Qt publishes the layout origin plus its bounding *size*: a first-line
        // page jump is not included in that size. Painting uses the translated
        // line bounds instead, so this compatibility rectangle never clips ink.
        next.outgoing = flow; next.rect = QRectF(QPointF(left, origin), layout->boundingRect().size());
        next.right = left + layout->boundingRect().right() + root.rightMargin();
        documentWidth = qMax(documentWidth, next.right);
        if (known && old.right >= oldSize.width() && next.right < old.right) m_widthDirty = true;
        m_blocks.insert(info->lifetime, next);
        changed.append(b);
        const qreal delta = next.rect.y() - old.rect.y();
        const bool wholePage = delta == qRound64(delta/stride)*stride;
        if (!full && b.position() > from+added && sameLines && wholePage
            && next.incoming.y-old.incoming.y == delta && next.outgoing.y-old.outgoing.y == delta
            && next.incoming.bottom == old.incoming.bottom && next.incoming.pageBreak == old.incoming.pageBreak
            && next.outgoing.bottom == old.outgoing.bottom && next.outgoing.pageBreak == old.outgoing.pageBreak
            && next.rect.x() == old.rect.x() && next.rect.size() == old.rect.size()) {
            ++counters.convergences;
            if (delta != 0) {
                for (auto tail = b.next(); tail.isValid(); tail = tail.next()) {
                    auto *tailInfo = blockInfo(tail);
                    auto &cache = m_blocks[tailInfo->lifetime];
                    cache.rect.translate(0, delta); cache.incoming.y += delta; cache.outgoing.y += delta;
                    tail.layout()->setPosition(cache.rect.topLeft());
                    ++counters.translations; changed.append(tail);
                }
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
    // Publish only after every queried rectangle and live layout agrees.
    for (const auto &block : changed) emit updateBlock(block);
    if (m_size != oldSize) emit documentSizeChanged(m_size);
    if (m_pages != oldPages) emit pageCountChanged(m_pages);
    emit update();
}
int RichTextLayout::hitTest(const QPointF &point, Qt::HitTestAccuracy accuracy) const
{
    qreal best = std::numeric_limits<qreal>::max(); int result = -1;
    for (auto b = document()->begin(); b.isValid(); b = b.next()) {
        auto *layout = b.layout();
        for (int i = 0; i < layout->lineCount(); ++i) {
            const auto line = layout->lineAt(i);
            const auto rect = line.naturalTextRect().translated(layout->position());
            if (accuracy == Qt::ExactHit && !rect.contains(point)) continue;
            const qreal dy = std::max({rect.top()-point.y(), 0., point.y()-rect.bottom()});
            const qreal dx = std::max({rect.left()-point.x(), 0., point.x()-rect.right()});
            const qreal distance = dy*dy+dx*dx;
            if (distance < best) {
                best = distance;
                result = b.position()+qMin(b.length()-1, line.xToCursor(point.x()-layout->position().x()));
            }
        }
    }
    return result;
}
void RichTextLayout::draw(QPainter *painter, const PaintContext &context)
{
    painter->save();
    if (!context.clip.isEmpty()) painter->setClipRect(context.clip, Qt::IntersectClip);
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
