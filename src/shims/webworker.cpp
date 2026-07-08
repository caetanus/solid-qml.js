#include "webworker.h"

#include "jspolyfill.h"
#include "nodeshims.h"
#include "sharedbuffers.h"
#include "webfetch.h"
#include "weblocalstorage.h"
#include "webplatform.h"
#include "webtimers.h"

#include <QCoreApplication>
#include <QQmlEngine>

namespace SolidWorkers {

// ─── WorkerScriptHost (worker thread) ───────────────────────────────────────────────────────────

WorkerScriptHost::WorkerScriptHost(const QUrl &script, const QVariant &workerData, int threadId,
                                   QObject *parent)
    : QObject(parent)
    , m_script(script)
    , m_workerData(workerData)
    , m_threadId(threadId)
{
}

WorkerScriptHost::~WorkerScriptHost()
{
    delete m_engine;
}

void WorkerScriptHost::start()
{
    // The worker runtime = the --mini-node stack on this thread: a windowless QQmlEngine with
    // the browser/node shims. Node-module imports resolve exactly like the main engine's.
    m_engine = new QQmlEngine;
    m_engine->installExtensions(QJSEngine::ConsoleExtension);
    WebLocalStorage::install(m_engine);
    WebFetch::install(m_engine);
    WebTimers::install(m_engine);
    WebPlatform::install(m_engine);
    JsPolyfill::install(m_engine);
    NodeShims::install(m_engine);
    SharedBuffers::install(m_engine);

    // Worker-global surface: self, postMessage, onmessage/onerror slots, close().
    // globalObject().setProperty, NOT setContextProperty: plain evaluate() runs in the JS global
    // scope and does not see QML context properties. CppOwnership is MANDATORY: newQObject on a
    // parentless object hands it to the JS GC, which would double-free against the thread's
    // finished→deleteLater teardown.
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    m_engine->globalObject().setProperty(QStringLiteral("__workerHost"), m_engine->newQObject(this));
    m_engine->evaluate(QStringLiteral(R"((function () {
        globalThis.self = globalThis;
        globalThis.onmessage = null;
        globalThis.onerror = null;
        var listeners = [];
        globalThis.postMessage = function (m) { __workerHost.postToParent(m); };
        globalThis.addEventListener = function (type, fn) { if (type === "message") listeners.push(fn); };
        globalThis.removeEventListener = function (type, fn) {
            var i = listeners.indexOf(fn); if (i >= 0) listeners.splice(i, 1);
        };
        globalThis.__dispatchMessage = function (data) {
            var ev = { data: data, type: "message" };
            if (typeof globalThis.onmessage === "function") globalThis.onmessage(ev);
            for (var i = 0; i < listeners.length; i++) listeners[i](ev);
        };
    })())"));

    // node worker_threads (worker side): parentPort over the same message plumbing.
    QJSValue wtMod = m_engine->evaluate(QStringLiteral(R"((function () {
        var listeners = { message: [], error: [] };
        globalThis.addEventListener("message", function (ev) {
            for (var i = 0; i < listeners.message.length; i++) listeners.message[i](ev.data);
        });
        var parentPort = {
            postMessage: function (m) { __workerHost.postToParent(m); },
            on: function (type, fn) { if (listeners[type]) listeners[type].push(fn); },
            once: function (type, fn) {
                if (!listeners[type]) return;
                var wrap = function (d) { var i = listeners[type].indexOf(wrap); if (i >= 0) listeners[type].splice(i, 1); fn(d); };
                listeners[type].push(wrap);
            },
            off: function (type, fn) { if (!listeners[type]) return; var i = listeners[type].indexOf(fn); if (i >= 0) listeners[type].splice(i, 1); },
            removeListener: function (type, fn) { this.off(type, fn); }
        };
        return {
            isMainThread: false,
            parentPort: parentPort,
            workerData: __workerHost.workerData,
            threadId: __workerHost.threadId
        };
    })())"));
    wtMod.setProperty(QStringLiteral("default"), wtMod);
    m_engine->registerModule(QStringLiteral("worker_threads"), wtMod);
    m_engine->registerModule(QStringLiteral("node:worker_threads"), wtMod);

    const QJSValue mod = m_engine->importModule(
        m_script.isLocalFile() ? m_script.toLocalFile() : m_script.toString());
    if (mod.isError()) {
        emit errorOut(mod.property(QStringLiteral("message")).toString() + QStringLiteral(" (")
                      + mod.property(QStringLiteral("fileName")).toString() + QStringLiteral(":")
                      + mod.property(QStringLiteral("lineNumber")).toString() + QStringLiteral(")"));
    }
}

