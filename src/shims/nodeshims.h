#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QQmlEngine;

// One running child process, wrapping a QProcess. The JS `child_process` shim connects to its
// signals and resolves/rejects a Promise — the promise-like API the project wants (QProcess is
// async by nature, so this is the natural fit; no event-loop-blocking execSync).
class NodeProcessReply final : public QObject {
    Q_OBJECT

public:
    explicit NodeProcessReply(QProcess *proc, QObject *parent = nullptr);

    Q_INVOKABLE void kill();

signals:
    void finished(int code, const QString &stdoutText, const QString &stderrText);
    void failed(const QString &error, int code, const QString &stdoutText, const QString &stderrText);

private:
    QProcess *m_proc = nullptr;
};

// Qt-backed implementation of the Node host APIs the mirror exposes as importable modules
// (`process`, `fs`, `child_process`). fs is synchronous (faithful and simple over QFile/QDir);
// child_process is async/promise-like over QProcess. install() registers the JS module objects via
// QJSEngine::registerModule so `import fs from "fs"` / `import { exec } from "child_process"` resolve.
class NodeShims final : public QObject {
    Q_OBJECT

public:
    explicit NodeShims(QObject *parent = nullptr);

    static void install(QQmlEngine *engine);

    // --- process ---
    Q_INVOKABLE QString cwd() const;
    Q_INVOKABLE QVariantMap env() const;
    Q_INVOKABLE QString platform() const;
    Q_INVOKABLE QString qtVersion() const;
    Q_INVOKABLE QString solidQmlVersion() const;
    Q_INVOKABLE QStringList argv() const;

    // --- fs (synchronous) --- each returns a { ok, ... } map; the JS layer throws a Node-style error
    // when ok is false so callers see ENOENT/EEXIST etc. instead of silent failure.
    Q_INVOKABLE QVariantMap readFile(const QString &path) const;          // { ok, data (utf8) | code, message }
    Q_INVOKABLE QVariantMap writeFile(const QString &path, const QString &data, bool append) const;
    Q_INVOKABLE bool exists(const QString &path) const;
    Q_INVOKABLE QVariantMap readdir(const QString &path) const;          // { ok, entries }
    Q_INVOKABLE QVariantMap mkdir(const QString &path, bool recursive) const;
    Q_INVOKABLE QVariantMap rm(const QString &path, bool recursive) const;
    Q_INVOKABLE QVariantMap stat(const QString &path) const;            // { ok, isFile, isDirectory, size, mtimeMs }

    // --- child_process --- async; returns a reply whose signals drive the JS Promise.
    Q_INVOKABLE NodeProcessReply *spawn(const QString &command, const QStringList &args, const QVariantMap &options);
};
