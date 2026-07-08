#include "tray.h"

#include "snippetwidget.h"

namespace {

// Original Tray.qml internals — `root` = the C++ wrapper.
const char *kTrayBody = R"(import QtQuick
import Qt.labs.platform 1.1 as Platform

Platform.SystemTrayIcon {
    id: tray
    visible: root.shown
    icon.source: root.iconSource
    tooltip: root.tooltip
    menu: root.menu
    onActivated: root.activated(reason)
    onAvailableChanged: root.availableChanged()
    Component.onCompleted: root.availableChanged()
}
)";

} // namespace

namespace SolidWidgets {

Tray::Tray(QQuickItem *parent)
    : QQuickItem(parent)
{
    setWidth(0);
    setHeight(0);
}

void Tray::setTooltip(const QString &v)
{
    if (m_tooltip == v)
        return;
    m_tooltip = v;
    emit tooltipChanged();
}

void Tray::setIconSource(const QUrl &v)
{
    if (m_iconSource == v)
        return;
    m_iconSource = v;
    emit iconSourceChanged();
}

void Tray::setShown(bool v)
{
    if (m_shown == v)
        return;
    m_shown = v;
    emit shownChanged();
}

void Tray::setMenu(QObject *v)
{
    if (m_menu == v)
        return;
    m_menu = v;
    emit menuChanged();
}

bool Tray::available() const
{
    return m_tray && m_tray->property("available").toBool();
}

void Tray::componentComplete()
{
    QQuickItem::componentComplete();
    m_tray = composeInternalPlain(this, QStringLiteral("solidwidgets-tray"), kTrayBody);
    emit availableChanged();
}

} // namespace SolidWidgets
