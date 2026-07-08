#include "drawer.h"

#include "snippetwidget.h"

#include <QQmlListReference>

namespace {

// Original Drawer.qml internals — `root` = the C++ wrapper.
//
// T.Drawer carries no built-in open/close animation — the STYLE must supply enter/exit
// transitions that drive `position` 0↔1 (without them `open()`/`visible:true` set `opened:true`
// but `position` stays 0, leaving the panel off-screen). The cross axis is NOT sized
// automatically by QQuickDrawer, so left/right take width = overlay.width × size (default 0.34),
// full height; top/bottom transposed.
const char *kDrawerBody = R"(import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.Drawer {
    id: ctl
    parent: T.Overlay.overlay
    edge: root.edge
    readonly property bool __horizontal: root.edge === Qt.LeftEdge || root.edge === Qt.RightEdge
    // Keyboard model: a drawer is a modal panel — it takes focus when it opens (Tab stays
    // inside; the first focusable child is reachable immediately) and Esc closes it
    // (CloseOnEscape is in the default closePolicy, but only a focused popup sees the key).
    modal: true
    focus: true
    // Desktop semantics: no edge-swipe open (an interactive drag would fight the controlled Binding).
    dragMargin: 0
    width: parent ? (__horizontal ? parent.width * root.size : parent.width) : 0
    height: parent ? (__horizontal ? parent.height : parent.height * root.size) : 0
    enter: Transition { NumberAnimation { property: "position"; to: 1.0; duration: 220; easing.type: Easing.OutCubic } }
    exit: Transition { NumberAnimation { property: "position"; to: 0.0; duration: 180; easing.type: Easing.InCubic } }
    onVisibleChanged: root.open = visible
    onClosed: root.closed()

    Binding on visible {
        value: root.open
        restoreMode: Binding.RestoreNone
    }

    // Semi-transparent modal scrim (default style provides none → an opaque dim).
    T.Overlay.modal: Rectangle { color: "#66000000" }
    background: Css.CssFill {
        property Item cssAncestor: root
        cssPrimitive: "div"
        cssClass: ["panel"]
    }
    contentItem: Css.CssFill {
        property Item cssAncestor: root
        cssPrimitive: "div"
        cssClass: ["content"]
    }
}
)";

} // namespace

namespace SolidWidgets {

Drawer::Drawer(QQuickItem *parent)
    : QmlCss::CssItem(parent)
{
    setCssPrimitive(QStringLiteral("drawer"));
}

void Drawer::setEdge(int v)
{
    if (m_edge == v)
        return;
    m_edge = v;
    emit edgeChanged();
}

void Drawer::setSize(qreal v)
{
    if (qFuzzyCompare(m_size, v))
        return;
    m_size = v;
    emit sizeChanged();
}

void Drawer::setOpen(bool v)
{
    if (m_open == v)
        return;
    m_open = v;
    emit openChanged();
}

QQmlListProperty<QObject> Drawer::contentSlot()
{
    return QQmlListProperty<QObject>(this, nullptr, content_append, content_count, content_at, nullptr);
}

void Drawer::content_append(QQmlListProperty<QObject> *prop, QObject *obj)
{
    static_cast<Drawer *>(prop->object)->appendContent(obj);
}

qsizetype Drawer::content_count(QQmlListProperty<QObject> *prop)
{
    auto *self = static_cast<Drawer *>(prop->object);
    if (self->m_contentBox) {
        QQmlListReference content(self->m_contentBox, "content");
        return content.count();
    }
    return self->m_pending.size();
}

QObject *Drawer::content_at(QQmlListProperty<QObject> *prop, qsizetype index)
{
    auto *self = static_cast<Drawer *>(prop->object);
    if (self->m_contentBox) {
        QQmlListReference content(self->m_contentBox, "content");
        return content.at(index);
    }
    return self->m_pending.value(index);
}

void Drawer::appendContent(QObject *obj)
{
    if (m_contentBox) {
        QQmlListReference content(m_contentBox, "content");
        content.append(obj);
        return;
    }
    m_pending.append(obj);
}

void Drawer::componentComplete()
{
    QmlCss::CssItem::componentComplete();
    QObject *control = composeInternalPlain(this, QStringLiteral("solidwidgets-drawer"), kDrawerBody);
    if (control) {
        m_contentBox = control->property("contentItem").value<QQuickItem *>();
        if (m_contentBox) {
            QQmlListReference content(m_contentBox, "content");
            for (QObject *obj : std::as_const(m_pending))
                content.append(obj);
            m_pending.clear();
        }
    }
}

} // namespace SolidWidgets
