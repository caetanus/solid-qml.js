#include "qmlcss/componentcache.h"

#include <QHash>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

namespace QmlCss {

QQmlComponent *cachedComponent(QQmlEngine *engine, const QString &key, const char *qml)
{
    // QML engines live on one thread; a plain static map suffices (no locking on the hot path).
    static QHash<QQmlEngine *, QHash<QString, QQmlComponent *>> cache;

    auto engineIt = cache.find(engine);
    if (engineIt == cache.end()) {
        engineIt = cache.insert(engine, {});
        QObject::connect(engine, &QObject::destroyed, [engine]() { cache.remove(engine); });
    }
    QQmlComponent *&slot = (*engineIt)[key];
    if (!slot) {
        slot = new QQmlComponent(engine, engine); // parented to the engine — freed with it
        slot->setData(qml, QUrl()); // empty base URL — a foreign scheme leaves the component pending
    }
    return slot;
}

} // namespace QmlCss
