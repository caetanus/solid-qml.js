#include "splitview.h"

#include "snippetwidget.h"

namespace {

// Original SplitView.qml + SplitHandle.qml internals — `root` = the C++ wrapper. The handle's
// hover/press are read from the DELEGATE root, where SplitView drives the SplitHandle attached
// properties (referencing them in the child would attach a fresh, undriven handle).
const char *kSplitViewBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.SplitView {
    id: split
    anchors.fill: parent
    orientation: root.orientation
    // Desktop semantics (QSplitter): a splitter never paints outside itself — while dragging,
    // a pane squeezed below its content size would otherwise leak past the container border.
    clip: true
    handle: Rectangle {
        id: hroot
        color: "transparent"
        property bool horizontal: split.orientation === Qt.Horizontal
        property int thickness: 6
        property bool handleHovered: T.SplitHandle.hovered
        property bool handlePressed: T.SplitHandle.pressed
        implicitWidth: horizontal ? thickness : (parent ? parent.width : thickness)
        implicitHeight: horizontal ? (parent ? parent.height : thickness) : thickness
        Css.CssRect {
            anchors.fill: parent
            property Item cssAncestor: root
            cssPrimitive: "div"
            cssClass: ["handle"]
            cssState: (hroot.handlePressed ? ["active"] : []).concat(hroot.handleHovered ? ["hover"] : [])
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

SplitView::SplitView(QQuickItem *parent)
    : SlotContainer(parent)
{
    setCssPrimitive(QStringLiteral("splitview"));
}

void SplitView::setOrientation(int v)
{
    if (m_orientation == v)
        return;
    m_orientation = v;
    emit orientationChanged();
}

void SplitView::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    adoptControl(composeInternal(this, content(), QStringLiteral("solidwidgets-splitview"), kSplitViewBody));
}

} // namespace SolidWidgets
