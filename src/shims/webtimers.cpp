#include "webtimers.h"

#include <QDebug>
#include <QJSEngine>
#include <QQmlEngine>
#include <QTimer>

WebTimers::WebTimers(QObject *parent)
    : QObject(parent)
{
}

int WebTimers::start(const QJSValue &callback, int delay, bool once)
{
    const int id = m_nextId++;
    auto *timer = new QTimer(this);
    timer->setSingleShot(once);
    timer->setInterval(qMax(0, delay));
    m_timers.insert(id, { timer, callback, once });
    connect(timer, &QTimer::timeout, this, [this, id] { fire(id); });
    timer->start();
    return id;
}

int WebTimers::setTimeout(const QJSValue &callback, int delay)
{
    return start(callback, delay, /*once=*/true);
}

int WebTimers::setInterval(const QJSValue &callback, int delay)
{
    return start(callback, delay, /*once=*/false);
}

void WebTimers::fire(int id)
{
    const auto it = m_timers.constFind(id);
    if (it == m_timers.constEnd())
        return;
    QJSValue callback = it->callback; // keep alive across the (possibly self-clearing) call
    if (it->once)
        clearTimeout(id); // single-shot: retire before invoking (matches setTimeout semantics)
    const QJSValue r = callback.call();
    if (r.isError())
        qWarning().noquote() << "timer callback:" << r.toString();
}

void WebTimers::clearTimeout(int id)
{
    const auto it = m_timers.find(id);
    if (it == m_timers.end())
        return;
    it->timer->stop();
    it->timer->deleteLater();
    m_timers.erase(it);
}

// Thin JS layer: spec signatures (default delay, extra args forwarded to the callback) and
// queueMicrotask, over the C++ backend.
static const char *kTimersShim = R"JS(
(function (T) {
    function bind(fn, args) { return args.length ? function () { fn.apply(null, args); } : fn; }
    return {
        setTimeout: function (fn, delay) { return T.setTimeout(bind(fn, Array.prototype.slice.call(arguments, 2)), delay | 0); },
        setInterval: function (fn, delay) { return T.setInterval(bind(fn, Array.prototype.slice.call(arguments, 2)), delay | 0); },
        clearTimeout: function (id) { T.clearTimeout(id | 0); },
        clearInterval: function (id) { T.clearTimeout(id | 0); },
        queueMicrotask: function (fn) { Promise.resolve().then(fn); }
    };
})(__timersBackend)
)JS";

void WebTimers::install(QQmlEngine *engine)
{
    if (!engine)
        return;
    auto *backend = new WebTimers(engine);
    QQmlEngine::setObjectOwnership(backend, QQmlEngine::CppOwnership);

    QJSValue global = engine->globalObject();
    global.setProperty(QStringLiteral("__timersBackend"), engine->newQObject(backend));

    const QJSValue api = engine->evaluate(QString::fromUtf8(kTimersShim));
    if (api.isError()) {
        qWarning().noquote() << "timers: shim failed:" << api.toString();
        return;
    }
    for (const QString &name : { QStringLiteral("setTimeout"), QStringLiteral("setInterval"),
                                 QStringLiteral("clearTimeout"), QStringLiteral("clearInterval"),
                                 QStringLiteral("queueMicrotask") })
        global.setProperty(name, api.property(name));
}