void WorkerScriptHost::deliver(const QVariant &data)
{
    if (!m_engine)
        return;
    QJSValue dispatch = m_engine->globalObject().property(QStringLiteral("__dispatchMessage"));
    if (!dispatch.isCallable())
        return;
    const QJSValue result = dispatch.call({ SharedBuffers::rehydrate(m_engine, data) });
    if (result.isError())
        emit errorOut(result.property(QStringLiteral("message")).toString());
}

void WorkerScriptHost::postToParent(const QJSValue &message)
{
    // Deep-converts on THIS (owning) thread — the structured-clone subset: primitives, arrays,
    // plain objects, Date, ArrayBuffer/TypedArray as byte copies; OUR SharedArrayBuffers pass
    // the ref-counted block through uncopied (top level or one property deep).
    emit messageOut(SharedBuffers::marshal(message));
}

// ─── WebWorker (main thread) ────────────────────────────────────────────────────────────────────

WebWorker::WebWorker(const QUrl &script, const QVariant &workerData, QObject *parent)
    : QObject(parent)
{
    static QAtomicInt nextThreadId(1);
    m_host = new WorkerScriptHost(script, workerData, nextThreadId.fetchAndAddRelaxed(1));
    m_host->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, m_host, &WorkerScriptHost::start);
    connect(&m_thread, &QThread::finished, m_host, &QObject::deleteLater);
    // Cross-thread by construction → automatically queued.
    connect(this, &WebWorker::deliverToWorker, m_host, &WorkerScriptHost::deliver);
    connect(m_host, &WorkerScriptHost::messageOut, this, &WebWorker::messageReceived);
    // Swallow the engine's own "Interrupted" error when WE killed it (terminate()).
    connect(m_host, &WorkerScriptHost::errorOut, this, [this](const QString &msg) {
        if (!m_terminated)
            emit errorOccurred(msg);
    });
    m_thread.setObjectName(QStringLiteral("solid-worker"));
    m_thread.start();
}

WebWorker::~WebWorker()
{
    terminate();
}

void WebWorker::postMessage(const QJSValue &message)
{
    if (m_terminated)
        return;
    emit deliverToWorker(SharedBuffers::marshal(message));
}

void WebWorker::terminate()
{
    if (m_terminated)
        return;
    m_terminated = true;
    // Interrupt kills even a hot JS loop; the engine belongs to the worker thread but
    // setInterrupted is documented thread-safe.
    if (m_host && m_host->engine())
        m_host->engine()->setInterrupted(true);
    m_thread.quit();
    m_thread.wait(3000);
}

// ─── Factory + install ──────────────────────────────────────────────────────────────────────────

WebWorkerFactory::WebWorkerFactory(const QUrl &baseUrl, QObject *parent)
    : QObject(parent)
    , m_baseUrl(baseUrl)
{
}

QObject *WebWorkerFactory::create(const QString &url)
{
    return createNode(url, QVariant());
}

QObject *WebWorkerFactory::createNode(const QString &url, const QVariant &workerData)
{
    const QUrl resolved = m_baseUrl.resolved(QUrl(url));
    // V4 may hand a JS object through a QVariant parameter as a wrapped QJSValue — which is
    // bound to THIS engine and unreadable from the worker thread. Deep-convert here, on the
    // owning thread, so the host stores plain QVariant data.
    QVariant plain = workerData;
    if (plain.userType() == qMetaTypeId<QJSValue>())
        plain = plain.value<QJSValue>().toVariant();
    auto *worker = new WebWorker(resolved, plain);
    // JS wrapper owns it (QML ownership): GC of the wrapper tears the thread down via ~WebWorker.
    QQmlEngine::setObjectOwnership(worker, QQmlEngine::JavaScriptOwnership);
    return worker;
}

