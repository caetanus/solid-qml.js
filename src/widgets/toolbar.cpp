#include "toolbar.h"

#include "snippetwidget.h"

namespace {

// Original ToolBar.qml internals: the T.ToolBar's contentItem is an empty Item so the template
// never instantiates style-less chrome of its own.
const char *kToolBarBody = R"(import QtQuick
import QtQuick.Templates as T

T.ToolBar {
    anchors.fill: parent
    background: null
    contentItem: Item { }
}
)";

// Original ToolSeparator.qml internals. The host Item insulates the centring anchors from any
// CSS pass and gives the control its implicit metrics.
const char *kToolSeparatorBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.ToolSeparator {
    id: __ctl
    anchors.fill: parent
    background: null
    // Basic-style implicit size: Templates leave implicit sizes to the style (us).
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    contentItem: Item {
        implicitWidth: 9
        implicitHeight: 28
        Css.CssRect {
            cssClass: ["sep"]
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: parent.height * 0.6
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

ToolBar::ToolBar(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("toolbar"));
}

void ToolBar::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    composeInternal(this, content(), QStringLiteral("solidwidgets-toolbar"), kToolBarBody);
}

ToolSeparator::ToolSeparator(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QString());
}

void ToolSeparator::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    m_control = composeInternal(this, content(), QStringLiteral("solidwidgets-toolseparator"), kToolSeparatorBody);
    if (m_control) {
        const auto mirror = [this] {
            setImplicitWidth(m_control->implicitWidth());
            setImplicitHeight(m_control->implicitHeight());
        };
        connect(m_control, &QQuickItem::implicitWidthChanged, this, mirror);
        connect(m_control, &QQuickItem::implicitHeightChanged, this, mirror);
        mirror();
    }
}

} // namespace SolidWidgets
