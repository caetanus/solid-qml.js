#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>
#include <QQmlListProperty>

// TabBar (port of TabBar.qml). The C++ wrapper owns the controlled `currentIndex` (RestoreNone
// Binding forwards root→control, onCurrentIndexChanged mirrors back) and forwards its DEFAULT
// property (`tabs`) into the control's contentData — appends that arrive before the snippet
// composes are buffered and flushed in componentComplete. The T.TabBar with the Basic-style
// horizontal ListView contentItem, the stretch-to-CSS contentHeight and the wrap-around arrow
// stepping rides the original QML body as a `root`-bound snippet.
//
// NOTE: TabButton stays a .qml composite — its ROOT is a T.TabButton, and QQuickTabBar manages
// checked/currentIndex through qobject_cast<QQuickTabButton*> (a PRIVATE Qt class). Subclassing
// it means a QtQuickTemplates2-private dependency — owner's call, parked.
namespace SolidWidgets {

class TabBar : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QQmlListProperty<QObject> tabs READ tabs CONSTANT)
    Q_CLASSINFO("DefaultProperty", "tabs")

public:
    explicit TabBar(QQuickItem *parent = nullptr);

    int currentIndex() const { return m_currentIndex; }
    void setCurrentIndex(int v);
    QQmlListProperty<QObject> tabs();

signals:
    void currentIndexChanged();

protected:
    void componentComplete() override;

private:
    static void tabs_append(QQmlListProperty<QObject> *prop, QObject *obj);
    static qsizetype tabs_count(QQmlListProperty<QObject> *prop);
    static QObject *tabs_at(QQmlListProperty<QObject> *prop, qsizetype index);

    void appendTab(QObject *obj);

    int m_currentIndex = 0;
    QList<QObject *> m_pendingTabs; // buffered until the T.TabBar snippet composes
    QPointer<QQuickItem> m_control;
};

} // namespace SolidWidgets
