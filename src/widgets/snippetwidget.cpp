#include "snippetwidget.h"

#include "qmlcss/componentcache.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>

namespace SolidWidgets {

QQuickItem *composeInternal(QQuickItem *widget, QQmlListProperty<QObject> slot,
                            const QString &key, const char *qml)
{
    QQmlEngine *eng = qmlEngine(widget);
    if (!eng)
        return nullptr;
    QQmlComponent *comp = QmlCss::cachedComponent(eng, key, qml);
    // `root` = the C++ widget: the snippet's bindings resolve it through this context and stay
    // live against the widget's NOTIFY signals.
    auto *ctx = new QQmlContext(qmlContext(widget), widget);
    ctx->setContextProperty(QStringLiteral("root"), widget);
    QObject *o = comp->create(ctx);
    if (!o) {
        qWarning("SolidWidgets: snippet '%s' failed: %s", qPrintable(key), qPrintable(comp->errorString()));
        return nullptr;
    }
    auto *item = qobject_cast<QQuickItem *>(o);
    if (!item) {
        delete o;
        return nullptr;
    }
    item->setParent(widget);
    slot.append(&slot, item);
    return item;
}

} // namespace SolidWidgets
