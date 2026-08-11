// M-AOT-0 hand-written proof — the SHAPE the C++ emitter (transpiler/src/emit/cpp) will generate,
// written by hand first to establish the runtime mechanics (context, lifecycle, layout, grab)
// independently of codegen. Once this renders pixel-identical to the QML back-end, the emitter
// reproduces this pattern from the AST.
//
// Mirrors Counter.qml: `property var label; property var count: 0` → a QObject state class with one
// QVariant Q_PROPERTY per reactive cell (decision #1: QVariant-first). Bindings/handlers connect to
// the *Changed signals in the build function.
#pragma once

#include <QObject>
#include <QVariant>
#include <QtQml/qqml.h>

class QQmlContext;
class QQuickItem;

namespace aot {

class CounterState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariant label READ label WRITE setLabel NOTIFY labelChanged)
    Q_PROPERTY(QVariant count READ count WRITE setCount NOTIFY countChanged)
public:
    explicit CounterState(QObject *parent = nullptr) : QObject(parent) {}

    QVariant label() const { return m_label; }
    void setLabel(const QVariant &v)
    {
        if (m_label == v)
            return;
        m_label = v;
        emit labelChanged();
    }

    QVariant count() const { return m_count; }
    void setCount(const QVariant &v)
    {
        if (m_count == v)
            return;
        m_count = v;
        emit countChanged();
    }

signals:
    void labelChanged();
    void countChanged();

private:
    QVariant m_label;
    QVariant m_count = 0; // `property var count: 0`
};

// Build the Counter component subtree for one instance (state already seeded). Returns the completed
// root Div.
QQuickItem *buildCounter(QQmlContext *ctx, CounterState *state);

// Build the app root (window box + app div + three counters). Returns the window-primitive CssRect
// to host in the QQuickWindow. Mirrors App.generated.qml minus the Window/Tabstop chrome (which the
// AOT app_main owns).
QQuickItem *buildCounterApp(QQmlContext *ctx);

} // namespace aot
