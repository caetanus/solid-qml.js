#include "focusring.h"

#include <QQmlContext>
#include <QQmlEngine>

namespace SolidWidgets {

Tabstop::Tabstop(QQuickItem *parent)
    : QmlCss::CssRect(parent)
{
    setCssPrimitive(QString());
    setCssPart(QStringLiteral("tab-stop"));
    setZ(1000000);
    setVisible(false);

    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &Tabstop::track);
}

void Tabstop::setWindow(QQuickWindow *w)
{
    if (m_window == w)
        return;
    if (m_window)
        disconnect(m_window, nullptr, this, nullptr);
    m_window = w;
    if (m_window)
        connect(m_window, &QQuickWindow::activeFocusItemChanged, this, &Tabstop::refreshVisible);
    emit windowChanged();
    refreshVisible();
}

void Tabstop::componentComplete()
{
    QmlCss::CssRect::componentComplete();
    // The loader's app-wide tab-navigation switch (a context property, like cssTheme).
    if (QQmlContext *ctx = qmlContext(this))
        m_tabstop = ctx->contextProperty(QStringLiteral("solidTabstop")).value<QObject *>();
    if (m_tabstop)
        connect(m_tabstop, SIGNAL(enabledChanged()), this, SLOT(refreshVisible()));
    refreshVisible();
}

void Tabstop::refreshVisible()
{
    const bool enabled = m_tabstop ? m_tabstop->property("enabled").toBool() : true;
    QQuickItem *focus = m_window ? m_window->activeFocusItem() : nullptr;
    const bool on = enabled && m_window && focus && focus != m_window->contentItem();
    setVisible(on);
    if (on) {
        m_timer.start();
        track();
    } else {
        m_timer.stop();
    }
}

void Tabstop::track()
{
    QQuickItem *t = m_window ? m_window->activeFocusItem() : nullptr;
    if (!t || !parentItem())
        return;
    const QPointF p = t->mapToItem(parentItem(), QPointF(0, 0));
    setX(p.x() - 2);
    setY(p.y() - 2);
    setWidth(t->width() + 4);
    setHeight(t->height() + 4);
}

} // namespace SolidWidgets
