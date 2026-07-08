#include "spindial.h"

#include "snippetwidget.h"

namespace {

// Original SpinBox.qml internals — `root` = the C++ wrapper.
const char *kSpinBoxBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.SpinBox {
    id: ctl
    anchors.fill: parent
    background: null
    editable: true
    activeFocusOnTab: solidTabstop.enabled
    from: root.from
    to: root.to
    stepSize: root.stepSize
    onValueChanged: root.value = value
    onValueModified: root.valueModified()
    // Controls resize contentItem to the control minus paddings — without a rightPadding the
    // TextInput covers the +/- buttons and eats their clicks.
    leftPadding: 12
    rightPadding: 32

    Binding on value {
        value: root.value
        restoreMode: Binding.RestoreNone
    }

    // HTML semantics: the wheel steps the value, but ONLY while the field has focus; unfocused,
    // the event must fall through to the page scroll. valueModified() reuses the onChange wiring.
    // Stepping writes `value` directly: Qt 6.11's SpinBox refactor (QQuickAbstractSpinBox) dropped
    // the Q_INVOKABLE from the increment/decrement methods. acceptedDevices: the default is Mouse ONLY —
    // touchpad scrolling is filtered in wantsPointerEvent; accumulate to the 120-unit notch.
    WheelHandler {
        property real __acc: 0
        enabled: ctl.activeFocus
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { ctl.value = Math.max(ctl.from, Math.min(ctl.to, ctl.value + s * ctl.stepSize)); ctl.valueModified() } }
    }
    // contentItem: a plain TextInput (not Css) — it lives inside the control's item tree, not our
    // CSS layout engine. Color/font are bridged from the CssFill wrapper (root).
    contentItem: TextInput {
        // T.SpinBox is a focus scope: focus: true forwards the control's active focus into the
        // TextInput so tabbing in lets the user type immediately.
        focus: true
        text: ctl.displayText
        validator: ctl.validator
        readOnly: !ctl.editable
        color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
        font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
        font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        selectByMouse: true
    }
    // up indicator: Css.CssFill at the top-right of the SpinBox; cssState "active" when pressed.
    // Inset 2px from the wrapper's edge so the buttons sit INSIDE the rounded border.
    up.indicator: Css.CssFill {
        cssPrimitive: ""
        cssClass: ["spin-up"]
        cssState: ctl.up.pressed ? ["active"] : []
        x: parent.width - width - 2
        y: 2
        width: 24
        height: (parent.height - 4) / 2
        implicitWidth: 24
        implicitHeight: (parent.height - 4) / 2
        // Plain Text, NOT CssText: a Css child inside this CssFill is re-laid-out by the CSS engine
        // (stomps the centerIn anchor). A plain primitive is invisible to the layout; the nested
        // CssItem injects the CSS (color/font from the .spin-glyph rule) without joining the layout.
        Item {
            anchors.fill: parent
            Text {
                text: "+"
                anchors.centerIn: parent
                Css.CssItem { cssPrimitive: "text"; cssClass: ["spin-glyph"] }
            }
        }
    }
    // down indicator: mirrors up, at the bottom-right (same 2px inset).
    down.indicator: Css.CssFill {
        cssPrimitive: ""
        cssClass: ["spin-down"]
        cssState: ctl.down.pressed ? ["active"] : []
        x: parent.width - width - 2
        y: parent.height / 2
        width: 24
        height: (parent.height - 4) / 2
        implicitWidth: 24
        implicitHeight: (parent.height - 4) / 2
        Item {
            anchors.fill: parent
            Text {
                text: "−"
                anchors.centerIn: parent
                Css.CssItem { cssPrimitive: "text"; cssClass: ["spin-glyph"] }
            }
        }
    }
}
)";

// Original Dial.qml internals. T.Dial positions NOTHING (qquickdial_p.h): the STYLE places the
// handle from `angle` (0° = 12 o'clock, positive clockwise): cx + sin(angle)·r, cy − cos(angle)·r
// with r = background.width/2 − 12. The dial circle is the background slot (border-radius via CSS).
const char *kDialBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.Dial {
    id: __ctl
    anchors.fill: parent
    activeFocusOnTab: solidTabstop.enabled
    from: root.from
    to: root.to
    stepSize: root.stepSize
    enabled: !root.disabled
    onValueChanged: root.value = value
    onMoved: root.moved()
    onPressedChanged: root.pressed = pressed

    Binding on value {
        value: root.value
        restoreMode: Binding.RestoreNone
    }

    // Basic-style implicit size: Templates leave implicit sizes to the style (us).
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    // The dial face: a centred square CssFill — `.dial { border-radius: … }` makes it a circle.
    background: Css.CssFill {
        cssPrimitive: ""
        cssClass: ["dial"]
        x: __ctl.width / 2 - width / 2
        y: __ctl.height / 2 - height / 2
        width: Math.max(32, Math.min(__ctl.width, __ctl.height))
        height: width
        implicitWidth: 96
        implicitHeight: 96
    }
    handle: Css.CssRect {
        cssClass: ["handle"]
        width: 12
        height: 12
        implicitWidth: 12
        implicitHeight: 12
        x: __ctl.background.x + __ctl.background.width / 2 - width / 2 + Math.sin(__ctl.angle * Math.PI / 180) * (__ctl.background.width / 2 - 12)
        y: __ctl.background.y + __ctl.background.height / 2 - height / 2 - Math.cos(__ctl.angle * Math.PI / 180) * (__ctl.background.width / 2 - 12)
    }
}
)";

} // namespace

namespace SolidWidgets {

SpinBox::SpinBox(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("input"));
    connect(this, &QQuickItem::enabledChanged, this, &SpinBox::syncState);
}

void SpinBox::setValue(int v)
{
    if (m_value == v)
        return;
    m_value = v;
    emit valueChanged();
}

void SpinBox::setFrom(int v)
{
    if (m_from == v)
        return;
    m_from = v;
    emit fromChanged();
}

void SpinBox::setTo(int v)
{
    if (m_to == v)
        return;
    m_to = v;
    emit toChanged();
}

void SpinBox::setStepSize(int v)
{
    if (m_stepSize == v)
        return;
    m_stepSize = v;
    emit stepSizeChanged();
}

void SpinBox::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-spinbox"), kSpinBoxBody);
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

void SpinBox::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (!isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

Dial::Dial(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    connect(this, &QQuickItem::enabledChanged, this, &Dial::syncState);
}

void Dial::setValue(qreal v)
{
    if (qFuzzyCompare(m_value, v))
        return;
    m_value = v;
    emit valueChanged();
}

void Dial::setFrom(qreal v)
{
    if (qFuzzyCompare(m_from, v))
        return;
    m_from = v;
    emit fromChanged();
}

void Dial::setTo(qreal v)
{
    if (qFuzzyCompare(m_to, v))
        return;
    m_to = v;
    emit toChanged();
}

void Dial::setStepSize(qreal v)
{
    if (qFuzzyCompare(m_stepSize, v))
        return;
    m_stepSize = v;
    emit stepSizeChanged();
}

void Dial::setDisabled(bool v)
{
    if (m_disabled == v)
        return;
    m_disabled = v;
    emit disabledChanged();
    syncState();
}

void Dial::setPressed(bool v)
{
    if (m_pressed == v)
        return;
    m_pressed = v;
    emit pressedChanged();
    syncState();
}

void Dial::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-dial"), kDialBody);
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

void Dial::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (m_pressed)
        state << QStringLiteral("active");
    if (m_disabled || !isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

} // namespace SolidWidgets
