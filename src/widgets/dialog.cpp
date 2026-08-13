#include "dialog.h"

#include "snippetwidget.h"

#include <QQmlListReference>

namespace {

// Original Dialog.qml internals — `root` = the C++ page anchor. A separate window severs the CSS
// ancestor chain, so the Css root carries `cssAncestor: root` to re-anchor the engine's ancestor
// walk back at the page anchor. `__cssRoot` hands the content receiver back to C++.
const char *kDialogBody = R"(import QtQuick
import QtQuick.Window
import solidqml.Widgets 1.0 as W
import qmlcss 1.0 as Css

Window {
    id: dlg
    flags: Qt.Dialog
    modality: Qt.WindowModal
    transientParent: root.pageWindow
    title: root.title
    visible: root.open
    readonly property Item __cssRoot: rootBox
    // Size the window to its content (the Css root's implicit size). Not circular: the root's
    // implicit is content-driven, its actual size comes back via anchors.fill.
    width: Math.max(1, rootBox.implicitWidth)
    height: Math.max(1, rootBox.implicitHeight)
    onClosing: root.dialogClosed()

    // Find the default button (<button type="submit"> → isDefault) anywhere in the dialog
    // content, so Enter fires it (study §6). Recursive; non-buttons lack `isDefault`.
    function __findDefault(item) {
        if (!item || !item.children)
            return null;
        for (var k = 0; k < item.children.length; k++) {
            var c = item.children[k];
            if (c && c.isDefault === true)
                return c;
            var f = __findDefault(c);
            if (f)
                return f;
        }
        return null;
    }

    // Tab-stop is born by default in the dialog too (study §6, owner call): a modal opens with
    // keyboard focus already placed, so Tab works and the ring shows without a click. Prefer the
    // default button (<button type="submit">) so Enter confirms it (desktop autoDefault); else the
    // first focusable. A separate window has its own focus chain, independent of the app root.
    onVisibleChanged: if (visible && solidTabstop.enabled) Qt.callLater(function() {
        var def = dlg.__findDefault(rootBox);
        if (def && def.takeFocus) { def.takeFocus(); return; }
        var f = dlg.contentItem.nextItemInFocusChain(true);
        if (f) f.forceActiveFocus(Qt.TabFocusReason);
    })

    // Esc cancels the dialog (desktop-essential for keyboard-only use, study §6). A QtQuick
    // Window does not close on Escape by default, so drive it explicitly: fire dialogClosed →
    // the author's onClose sets `open` false → the Binding hides the window.
    Shortcut {
        sequences: ["Escape"]
        enabled: dlg.visible
        onActivated: root.dialogClosed()
    }

    Css.CssRect {
        id: rootBox
        anchors.fill: parent
        // Re-anchor the CSS ancestor walk at the page wrapper: the dialog lives in a separate
        // window, so without this scoped rules (`.native .dialog …`) and inheritance don't reach it.
        property Item cssAncestor: root
        cssPrimitive: "dialog"
        cssClass: root.cssClass

        // Enter fires the default button — but only when it BUBBLES up to here, i.e. the focused
        // item didn't consume it. A focused button handles Enter itself (activating THAT button)
        // and accepts the event, so it never reaches this handler — no double-fire, and Enter on
        // a non-default button still activates just that button, matching the desktop model (§6).
        Keys.onReturnPressed: (event) => { var b = dlg.__findDefault(rootBox); if (b) { b.clicked(); event.accepted = true; } }
        Keys.onEnterPressed: (event) => { var b = dlg.__findDefault(rootBox); if (b) { b.clicked(); event.accepted = true; } }
    }

    // No focus-ring overlay here either: controls inside the dialog paint their own
    // `:focus { outline }` ring, so a per-window tracker is unnecessary.

    // Controlled open state: survives the imperative visible=false a window self-close performs.
    Binding {
        target: dlg
        property: "visible"
        value: root.open
        restoreMode: Binding.RestoreNone
    }
}
)";

} // namespace

namespace SolidWidgets {

Dialog::Dialog(QQuickItem *parent)
    : QQuickItem(parent)
{
}

void Dialog::setOpen(bool v)
{
    if (m_open == v)
        return;
    m_open = v;
    emit openChanged();
}

void Dialog::setTitle(const QString &v)
{
    if (m_title == v)
        return;
    m_title = v;
    emit titleChanged();
}

void Dialog::setCssClass(const QVariant &v)
{
    if (m_cssClass == v)
        return;
    m_cssClass = v;
    emit cssClassChanged();
}

QQmlListProperty<QObject> Dialog::contentSlot()
{
    return QQmlListProperty<QObject>(this, nullptr, content_append, nullptr, nullptr, nullptr);
}

void Dialog::content_append(QQmlListProperty<QObject> *prop, QObject *obj)
{
    static_cast<Dialog *>(prop->object)->appendContent(obj);
}

void Dialog::appendContent(QObject *obj)
{
    if (m_cssRoot) {
        QQmlListReference content(m_cssRoot, "content");
        content.append(obj);
        return;
    }
    m_pending.append(obj);
}

void Dialog::itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change == QQuickItem::ItemSceneChange)
        emit pageWindowChanged();
}

void Dialog::componentComplete()
{
    QQuickItem::componentComplete();
    QObject *dlg = composeInternalPlain(this, QStringLiteral("solidwidgets-dialog"), kDialogBody);
    if (dlg) {
        m_cssRoot = dlg->property("__cssRoot").value<QQuickItem *>();
        if (m_cssRoot) {
            QQmlListReference content(m_cssRoot, "content");
            for (QObject *obj : std::as_const(m_pending))
                content.append(obj);
            m_pending.clear();
        }
    }
}

} // namespace SolidWidgets
