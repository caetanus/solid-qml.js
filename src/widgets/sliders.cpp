#include "sliders.h"

#include "snippetwidget.h"

namespace {

// Original Slider.qml internals: T.Slider + Basic-style track/fill/handle + the focused-wheel
// stepping. `root` = the C++ wrapper; the RestoreNone Binding forwards the controlled value.
const char *kSliderBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.Slider {
    id: ctl
    anchors.fill: parent
    activeFocusOnTab: solidTabstop.enabled
    from: root.from
    to: root.to
    stepSize: root.stepSize
    onMoved: { root.value = value; root.moved(); }

    Binding on value {
        value: root.value
        restoreMode: Binding.RestoreNone
    }

    background: Css.CssFill {
        cssPrimitive: ""
        cssClass: ["track"]
        x: ctl.leftPadding
        y: ctl.topPadding + (ctl.availableHeight - height) / 2
        width: ctl.availableWidth
        height: 6
        implicitWidth: 200
        implicitHeight: 6
        Css.CssRect {
            cssClass: ["track-fill"]
            width: ctl.visualPosition * parent.width
            height: parent.height
        }
    }
    handle: Css.CssRect {
        cssClass: ["handle"]
        width: 18
        height: 18
        implicitWidth: 18
        implicitHeight: 18
        x: ctl.leftPadding + ctl.visualPosition * (ctl.availableWidth - width)
        y: ctl.topPadding + ctl.availableHeight / 2 - height / 2
    }
    WheelHandler {
        property real __acc: 0
        enabled: ctl.activeFocus
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { ctl.value = Math.max(ctl.from, Math.min(ctl.to, ctl.value + s * ctl.stepSize)); ctl.moved() } }
    }

    // Basic-style implicit size (background/handle carry the natural metrics).
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitHandleWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitHandleHeight + topPadding + bottomPadding)
}
)";

// Original RangeSlider.qml internals: two nodes, shared track, per-node handles.
const char *kRangeBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.RangeSlider {
    id: __ctl
    anchors.fill: parent
    activeFocusOnTab: solidTabstop.enabled
    from: root.from
    to: root.to
    stepSize: root.stepSize
    enabled: !root.disabled
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, first.implicitHandleWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, first.implicitHandleHeight + topPadding + bottomPadding)
    first.onMoved: root.moved()
    second.onMoved: root.moved()
    background: Css.CssFill {
        cssPrimitive: ""
        cssClass: ["track"]
        x: __ctl.leftPadding
        y: __ctl.topPadding + (__ctl.availableHeight - height) / 2
        width: __ctl.availableWidth
        height: 6
        implicitWidth: 200
        implicitHeight: 6
        Item {
            anchors.fill: parent
            Css.CssRect {
                cssClass: ["track-fill"]
                x: __ctl.first.visualPosition * parent.width
                width: (__ctl.second.visualPosition - __ctl.first.visualPosition) * parent.width
                height: parent.height
            }
        }
    }
    first.handle: Css.CssRect {
        cssClass: ["handle"]
        width: 18
        height: 18
        implicitWidth: 18
        implicitHeight: 18
        x: __ctl.leftPadding + __ctl.first.visualPosition * (__ctl.availableWidth - width)
        y: __ctl.topPadding + __ctl.availableHeight / 2 - height / 2
    }
    second.handle: Css.CssRect {
        cssClass: ["handle"]
        width: 18
        height: 18
        implicitWidth: 18
        implicitHeight: 18
        x: __ctl.leftPadding + __ctl.second.visualPosition * (__ctl.availableWidth - width)
        y: __ctl.topPadding + __ctl.availableHeight / 2 - height / 2
    }
    WheelHandler {
        property real __acc: 0
        enabled: __ctl.activeFocus
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (ev) => { __acc += ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.pixelDelta.y * 8; var s = 0; while (__acc >= 120) { __acc -= 120; s++ } while (__acc <= -120) { __acc += 120; s-- } if (s !== 0) { __ctl.first.value = Math.max(__ctl.from, Math.min(__ctl.to, __ctl.first.value + s * __ctl.stepSize)); __ctl.first.moved() } }
    }
}
)";

} // namespace

namespace SolidWidgets {

Slider::Slider(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("input"));
    connect(this, &QQuickItem::enabledChanged, this, &Slider::syncState);
}

void Slider::setValue(qreal v)
{
    if (qFuzzyCompare(m_value, v))
        return;
    m_value = v;
    emit valueChanged();
}

void Slider::setFrom(qreal v)
{
    if (qFuzzyCompare(m_from, v))
        return;
    m_from = v;
    emit fromChanged();
}

void Slider::setTo(qreal v)
{
    if (qFuzzyCompare(m_to, v))
        return;
    m_to = v;
    emit toChanged();
}

void Slider::setStepSize(qreal v)
{
    if (qFuzzyCompare(m_stepSize, v))
        return;
    m_stepSize = v;
    emit stepSizeChanged();
}

void Slider::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-slider"), kSliderBody);
    if (m_control) {
        connect(m_control, SIGNAL(activeFocusChanged(bool)), this, SLOT(syncState()));
        // Mirror the control's Basic-style implicit metrics onto the wrapper (CSS overrides).
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

void Slider::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (!isEnabled())
        state << QStringLiteral("disabled");
    setCssState(state);
}

RangeSlider::RangeSlider(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("input"));
}

void RangeSlider::setFrom(qreal v)
{
    if (qFuzzyCompare(m_from, v))
        return;
    m_from = v;
    emit fromChanged();
}

void RangeSlider::setTo(qreal v)
{
    if (qFuzzyCompare(m_to, v))
        return;
    m_to = v;
    emit toChanged();
}

void RangeSlider::setStepSize(qreal v)
{
    if (qFuzzyCompare(m_stepSize, v))
        return;
    m_stepSize = v;
    emit stepSizeChanged();
}

void RangeSlider::setDisabled(bool v)
{
    if (m_disabled == v)
        return;
    m_disabled = v;
    emit disabledChanged();
    syncState();
}

void RangeSlider::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-rangeslider"), kRangeBody);
    if (m_control) {
        // The emit binds the NODES across the boundary (Binding { target: __inputN.first }).
        m_firstNode = m_control->property("first").value<QObject *>();
        m_secondNode = m_control->property("second").value<QObject *>();
        emit nodesChanged();
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

void RangeSlider::syncState()
{
    QVariantList state;
    if (m_control && m_control->hasActiveFocus())
        state << QStringLiteral("focus");
    if (m_disabled)
        state << QStringLiteral("disabled");
    setCssState(state);
}

} // namespace SolidWidgets
