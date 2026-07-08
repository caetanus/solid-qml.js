#include "containerbase.h"

#include <QQmlListReference>

namespace SolidWidgets {

SlotContainer::SlotContainer(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
}

QQmlListProperty<QObject> SlotContainer::slotChildren()
{
    return QQmlListProperty<QObject>(this, nullptr, slot_append, slot_count, slot_at, nullptr);
}

void SlotContainer::slot_append(QQmlListProperty<QObject> *prop, QObject *obj)
{
    static_cast<SlotContainer *>(prop->object)->appendSlotChild(obj);
}

qsizetype SlotContainer::slot_count(QQmlListProperty<QObject> *prop)
{
    auto *self = static_cast<SlotContainer *>(prop->object);
    if (self->m_control) {
        QQmlListReference content(self->m_control, "contentData");
        return content.count();
    }
    return self->m_pending.size();
}

QObject *SlotContainer::slot_at(QQmlListProperty<QObject> *prop, qsizetype index)
{
    auto *self = static_cast<SlotContainer *>(prop->object);
    if (self->m_control) {
        QQmlListReference content(self->m_control, "contentData");
        return content.at(index);
    }
    return self->m_pending.value(index);
}

void SlotContainer::appendSlotChild(QObject *obj)
{
    if (m_control) {
        QQmlListReference content(m_control, "contentData");
        content.append(obj);
        return;
    }
    m_pending.append(obj);
}

void SlotContainer::adoptControl(QQuickItem *control)
{
    m_control = control;
    if (!m_control)
        return;
    QQmlListReference content(m_control, "contentData");
    for (QObject *obj : std::as_const(m_pending))
        content.append(obj);
    m_pending.clear();
}

} // namespace SolidWidgets
