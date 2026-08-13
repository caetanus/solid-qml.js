#include "paneheader.h"

#include <QMouseEvent>
#include <QPainter>

PaneHeader::PaneHeader(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setImplicitHeight(24);
}

void PaneHeader::setTitle(const QString &v)
{
    if (m_title == v)
        return;
    m_title = v;
    emit titleChanged();
    update();
}

void PaneHeader::setIndex(int v)
{
    if (m_index == v)
        return;
    m_index = v;
    emit indexChanged();
    update();
}

void PaneHeader::setFocused(bool v)
{
    if (m_focused == v)
        return;
    m_focused = v;
    emit focusedChanged();
    update();
}

void PaneHeader::setReadOnly(bool v)
{
    if (m_readOnly == v)
        return;
    m_readOnly = v;
    emit readOnlyChanged();
    update();
}

void PaneHeader::setBackground(const QColor &v) { if (m_background == v) return; m_background = v; emit styleChanged(); update(); }
void PaneHeader::setForeground(const QColor &v) { if (m_foreground == v) return; m_foreground = v; emit styleChanged(); update(); }
void PaneHeader::setAccent(const QColor &v) { if (m_accent == v) return; m_accent = v; emit styleChanged(); update(); }

QRectF PaneHeader::closeRect() const
{
    const qreal s = 16;
    return QRectF(width() - s - 8, (height() - s) / 2, s, s);
}

QRectF PaneHeader::maximizeRect() const
{
    const qreal s = 16;
    return QRectF(width() - 2 * s - 16, (height() - s) / 2, s, s); // left of the close ×
}

void PaneHeader::paint(QPainter *p)
{
    p->setRenderHint(QPainter::Antialiasing);
    p->fillRect(boundingRect(), m_background);
    // Focused pane: a 2px accent underline (tilix highlights the active terminal).
    if (m_focused)
        p->fillRect(QRectF(0, height() - 2, width(), 2), m_accent);

    QFont f = p->font();
    f.setPixelSize(12);
    p->setFont(f);
    // "N: title" (plain, tilix-style) — the number is a text prefix, not a badge.
    const QString base = m_title.isEmpty() ? QStringLiteral("terminal") : m_title;
    const QString full = m_index > 0 ? (QString::number(m_index) + QStringLiteral(": ") + base) : base;
    p->setPen(m_focused ? m_foreground : m_foreground.darker(130));
    const QRectF textRect(10, 0, width() - 76, height());
    const QString elided = p->fontMetrics().elidedText(full, Qt::ElideRight, int(textRect.width()));
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);

    // ▼ dropdown caret right after the title (the whole header is a dropdown, tilix-style).
    const qreal tw = qMin(textRect.width(), qreal(p->fontMetrics().horizontalAdvance(elided)));
    const qreal ax = textRect.left() + tw + 7, ay = height() / 2;
    p->setBrush(m_foreground.darker(120));
    p->setPen(Qt::NoPen);
    p->drawPolygon(QPolygonF({ QPointF(ax, ay - 2), QPointF(ax + 7, ay - 2), QPointF(ax + 3.5, ay + 2.5) }));

    // Read-only lock indicator (before the ⤡ button).
    if (m_readOnly) {
        p->setPen(QPen(m_accent, 1.3));
        p->setBrush(Qt::NoBrush);
        const QRectF b(maximizeRect().left() - 18, ay - 3, 8, 6); // padlock body
        p->drawRect(b);
        p->drawArc(QRectF(b.left() + 1.5, b.top() - 4, 5, 6), 0, 180 * 16); // shackle
    }

    // Maximize/restore, drawn as Tilix's zoom glyph: TWO SEPARATE arrows pointing out to opposite
    // corners with a gap between them — not one continuous diagonal (that reads as a resize cursor).
    // Each arrow is a short stem plus a proper arrowhead at the corner end.
    const QRectF mr = maximizeRect();
    p->setPen(QPen(m_foreground.darker(120), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const qreal m = 3.0;   // inset from the button box
    const qreal head = 3.4; // arrowhead leg length
    const qreal gap = 1.8;  // half the empty space at the centre
    const QPointF c = mr.center();
    // ↙ arrow: stem toward the bottom-left corner, head opening left+down.
    const QPointF bl(mr.left() + m, mr.bottom() - m);
    p->drawLine(QPointF(c.x() - gap, c.y() + gap), bl);
    p->drawLine(bl, bl + QPointF(head, 0));
    p->drawLine(bl, bl + QPointF(0, -head));
    // ↗ arrow: stem toward the top-right corner, head opening right+up.
    const QPointF tr(mr.right() - m, mr.top() + m);
    p->drawLine(QPointF(c.x() + gap, c.y() - gap), tr);
    p->drawLine(tr, tr + QPointF(-head, 0));
    p->drawLine(tr, tr + QPointF(0, head));

    const QRectF cr = closeRect();
    p->drawLine(cr.topLeft() + QPointF(4, 4), cr.bottomRight() - QPointF(4, 4));
    p->drawLine(cr.topRight() + QPointF(-4, 4), cr.bottomLeft() + QPointF(4, -4));
}

void PaneHeader::mousePressEvent(QMouseEvent *event)
{
    m_armed = false;
    m_dragging = false;
    if (closeRect().adjusted(-3, -3, 3, 3).contains(event->position()))
        emit closeRequested();
    else if (maximizeRect().adjusted(-3, -3, 3, 3).contains(event->position()))
        emit maximizeRequested();
    else {
        // Title area: focus now, but DEFER the dropdown to release — a press that turns into a drag
        // is a pane move, not a menu open. Arm the drag; the threshold decides which it is.
        emit clicked();
        m_armed = true;
        m_pressGlobal = event->globalPosition();
    }
    event->accept();
}

void PaneHeader::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_armed) {
        event->ignore();
        return;
    }
    const QPointF g = event->globalPosition();
    if (!m_dragging) {
        // Past the threshold → promote the press to a drag (tilix-style pane rearrange).
        if ((g - m_pressGlobal).manhattanLength() < 8) {
            event->accept();
            return;
        }
        m_dragging = true;
        emit dragStarted();
    }
    emit dragMoved(g.x(), g.y());
    event->accept();
}

void PaneHeader::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPointF g = event->globalPosition();
        emit dragEnded(g.x(), g.y());
    } else if (m_armed) {
        // No drag happened → this was a click: open the dropdown below the header.
        const QPointF sp = mapToScene(QPointF(4, height()));
        emit menuRequested(sp.x(), sp.y());
    }
    m_armed = false;
    m_dragging = false;
    event->accept();
}
