#pragma once

#include <QHash>
#include <QJSValue>
#include <QObject>

class QTimer;
class QQmlEngine;

// WHATWG timer globals for the V4 engine (which has none): setTimeout / setInterval /
// clearTimeout / clearInterval, backed by QTimer (single-shot for setTimeout, repeating for
// setInterval). install() also adds queueMicrotask (via Promise). Extra-argument forwarding
// and default delay are handled by the thin JS wrapper install() evaluates.
class WebTimers final : public QObject {
    Q_OBJECT

public:
    explicit WebTimers(QObject *parent = nullptr);

    static void install(QQmlEngine *engine);

    Q_INVOKABLE int setTimeout(const QJSValue &callback, int delay);
    Q_INVOKABLE int setInterval(const QJSValue &callback, int delay);
    Q_INVOKABLE void clearTimeout(int id);

private:
    struct Entry {
        QTimer *timer = nullptr;
        QJSValue callback;
        bool once = true;
    };
    int start(const QJSValue &callback, int delay, bool once);
    void fire(int id);

    int m_nextId = 1;
    QHash<int, Entry> m_timers;
};
