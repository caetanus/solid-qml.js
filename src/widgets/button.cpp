#include "button.h"

#include "snippetwidget.h"

#include "qmlcss/componentcache.h"

#include <QCursor>

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>

namespace SolidWidgets {

namespace {

// The behavior half of <button>: a real T.Button filling the CssFill root, with its own visuals
// nulled (the root paints via CSS). It owns click, Space/Enter activation, auto-repeat, the
// press/hover/focus states and the Button accessibility role/name — everything this widget used
// to re-derive from raw mouse/key/hover events.
//
// `activeFocusOnTab` follows the app-wide switch (the desktop model: a button IS a tab stop), and
// every state change is mirrored back into the root's cssState so `:hover`/`:active`/`:focus`
// cascade exactly as before.
const char *kButtonBody = R"QML(
import QtQuick
import QtQuick.Templates as T

T.Button {
    // Sized off `root` (the context property), NOT `anchors.fill: parent`: this snippet is created
    // before its parentItem is assigned, so an anchor to `parent` would bind to null and leave the
    // control 0x0 — it would take focus but never receive a click.
    x: 0
    y: 0
    width: root.width
    height: root.height
    background: null
    contentItem: null
    // Drives the ACCESSIBLE NAME (QQuickAbstractButton exposes text to QAccessible); the visible
    // label is the root's own CssText in the layout slot, so nothing is painted twice.
    text: root.text
    enabled: root.enabled
    activeFocusOnTab: typeof solidTabstop !== "undefined" && solidTabstop ? solidTabstop.enabled : true
    onClicked: root.clicked()
    onHoveredChanged: root.syncState()
    onDownChanged: root.syncState()
    onActiveFocusChanged: root.syncState()
}
)QML";

} // namespace

Button::Button(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("button"));
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

void Button::takeFocus()
{
    if (m_control)
        m_control->forceActiveFocus(Qt::TabFocusReason);
    else
        forceActiveFocus(Qt::TabFocusReason);
}

void Button::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    ensureLabel();
    ensureControl();
    syncState();
}

void Button::ensureControl()
{
    if (m_control)
        return;
    // A PLAIN child (not a layout participant): it fills the root and only handles input.
    QObject *o = composeInternalPlain(this, QStringLiteral("solidwidgets-button-ctl"), kButtonBody);
    m_control = qobject_cast<QQuickItem *>(o);
    if (m_control)
        m_control->setZ(1); // above the label, so it receives the pointer events
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
    // States come from the composed control (it owns the interaction); the root only cascades them.
    const bool hovered = m_control && m_control->property("hovered").toBool();
    const bool down = m_control && m_control->property("down").toBool();
    const bool focused = m_control && m_control->property("activeFocus").toBool();

    QVariantList state;
    if (hovered)
        state << QStringLiteral("hover");
    if (down)
        state << QStringLiteral("active");
    if (focused)
        state << QStringLiteral("focus");
    if (m_isDefault)
        state << QStringLiteral("default");
    if (m_disabled)
        state << QStringLiteral("disabled");
    setCssState(state);
}

} // namespace SolidWidgets
