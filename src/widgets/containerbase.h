#pragma once

#include "qmlcss/cssfill.h"

#include <QPointer>
#include <QQmlListProperty>

// Shared base for container widgets whose DEFAULT property routes the author's children into a
// composed control's contentData (TabBar tabs, SplitView panes, SwipeView pages). Appends that
// arrive before the snippet composes (QML instantiation order: children first, componentComplete
// after) are buffered; adoptControl() flushes them and later appends (For-generated) forward
// directly via QQmlListReference. Q_CLASSINFO is inherited — subclasses keep the default slot.
namespace SolidWidgets {

class SlotContainer : public QmlCss::CssFill {
    Q_OBJECT
    Q_PROPERTY(QQmlListProperty<QObject> slotChildren READ slotChildren CONSTANT)
    Q_CLASSINFO("DefaultProperty", "slotChildren")

public:
    explicit SlotContainer(QQuickItem *parent = nullptr);

    QQmlListProperty<QObject> slotChildren();

protected:
    // Take ownership of the composed control: flush the buffered children into its contentData.
    void adoptControl(QQuickItem *control);

    QPointer<QQuickItem> m_control;

private:
    static void slot_append(QQmlListProperty<QObject> *prop, QObject *obj);
    static qsizetype slot_count(QQmlListProperty<QObject> *prop);
    static QObject *slot_at(QQmlListProperty<QObject> *prop, qsizetype index);

    void appendSlotChild(QObject *obj);

    QList<QObject *> m_pending;
};

} // namespace SolidWidgets
