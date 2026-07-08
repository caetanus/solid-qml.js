#include "button.h"

#include "qmlcss/componentcache.h"

#include <QCursor>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>

namespace SolidWidgets {

Button::Button(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("button"));
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
#if QT_CONFIG(cursor)
    setCursor(QCursor(Qt::PointingHandCursor));
#endif
}

void Button::setText(const QString &v)
{
    if (m_text == v)
        return;
    m_text = v;
    if (m_label) {
        m_label->setText(m_text);
        m_label->setVisible(!m_text.isEmpty());
    }
    emit textChanged();
}

void Button::setIsDefault(bool v)
{
    if (m_isDefault == v)
        return;
    m_isDefault = v;
    emit isDefaultChanged();
    syncState();
}

void Button::setDisabled(bool v)
{
    if (m_disabled == v)
        return;
    m_disabled = v;
    setEnabled(!v);
    emit disabledChanged();
    syncState();
}

void Button::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    ensureLabel();

    // Desktop model (study §6): a button IS a tab stop, gated by the app-wide switch.
    if (QQmlContext *ctx = qmlContext(this))
        m_tabstop = ctx->contextProperty(QStringLiteral("solidTabstop")).value<QObject *>();
    if (m_tabstop)
        connect(m_tabstop, SIGNAL(enabledChanged()), this, SLOT(syncTabstop()));
    syncTabstop();
    syncState();
}

void Button::syncTabstop()
{
    setActiveFocusOnTab(!m_tabstop || m_tabstop->property("enabled").toBool());
}

void Button::ensureLabel()
{
    if (m_label)
        return;
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    // Composed through a cached snippet so the label gets a REAL QML context — the engine types
    // resolve cssTheme/cssLayout through it (a bare `new CssText` would be themeless).
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("solidwidgets-button-label"),
        "import qmlcss 1.0\nCssText { cssPrimitive: \"text\" }\n");
    QObject *o = comp->create(qmlContext(this));
    m_label = qobject_cast<QmlCss::CssText *>(o);
    if (!m_label)
        return;
    m_label->setParent(this);
    // Into the CONTENT slot (the layout holder), like a declared child.
    auto slot = content();
    slot.append(&slot, m_label);
    m_label->setText(m_text);
    m_label->setVisible(!m_text.isEmpty());
}

void Button::syncState()
{
    QVariantList state;
    if (m_hovered)
        state << QStringLiteral("hover");
    if (m_pressed)
        state << QStringLiteral("active");
    if (hasActiveFocus())
        state << QStringLiteral("focus");
    if (m_isDefault)
        state << QStringLiteral("default");
    if (m_disabled)
        state << QStringLiteral("disabled");
    setCssState(state);
}

void Button::hoverEnterEvent(QHoverEvent *event)
{
    QmlCss::CssFill::hoverEnterEvent(event);
    m_hovered = true;
    syncState();
}

void Button::hoverLeaveEvent(QHoverEvent *event)
{
    QmlCss::CssFill::hoverLeaveEvent(event);
    m_hovered = false;
    syncState();
}

void Button::mousePressEvent(QMouseEvent *event)
{
    // Clicking a control also FOCUSES it (the tab anchor moves to the clicked button).
    if (activeFocusOnTab())
        forceActiveFocus(Qt::MouseFocusReason);
    m_pressed = true;
    syncState();
    event->accept();
}

void Button::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
    m_pressed = false;
    syncState();
    if (boundingRect().contains(event->position()))
        emit clicked();
}

void Button::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        event->accept();
        emit clicked();
        return;
    default:
        QmlCss::CssFill::keyPressEvent(event);
    }
}

void Button::itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &data)
{
    QmlCss::CssFill::itemChange(change, data);
    if (change == ItemActiveFocusHasChanged)
        syncState();
}

} // namespace SolidWidgets