void WebWorkerFactory::install(QQmlEngine *engine, const QUrl &baseUrl)
{
    auto *factory = new WebWorkerFactory(baseUrl, engine);
    engine->globalObject().setProperty(QStringLiteral("__solidWorkerFactory"), engine->newQObject(factory));
    // WHATWG-shaped wrapper: `new Worker(url)` with postMessage/terminate/onmessage/onerror and
    // message listeners. `{type:"module"}` is accepted and implied — scripts load as ES modules.
    engine->evaluate(QStringLiteral(R"((function () {
        globalThis.Worker = function Worker(url, opts) {
            var h = __solidWorkerFactory.create(String(url));
            var w = this;
            w.onmessage = null;
            w.onerror = null;
            var listeners = [];
            w.postMessage = function (m) { h.postMessage(m); };
            w.terminate = function () { h.terminate(); };
            w.addEventListener = function (type, fn) { if (type === "message") listeners.push(fn); };
            w.removeEventListener = function (type, fn) {
                var i = listeners.indexOf(fn); if (i >= 0) listeners.splice(i, 1);
            };
            h.messageReceived.connect(function (data) {
                if (typeof __solidSAB !== "undefined") data = __solidSAB.hydrate(data);
                var ev = { data: data, type: "message" };
                if (typeof w.onmessage === "function") w.onmessage(ev);
                for (var i = 0; i < listeners.length; i++) listeners[i](ev);
            });
            h.errorOccurred.connect(function (msg) {
                if (typeof w.onerror === "function") w.onerror({ message: msg, type: "error" });
                else console.error("Worker error:", msg);
            });
            w.__handle = h; // keeps the C++ handle alive with the wrapper
        };
    })())"));

    // node worker_threads (main side): Worker with the EventEmitter subset over the same backend.
    QJSValue wtMod = engine->evaluate(QStringLiteral(R"((function () {
        function NodeWorker(filename, opts) {
            var h = __solidWorkerFactory.createNode(String(filename), opts && opts.workerData);
            var self = this;
            var listeners = { message: [], error: [], exit: [] };
            this.threadId = -1;
            this.postMessage = function (m) { h.postMessage(m); };
            this.terminate = function () {
                h.terminate();
                for (var i = 0; i < listeners.exit.length; i++) listeners.exit[i](1);
                return Promise.resolve(1);
            };
            this.on = function (type, fn) { if (listeners[type]) listeners[type].push(fn); return self; };
            this.once = function (type, fn) {
                if (!listeners[type]) return self;
                var wrap = function (d) { var i = listeners[type].indexOf(wrap); if (i >= 0) listeners[type].splice(i, 1); fn(d); };
                listeners[type].push(wrap);
                return self;
            };
            this.off = function (type, fn) { if (!listeners[type]) return self; var i = listeners[type].indexOf(fn); if (i >= 0) listeners[type].splice(i, 1); return self; };
            this.removeListener = this.off;
            h.messageReceived.connect(function (data) {
                if (typeof __solidSAB !== "undefined") data = __solidSAB.hydrate(data);
                for (var i = 0; i < listeners.message.length; i++) listeners.message[i](data);
            });
            h.errorOccurred.connect(function (msg) {
                var err = new Error(msg);
                if (!listeners.error.length) { console.error("worker_threads error:", msg); return; }
                for (var i = 0; i < listeners.error.length; i++) listeners.error[i](err);
            });
            this.__handle = h;
        }
        return { isMainThread: true, parentPort: null, workerData: undefined, threadId: 0, Worker: NodeWorker };
    })())"));
    wtMod.setProperty(QStringLiteral("default"), wtMod);
    engine->registerModule(QStringLiteral("worker_threads"), wtMod);
    engine->registerModule(QStringLiteral("node:worker_threads"), wtMod);
}

} // namespace SolidWorkers
