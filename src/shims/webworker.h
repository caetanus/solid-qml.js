#pragma once

#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QUrl>
#include <QVariant>

class QQmlEngine;

// Web Worker for the V4 runtime (owner 2026-07-08: "precisamos de webworker … e threads").
//
// Each Worker is a REAL OS thread (QThread with its own event loop) running its own QQmlEngine
// with the full shim stack installed — fetch, timers, localStorage, node modules, polyfills —
// i.e. the --mini-node runtime on a thread; pure Qt, so the same code serves desktop AND the
// mobile ports. The script loads through importModule (ES modules; node-module imports work).
//
// Message passing follows the platform rule: a QJSValue is bound to its engine AND thread, so a
// message converts to a QVariant on the OWNING thread (deep: arrays/objects/ArrayBuffer copies —
// the structured-clone subset), crosses via queued connection, and rehydrates on the receiving
// engine (`onmessage({ data })`). SharedArrayBuffer pass-through (same backing store, no copy)
// is the follow-up task.
//
// terminate() interrupts the JS engine (kills even a hot loop) and quits the thread.
namespace SolidWorkers {

// Runs INSIDE the worker thread: owns the worker engine, receives inbound messages, exposes the
// worker-global postMessage bridge.
class WorkerScriptHost : public QObject {
    Q_OBJECT

public:
    WorkerScriptHost(const QUrl &script, QObject *parent = nullptr);
    ~WorkerScriptHost() override;

    QQmlEngine *engine() const { return m_engine; }

public slots:
    void start();                          // create engine + shims, import the module
    void deliver(const QVariant &data);    // main → worker (onmessage)

    // Called by the worker script through the JS bridge (already on the worker thread).
    void postToParent(const QJSValue &message);

signals:
    void messageOut(const QVariant &data); // worker → main
    void errorOut(const QString &message);

private:
    QUrl m_script;
    QQmlEngine *m_engine = nullptr;
};

// The main-thread handle the `Worker` JS wrapper drives.
class WebWorker : public QObject {
    Q_OBJECT

public:
    explicit WebWorker(const QUrl &script, QObject *parent = nullptr);
    ~WebWorker() override;

    Q_INVOKABLE void postMessage(const QJSValue &message);
    Q_INVOKABLE void terminate();

signals:
    void messageReceived(const QVariant &data);
    void errorOccurred(const QString &message);
    void deliverToWorker(const QVariant &data); // internal: queued into the worker thread

private:
    QThread m_thread;
    WorkerScriptHost *m_host = nullptr; // lives on m_thread
    bool m_terminated = false;
};

// Factory + installer: defines the global `Worker` constructor on an engine.
class WebWorkerFactory : public QObject {
    Q_OBJECT

public:
    explicit WebWorkerFactory(const QUrl &baseUrl, QObject *parent = nullptr);

    // `baseUrl` anchors relative script URLs (the app dir).
    static void install(QQmlEngine *engine, const QUrl &baseUrl);

    Q_INVOKABLE QObject *create(const QString &url);

private:
    QUrl m_baseUrl;
};

} // namespace SolidWorkers
