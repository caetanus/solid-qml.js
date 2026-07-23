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
    const QRectF textRect(10, 0, width() - 62, height());
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(full, Qt::ElideRight, int(textRect.width())));

    // Maximize/restore (⤡) and close (×) on the right.
    const QRectF mr = maximizeRect();
    p->setPen(QPen(m_foreground.darker(120), 1.4));
    const qreal m = 3.5;
    // two opposing corner brackets — the tilix zoom glyph
    p->drawLine(mr.left() + m, mr.top() + m + 3, mr.left() + m, mr.top() + m);
    p->drawLine(mr.left() + m, mr.top() + m, mr.left() + m + 3, mr.top() + m);
    p->drawLine(mr.right() - m, mr.bottom() - m - 3, mr.right() - m, mr.bottom() - m);
    p->drawLine(mr.right() - m, mr.bottom() - m, mr.right() - m - 3, mr.bottom() - m);
    p->drawLine(mr.left() + m, mr.top() + m, mr.right() - m, mr.bottom() - m);

    const QRectF cr = closeRect();
    p->drawLine(cr.topLeft() + QPointF(4, 4), cr.bottomRight() - QPointF(4, 4));
    p->drawLine(cr.topRight() + QPointF(-4, 4), cr.bottomLeft() + QPointF(4, -4));
}

void PaneHeader::mousePressEvent(QMouseEvent *event)
{
    if (closeRect().adjusted(-3, -3, 3, 3).contains(event->position()))
        emit closeRequested();
    else if (maximizeRect().adjusted(-3, -3, 3, 3).contains(event->position()))
        emit maximizeRequested();
    else
        emit clicked();
    event->accept();
}
