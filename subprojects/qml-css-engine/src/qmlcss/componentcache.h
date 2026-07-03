#pragma once

#include <QString>

class QQmlComponent;
class QQmlEngine;

namespace QmlCss {

// One compiled QQmlComponent per (engine, key), reused for every create(). The per-instance
// `QQmlComponent comp(eng); comp.setData(...)` pattern recompiled the SAME snippet for every
// element — opening one dashboard page compiled the render shell 80+ times (~seconds of CPU).
// The component is parented to the engine (freed with it); the map entry is dropped on
// engine destruction.
QQmlComponent *cachedComponent(QQmlEngine *engine, const QString &key, const char *qml);

} // namespace QmlCss
