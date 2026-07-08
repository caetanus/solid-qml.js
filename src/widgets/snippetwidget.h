#pragma once

#include <QQmlListProperty>
#include <QString>

class QQuickItem;

// Compose a widget's internal visual subtree from a cached QML snippet, with `root` injected as
// a context property — the snippet's bindings track the C++ widget's NOTIFY properties, so a
// visual-tree-heavy port keeps its reactive internals verbatim while the class, its property
// surface and its logic live in C++ (plan: widgets-to-cpp). The created item is appended to the
// widget's CONTENT slot (the engine's layout holder), exactly like a declared child.
namespace SolidWidgets {

QQuickItem *composeInternal(QQuickItem *widget, QQmlListProperty<QObject> slot,
                            const QString &key, const char *qml);

} // namespace SolidWidgets
