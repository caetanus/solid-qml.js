#include "tooltip.h"

#include "snippetwidget.h"

namespace {

// `root` = the C++ wrapper; the TOOLTIP anchors at root.parent, the titled element (the wrapper
// itself is a 0x0 child). In-scene overlay popup (Popup.Window mis-positions on Wayland once the
// app window floats — the DateField precedent), below the element, centred, flipped above when it
// would leave the window. Templates popups have no implicit-size policy: the style supplies it.
const char *kToolTipBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.ToolTip {
    id: tip
    parent: root.parent
    text: root.text
    visible: root.shown && root.text.length > 0
    delay: 600
    popupType: T.Popup.Item
    margins: 6          // stays inside the window at its edges
    implicitWidth: contentWidth + leftPadding + rightPadding
    implicitHeight: contentHeight + topPadding + bottomPadding
    topPadding: 4
    bottomPadding: 4
    leftPadding: 8
    rightPadding: 8
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? ((parent.mapToItem(null, 0, parent.height + 6).y + height > (parent.Window.height || Screen.height))
                 ? -(height + 6) : parent.height + 6) : 0
    // cssAncestor: overlay reparenting severs the visual chain — re-anchor at the titled element.
    background: Css.CssFill {
        property Item cssAncestor: root.parent
        cssPrimitive: "div"
        cssClass: ["tooltip"]
    }
    contentItem: Css.CssText {
        property Item cssAncestor: root.parent
        cssPrimitive: ""
        cssClass: ["tooltip-text"]
        text: tip.text
    }
}
)";

} // namespace

namespace SolidWidgets {

ToolTip::ToolTip(QQuickItem *parent)
    : QQuickItem(parent)
{
}

void ToolTip::setText(const QString &v)
{
    if (m_text == v)
        return;
    m_text = v;
    emit textChanged();
}

void ToolTip::setShown(bool v)
{
    if (m_shown == v)
        return;
    m_shown = v;
    emit shownChanged();
}

void ToolTip::componentComplete()
{
    QQuickItem::componentComplete();
    composeInternalPlain(this, QStringLiteral("solidwidgets-tooltip"), kToolTipBody);
}

} // namespace SolidWidgets
