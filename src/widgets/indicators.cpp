#include "indicators.h"

#include "snippetwidget.h"

namespace {

// The original Progress.qml internals, verbatim — bound to the C++ `root`.
const char *kProgressBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.ProgressBar {
    id: bar
    anchors.fill: parent
    contentItem: null
    background: null
    from: 0
    to: root.max
    indeterminate: root.value < 0
    value: root.value < 0 ? 0 : root.value

    Css.CssRect {
        cssPrimitive: ""
        cssClass: ["track"]
        anchors.fill: parent
        Item {
            anchors.fill: parent
            Rectangle {
                width: bar.indeterminate ? parent.width * 0.3 : bar.visualPosition * parent.width
                height: parent.height
                color: "#176b87"
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["bar"] }
                NumberAnimation on x {
                    running: bar.indeterminate
                    from: 0
                    to: bar.width * 0.7
                    duration: 1200
                    loops: Animation.Infinite
                }
            }
        }
    }
}
)";

// The original BusyIndicator.qml spoke ring, verbatim (eight staggered spokes on a circle,
// the host spun while `root.running`).
const char *kBusyBody = R"(import QtQuick
import qmlcss 1.0 as Css

Item {
    anchors.fill: parent
    visible: root.running
    Item {
        anchors.centerIn: parent
        width: 40; height: 40
        property real __r: Math.min(width, height) / 2 - 5
        id: __ring
        RotationAnimation on rotation {
            from: 0; to: 360; duration: 900
            loops: Animation.Infinite
            running: root.running
        }
        Repeater {
            model: 8
            delegate: Rectangle {
                width: 6; height: 6; radius: 3; color: "#176b87"
                opacity: (index + 1) / 8
                x: __ring.width / 2 + Math.cos(index * Math.PI / 4) * __ring.__r - width / 2
                y: __ring.height / 2 + Math.sin(index * Math.PI / 4) * __ring.__r - height / 2
                Css.CssItem { cssPrimitive: "rect"; cssClass: ["spoke"] }
            }
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

Progress::Progress(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("progress"));
    // Fixed implicit size (Templates have no size policy — we ARE the style). CSS overrides.
    setImplicitWidth(200);
    setImplicitHeight(8);
}

void Progress::setValue(qreal v)
{
    if (qFuzzyCompare(m_value, v))
        return;
    m_value = v;
    emit valueChanged();
    syncState();
}

void Progress::setMax(qreal v)
{
    if (qFuzzyCompare(m_max, v))
        return;
    m_max = v;
    emit maxChanged();
}

void Progress::syncState()
{
    setCssState(m_value < 0 ? QVariantList{ QStringLiteral("indeterminate") } : QVariantList{});
}

void Progress::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    composeInternal(this, content(), QStringLiteral("solidwidgets-progress"), kProgressBody);
    syncState();
}

BusyIndicator::BusyIndicator(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QString());
    setImplicitWidth(40);
    setImplicitHeight(40);
}

void BusyIndicator::setRunning(bool v)
{
    if (m_running == v)
        return;
    m_running = v;
    emit runningChanged();
}

void BusyIndicator::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    composeInternal(this, content(), QStringLiteral("solidwidgets-busy"), kBusyBody);
}

} // namespace SolidWidgets
