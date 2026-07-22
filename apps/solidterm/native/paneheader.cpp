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
    p->setPen(m_focused ? m_foreground : m_foreground.darker(130));
    const QRectF textRect(10, 0, width() - 40, height());
    const QString elided = p->fontMetrics().elidedText(
        m_title.isEmpty() ? QStringLiteral("terminal") : m_title, Qt::ElideRight, int(textRect.width()));
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);

    // Close affordance (×) on the right.
    const QRectF cr = closeRect();
    p->setPen(QPen(m_foreground.darker(120), 1.4));
    p->drawLine(cr.topLeft() + QPointF(4, 4), cr.bottomRight() - QPointF(4, 4));
    p->drawLine(cr.topRight() + QPointF(-4, 4), cr.bottomLeft() + QPointF(4, -4));
}

void PaneHeader::mousePressEvent(QMouseEvent *event)
{
    if (closeRect().adjusted(-3, -3, 3, 3).contains(event->position()))
        emit closeRequested();
    else
        emit clicked();
    event->accept();
}
