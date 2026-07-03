#include "qmlcss/cssrepeater.h"

#include <QDebug>
#include <QJSValue>
#include <QMetaMethod>
#include <QQmlContext>
#include <QQmlEngine>

namespace {

// The model arrives from QML bindings as a QJSValue array; normalize to a QVariantList.
QVariantList toList(const QVariant &v)
{
    if (v.metaType().id() == qMetaTypeId<QJSValue>())
        return toList(v.value<QJSValue>().toVariant());
    if (v.metaType().id() == QMetaType::QVariantList)
        return v.toList();
    if (v.metaType().id() == QMetaType::QStringList) {
        QVariantList out;
        for (const QString &s : v.toStringList())
            out.append(s);
        return out;
    }
    // A number N behaves like Repeater's integer model: N empty rows.
    bool ok = false;
    const int n = v.toInt(&ok);
    if (ok && n > 0) {
        QVariantList out;
        for (int i = 0; i < n; ++i)
            out.append(i);
        return out;
    }
    return {};
}

} // namespace

CssRepeater::CssRepeater(QQuickItem *parent)
    : QQuickItem(parent)
{
    setVisible(false);
    setFlag(ItemHasContents, false);
}

CssRepeater::~CssRepeater()
{
    for (Row &row : m_rows)
        destroyRow(row);
    m_rows.clear();
}

void CssRepeater::setModel(const QVariant &v)
{
    m_model = v;
    emit modelChanged();
    if (isComponentComplete())
        rebuild();
}

void CssRepeater::setDelegate(QQmlComponent *c)
{
    if (m_delegate == c)
        return;
    m_delegate = c;
    emit delegateChanged();
    if (isComponentComplete())
        rebuild();
}

void CssRepeater::componentComplete()
{
    QQuickItem::componentComplete();
    rebuild();
}

void CssRepeater::destroyRow(Row &row)
{
    if (row.item) {
        row.item->setParentItem(nullptr);
        row.item->deleteLater();
    }
    if (row.context)
        row.context->deleteLater();
}

void CssRepeater::rebuild()
{
    if (!m_delegate)
        return;
    const QVariantList data = toList(m_model);

    // Reconcile by VALUE, stable first-match: surviving entries keep their delegate.
    QVector<Row> next;
    next.reserve(data.size());
    QVector<bool> used(m_rows.size(), false);
    bool changed = false;
    for (int i = 0; i < data.size(); ++i) {
        int found = -1;
        for (int j = 0; j < m_rows.size(); ++j) {
            if (!used[j] && m_rows[j].item && m_rows[j].data == data.at(i)) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            used[found] = true;
            Row row = m_rows[found];
            if (row.context)
                row.context->setContextProperty(QStringLiteral("index"), i);
            if (found != i)
                changed = true;
            next.append(row);
        } else {
            Row row;
            row.data = data.at(i);
            // createRow stores the context via qmlContext(item) — keep our own pointer
            // for index updates on later moves.
            QQmlContext *creation = m_delegate->creationContext();
            if (!creation)
                creation = qmlContext(this);
            auto *ctx = new QQmlContext(creation, this);
            ctx->setBaseUrl(creation->baseUrl());
            ctx->setContextProperty(QStringLiteral("modelData"), data.at(i));
            ctx->setContextProperty(QStringLiteral("index"), i);
            QObject *o = m_delegate->create(ctx);
            auto *item = qobject_cast<QQuickItem *>(o);
            if (!item) {
                if (o)
                    o->deleteLater();
                delete ctx;
                qWarning("CssRepeater: failed to create delegate: %s",
                         qPrintable(m_delegate->errorString()));
                continue;
            }
            item->setParent(this);
            item->setParentItem(parentItem() ? parentItem() : this);
            row.item = item;
            row.context = ctx;
            next.append(row);
            changed = true;
        }
    }
    for (int j = 0; j < m_rows.size(); ++j) {
        if (!used[j]) {
            destroyRow(m_rows[j]);
            changed = true;
        }
    }
    m_rows = next;
    if (changed) {
        restack();
        notifyLayout();
    }
}

void CssRepeater::restack()
{
    // Document order == childItems order: place each row right after US in model order, so
    // the layout engine walks them in sequence. stackAfter chains keep it O(n).
    QQuickItem *anchor = this;
    for (Row &row : m_rows) {
        if (!row.item)
            continue;
        row.item->stackAfter(anchor);
        anchor = row.item;
    }
}

void CssRepeater::notifyLayout()
{
    // Ask the owning box to relayout (same duck-typed climb the engine uses).
    QQuickItem *p = parentItem();
    for (int hops = 0; p && hops < 5; ++hops, p = p->parentItem()) {
        const QMetaObject *mo = p->metaObject();
        const int idx = mo->indexOfMethod("requestRelayout()");
        if (idx >= 0) {
            mo->method(idx).invoke(p);
            return;
        }
    }
}
