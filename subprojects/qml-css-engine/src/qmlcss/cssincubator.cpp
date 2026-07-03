#include "qmlcss/cssincubator.h"

#include <QDebug>
#include <QQmlContext>
#include <QQmlEngine>

CssIncubator::CssIncubator(QQuickItem *parent)
    : QQuickItem(parent)
{
    // Never a layout participant and never painted: a zero-size bookkeeping node.
    setVisible(false);
    setFlag(ItemHasContents, false);
}

CssIncubator::~CssIncubator()
{
    teardown();
}

void CssIncubator::Incubator::statusChanged(Status s)
{
    if (s == Ready)
        m_owner->ready();
    else if (s == Error) {
        for (const QQmlError &e : errors())
            qWarning().noquote() << "CssIncubator:" << e.toString();
    }
}

void CssIncubator::setActive(bool v)
{
    if (m_active == v)
        return;
    m_active = v;
    emit activeChanged();
    if (isComponentComplete())
        sync();
}

void CssIncubator::setSourceComponent(QQmlComponent *c)
{
    if (m_component == c)
        return;
    m_component = c;
    emit sourceComponentChanged();
    if (isComponentComplete())
        sync();
}

void CssIncubator::componentComplete()
{
    QQuickItem::componentComplete();
    sync();
}

void CssIncubator::sync()
{
    if (m_active && m_component) {
        if (m_item || m_incubator)
            return; // already incubating / mounted
        m_incubator = new Incubator(this);
        // Mirror Loader: instantiate in a CHILD of the component's creation context, so
        // bare names (the defining component's properties — __const_* tables, signals)
        // resolve through the enclosing scope chain exactly like a declared child's.
        QQmlContext *creation = m_component->creationContext();
        if (!creation)
            creation = qmlContext(this);
        auto *ctx = new QQmlContext(creation, this);
        // A fresh child context has no base URL of its own — relative asset paths
        // (Image sources) must keep resolving against the defining document.
        ctx->setBaseUrl(creation->baseUrl());
        m_component->create(*m_incubator, ctx);
        // Synchronous outcome (cached + trivial component, or error) resolves immediately
        // via statusChanged; nothing else to do here.
    } else {
        teardown();
    }
}

void CssIncubator::ready()
{
    QQuickItem *item = qobject_cast<QQuickItem *>(m_incubator ? m_incubator->object() : nullptr);
    delete m_incubator;
    m_incubator = nullptr;
    if (!item)
        return;
    if (!m_active) { // deactivated while incubating
        item->deleteLater();
        return;
    }
    item->setParent(this);
    // The Repeater trick: the item becomes a DIRECT child of our parent (the content
    // holder), so the layout engine treats it exactly like a declared child.
    item->setParentItem(parentItem() ? parentItem() : this);
    m_item = item;
    emit itemChanged();
}

void CssIncubator::teardown()
{
    if (m_incubator) {
        m_incubator->clear(); // cancels a pending incubation
        delete m_incubator;
        m_incubator = nullptr;
    }
    if (m_item) {
        m_item->setParentItem(nullptr);
        m_item->deleteLater();
        m_item = nullptr;
        emit itemChanged();
    }
}
