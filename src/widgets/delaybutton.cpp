#include "delaybutton.h"

#include "snippetwidget.h"

namespace {

// Original DelayButton.qml internals — `root` = the C++ wrapper. The wrapper stays paint-less
// (cssPrimitive ""): the `.delay` background slot owns the pill so a generic `button {}` rule
// can't double-paint it.
const char *kDelayButtonBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.DelayButton {
    id: __ctl
    anchors.fill: parent
    activeFocusOnTab: solidTabstop.enabled
    hoverEnabled: true
    delay: root.delay
    enabled: !root.disabled
    onHoveredChanged: root.hovered = hovered
    onPressedChanged: root.pressed = pressed
    onCheckedChanged: root.checked = checked
    // T.DelayButton has no built-in progress animation: without a `transition`, pressing sets
    // `progress` straight to 1.0 → the button arms on a single click. This is the Basic style's
    // transition (hold ramps 0→1 over `delay`; release eases back). Same class of bug as <Drawer>.
    transition: Transition { NumberAnimation { duration: __ctl.delay * (__ctl.pressed ? 1.0 - __ctl.progress : 0.3 * __ctl.progress) } }
    horizontalPadding: 16
    verticalPadding: 8
    // Basic-style implicit size: Templates leave implicit sizes to the style (us).
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    onActivated: root.activated()
    background: Css.CssFill {
        cssPrimitive: ""
        cssClass: ["delay"]
        cssState: root.cssState
        implicitWidth: 120
        implicitHeight: 36
        // Progress overlay: grows with the hold (progress 0→1 over `delay` ms).
        Item {
            anchors.fill: parent
            Css.CssRect {
                cssClass: ["delay-fill"]
                width: __ctl.progress * parent.width
                height: parent.height
            }
        }
    }
    // Label: the control sizes/positions its contentItem; colour/font inherit from the wrapper.
    contentItem: Css.CssText {
        cssPrimitive: "text"
        text: root.text
    }
}
)";

} // namespace

namespace SolidWidgets {

DelayButton::DelayButton(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    connect(this, &QQuickItem::enabledChanged, this, &DelayButton::syncState);
}

void DelayButton::setText(const QString &v)
{
    if (m_text == v)
        return;
    m_text = v;
    emit textChanged();
}

void DelayButton::setDelay(int v)
{
    if (m_delay == v)
        return;
    m_delay = v;
    emit delayChanged();
}

void DelayButton::setDisabled(bool v)
{
    if (m_disabled == v)
        return;
    m_disabled = v;
    emit disabledChanged();
    syncState();
}

void DelayButton::setHovered(bool v)
{
    if (m_hovered == v)
        return;
    m_hovered = v;
    emit hoveredChanged();
    syncState();
}

void DelayButton::setPressed(bool v)
{
    if (m_pressed == v)
        return;
    m_pressed = v;
    emit pressedChanged();
    syncState();
}

void DelayButton::setChecked(bool v)
{
    if (m_checked == v)
        return;
    m_checked = v;
    emit checkedChanged();
    syncState();
}

void DelayButton::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-delaybutton"), kDelayButtonBody);
    if (m_control) {
        connect(m_control, SIGNAL(activeFocusChanged(bool)), this, SLOT(syncState()));
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
    syncState();
}

void DelayButton::syncState()
{
    QVariantList state;
    if (m_hovered)
        state << QStringLiteral("hover");
    if (m_pressed)
        state << QStringLiteral("active");
    if (m_checked)
        state << QStringLiteral("checked");
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (m_disabled || !isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

} // namespace SolidWidgets
