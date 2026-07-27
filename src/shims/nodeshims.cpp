#include "nodeshims.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QProcessEnvironment>
#include <QQmlEngine>

// ---------------------------------------------------------------------------- NodeProcessReply

NodeProcessReply::NodeProcessReply(QProcess *proc, QObject *parent)
    : QObject(parent)
    , m_proc(proc)
{
    m_proc->setParent(this);
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QString out = QString::fromUtf8(m_proc->readAllStandardOutput());
        const QString err = QString::fromUtf8(m_proc->readAllStandardError());
        if (status == QProcess::CrashExit)
            emit failed(QStringLiteral("Process crashed"), code, out, err);
        else
            emit finished(code, out, err);
        deleteLater();
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        // finished() also fires for most errors; guard so we only report a start failure once.
        if (m_proc->state() == QProcess::NotRunning && m_proc->exitStatus() != QProcess::CrashExit
            && m_proc->processId() == 0) {
            emit failed(m_proc->errorString(), -1,
                        QString::fromUtf8(m_proc->readAllStandardOutput()),
                        QString::fromUtf8(m_proc->readAllStandardError()));
            deleteLater();
        }
    });
}

void NodeProcessReply::kill()
{
    if (m_proc)
        m_proc->kill();
}

// ---------------------------------------------------------------------------- NodeShims backend

NodeShims::NodeShims(QObject *parent) : QObject(parent) {}

QString NodeShims::cwd() const { return QDir::currentPath(); }

QVariantMap NodeShims::env() const
{
    QVariantMap out;
    const QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    for (const QString &key : e.keys())
        out.insert(key, e.value(key));
    return out;
}

QString NodeShims::platform() const
{
#if defined(Q_OS_WIN)
    return QStringLiteral("win32");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("darwin");
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("android");
#elif defined(Q_OS_IOS)
    return QStringLiteral("ios");
#else
    return QStringLiteral("linux");
#endif
}

QString NodeShims::qtVersion() const
{
    return QString::fromLatin1(qVersion());
}

QString NodeShims::solidQmlVersion() const
{
#ifdef SOLID_QML_VERSION
    return QStringLiteral(SOLID_QML_VERSION);
#else
    return QStringLiteral("dev");
#endif
}

void NodeShims::exitApp(int code)
{
    QCoreApplication::exit(code);
}

QStringList NodeShims::argv() const { return QCoreApplication::arguments(); }

static QVariantMap fsError(const QString &code, const QString &message)
{
    return { { QStringLiteral("ok"), false }, { QStringLiteral("code"), code }, { QStringLiteral("message"), message } };
}

QVariantMap NodeShims::readFile(const QString &path) const
{
    QFile f(path);
    if (!f.exists())
        return fsError(QStringLiteral("ENOENT"), QStringLiteral("no such file or directory, open '%1'").arg(path));
    if (!f.open(QIODevice::ReadOnly))
        return fsError(QStringLiteral("EACCES"), f.errorString());
    return { { QStringLiteral("ok"), true }, { QStringLiteral("data"), QString::fromUtf8(f.readAll()) } };
}

QVariantMap NodeShims::writeFile(const QString &path, const QString &data, bool append) const
{
    QFile f(path);
    const auto mode = QIODevice::WriteOnly | (append ? QIODevice::Append : QIODevice::Truncate);
    if (!f.open(mode))
        return fsError(QStringLiteral("EACCES"), f.errorString());
    f.write(data.toUtf8());
    return { { QStringLiteral("ok"), true } };
}

bool NodeShims::exists(const QString &path) const { return QFileInfo::exists(path); }

QVariantMap NodeShims::readdir(const QString &path) const
{
    QDir dir(path);
    if (!dir.exists())
        return fsError(QStringLiteral("ENOENT"), QStringLiteral("no such file or directory, scandir '%1'").arg(path));
    return { { QStringLiteral("ok"), true },
             { QStringLiteral("entries"), dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name) } };
}

QVariantMap NodeShims::mkdir(const QString &path, bool recursive) const
{
    QDir dir;
    const bool ok = recursive ? dir.mkpath(path) : dir.mkdir(path);
    if (!ok && !(recursive && QFileInfo(path).isDir()))
        return fsError(QStringLiteral("EEXIST"), QStringLiteral("mkdir '%1'").arg(path));
    return { { QStringLiteral("ok"), true } };
}

QVariantMap NodeShims::rm(const QString &path, bool recursive) const
{
    QFileInfo info(path);
    if (!info.exists())
        return fsError(QStringLiteral("ENOENT"), QStringLiteral("no such file or directory, rm '%1'").arg(path));
    const bool ok = info.isDir() && recursive ? QDir(path).removeRecursively()
                  : info.isDir()              ? QDir().rmdir(path)
                                              : QFile::remove(path);
    if (!ok)
        return fsError(QStringLiteral("EPERM"), QStringLiteral("operation not permitted, rm '%1'").arg(path));
    return { { QStringLiteral("ok"), true } };
}

QVariantMap NodeShims::stat(const QString &path) const
{
    QFileInfo info(path);
    if (!info.exists())
        return fsError(QStringLiteral("ENOENT"), QStringLiteral("no such file or directory, stat '%1'").arg(path));
    return { { QStringLiteral("ok"), true },
             { QStringLiteral("isFile"), info.isFile() },
             { QStringLiteral("isDirectory"), info.isDir() },
             { QStringLiteral("size"), info.size() },
             { QStringLiteral("mtimeMs"), double(info.lastModified().toMSecsSinceEpoch()) } };
}

NodeProcessReply *NodeShims::spawn(const QString &command, const QStringList &args, const QVariantMap &options)
{
    auto *proc = new QProcess();
    if (options.contains(QStringLiteral("cwd")))
        proc->setWorkingDirectory(options.value(QStringLiteral("cwd")).toString());
    if (options.contains(QStringLiteral("env"))) {
        QProcessEnvironment pe;
        const QVariantMap envMap = options.value(QStringLiteral("env")).toMap();
        for (auto it = envMap.constBegin(); it != envMap.constEnd(); ++it)
            pe.insert(it.key(), it.value().toString());
        proc->setProcessEnvironment(pe);
    }
    auto *reply = new NodeProcessReply(proc);
    proc->start(command, args);
    return reply;
}

// ---------------------------------------------------------------------------- JS modules

// process / fs / child_process as importable modules, built on the C++ backend exposed as
// `__nodeBackend`. fs is synchronous (throws Node-style errors); child_process.exec/execFile return
// Promises driven by the QProcess reply's signals. `C` is the capability profile (__nodeCaps):
// { fs, exec, procFull } — fs and child_process are only built when granted, and the ambient
// `process` keeps its native probe (versions) always but neuters env/cwd/argv/exit without procFull.
static const char *kNodeShim = R"JS(
(function (B, C) {
    function fsThrow(r, syscall, path) {
        var e = new Error(r.code + ": " + r.message);
        e.code = r.code; e.syscall = syscall; e.path = path; e.errno = -2;
        throw e;
    }
    function decode(data, encoding) {
        // No encoding (or "buffer") -> Node returns a Buffer; we have none, so hand back a Uint8Array.
        if (encoding && encoding !== "buffer" && (encoding.encoding || encoding) !== "buffer") return data;
        var u = new Uint8Array(data.length);
        for (var i = 0; i < data.length; ++i) u[i] = data.charCodeAt(i) & 0xff;
        return u;
    }

    var fs = !C.fs ? undefined : {
        readFileSync: function (path, options) {
            var r = B.readFile(String(path));
            if (!r.ok) fsThrow(r, "open", path);
            var enc = options && (typeof options === "string" ? options : options.encoding);
            return enc ? r.data : decode(r.data);
        },
        writeFileSync: function (path, data, options) { var r = B.writeFile(String(path), String(data), false); if (!r.ok) fsThrow(r, "write", path); },
        appendFileSync: function (path, data) { var r = B.writeFile(String(path), String(data), true); if (!r.ok) fsThrow(r, "write", path); },
        existsSync: function (path) { return B.exists(String(path)); },
        readdirSync: function (path) { var r = B.readdir(String(path)); if (!r.ok) fsThrow(r, "scandir", path); return r.entries; },
        mkdirSync: function (path, options) { var r = B.mkdir(String(path), !!(options && options.recursive)); if (!r.ok) fsThrow(r, "mkdir", path); },
        rmSync: function (path, options) { var r = B.rm(String(path), !!(options && options.recursive)); if (!r.ok && !(options && options.force)) fsThrow(r, "rm", path); },
        unlinkSync: function (path) { var r = B.rm(String(path), false); if (!r.ok) fsThrow(r, "unlink", path); },
        statSync: function (path) {
            var r = B.stat(String(path));
            if (!r.ok) fsThrow(r, "stat", path);
            return { size: r.size, mtimeMs: r.mtimeMs, isFile: function () { return r.isFile; }, isDirectory: function () { return r.isDirectory; } };
        }
    };
    if (fs) fs.promises = {
        readFile: function (p, o) { return new Promise(function (res, rej) { try { res(fs.readFileSync(p, o)); } catch (e) { rej(e); } }); },
        writeFile: function (p, d, o) { return new Promise(function (res, rej) { try { fs.writeFileSync(p, d, o); res(); } catch (e) { rej(e); } }); }
    };

    function run(file, args, options) {
        return new Promise(function (resolve, reject) {
            var reply = B.spawn(file, args || [], options || {});
            reply.finished.connect(function (code, out, err) {
                if (code === 0) { resolve({ stdout: out, stderr: err }); return; }
                var e = new Error("Command failed with exit code " + code);
                e.code = code; e.stdout = out; e.stderr = err; reject(e);
            });
            reply.failed.connect(function (error, code, out, err) {
                var e = new Error(error); e.code = code; e.stdout = out; e.stderr = err; reject(e);
            });
        });
    }
    var isWin = B.platform() === "win32";
    var child_process = !C.exec ? undefined : {
        // Promise-like (project preference): exec/execFile resolve { stdout, stderr } or reject an
        // Error carrying code/stdout/stderr — async over QProcess, never blocking the event loop.
        exec: function (command, options) {
            return run(isWin ? "cmd" : "/bin/sh", isWin ? ["/c", String(command)] : ["-c", String(command)], options);
        },
        execFile: function (file, args, options) {
            if (args && !Array.isArray(args)) { options = args; args = []; }
            return run(String(file), (args || []).map(String), options);
        },
        spawn: function (file, args, options) { return run(String(file), (args || []).map(String), options); }
    };

    function denied(what) { return function () { throw new Error(what + " is not permitted under the current capability profile"); }; }
    var process = {
        platform: B.platform(),
        // procFull gates the ambient-process powers: full argv/env, the real cwd, and app exit.
        // Without it (browser profile) the environment and argv stay empty and exit/cwd are blocked —
        // but `versions` (the native probe) is always present so feature detection still works.
        argv: C.procFull ? B.argv() : [],
        env: C.procFull ? B.env() : {},
        cwd: C.procFull ? function () { return B.cwd(); } : denied("process.cwd"),
        nextTick: function (cb) { var a = Array.prototype.slice.call(arguments, 1); Promise.resolve().then(function () { cb.apply(null, a); }); },
        exit: C.procFull ? function (code) { B.exitApp(code === undefined ? 0 : Number(code)); } : denied("process.exit"),
        version: "v18.0.0-solidqml",
        // Electron idiom: `process.versions.solidQml` IS the native-runtime probe —
        // `typeof process !== "undefined" && !!process.versions?.solidQml`.
        versions: { node: "18.0.0", solidQml: B.solidQmlVersion(), qml: B.solidQmlVersion(), qt: B.qtVersion() }
    };

    return { fs: fs, child_process: child_process, process: process };
})(__nodeBackend, __nodeCaps)
)JS";

NodeShims::Profile NodeShims::profileFromString(const QString &s, Profile fallback)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("browser")) return Profile::Browser;
    if (v == QLatin1String("desktop")) return Profile::Desktop;
    if (v == QLatin1String("trusted") || v == QLatin1String("trusted-node")) return Profile::Trusted;
    if (!v.isEmpty())
        qWarning().noquote() << "node shims: unknown capability profile" << s << "— using default (desktop)";
    return fallback;
}

void NodeShims::install(QQmlEngine *engine, Profile profile)
{
    if (!engine)
        return;

    // Capability profile (owner decision 2026-07-26): "import any npm" must not silently grant every
    // app full machine access. Three profiles gate the privileged host surface:
    //   browser  — no filesystem, no child_process; `process` keeps only its native probe (versions)
    //              and platform, with env/argv empty and cwd/exit blocked. For sandboxed embeds.
    //   desktop  — filesystem + full process, but NO arbitrary command execution (default).
    //   trusted  — full access incl. child_process / /bin/sh; logs a stark warning.
    const bool allowFs = profile != Profile::Browser;
    const bool allowExec = profile == Profile::Trusted;
    const bool procFull = profile != Profile::Browser;
    if (profile == Profile::Trusted)
        qWarning().noquote() << "node shims: TRUSTED capability profile — filesystem AND arbitrary "
                                "command execution (/bin/sh) are exposed to ALL imported code.";

    auto *backend = new NodeShims(engine);
    QQmlEngine::setObjectOwnership(backend, QQmlEngine::CppOwnership);

    QJSValue global = engine->globalObject();
    global.setProperty(QStringLiteral("__nodeBackend"), engine->newQObject(backend));

    QJSValue caps = engine->newObject();
    caps.setProperty(QStringLiteral("fs"), allowFs);
    caps.setProperty(QStringLiteral("exec"), allowExec);
    caps.setProperty(QStringLiteral("procFull"), procFull);
    global.setProperty(QStringLiteral("__nodeCaps"), caps);

    const QJSValue api = engine->evaluate(QString::fromUtf8(kNodeShim));
    if (api.isError()) {
        qWarning().noquote() << "node shims: failed:" << api.toString();
        return;
    }
    // Register each module under both its bare and node:-prefixed specifier. A `default` self-reference
    // makes `import fs from "fs"` and `import { readFileSync } from "fs"` both resolve. A module the
    // profile withholds (fs/child_process) is left unregistered → `import` fails cleanly, so the
    // capability is genuinely absent rather than present-but-throwing.
    const std::pair<const char *, const char *> mods[] = {
        { "fs", "fs" }, { "child_process", "child_process" }, { "process", "process" },
    };
    for (const auto &[name, prop] : mods) {
        QJSValue mod = api.property(QString::fromUtf8(prop));
        if (mod.isUndefined() || mod.isNull())
            continue;
        mod.setProperty(QStringLiteral("default"), mod);
        engine->registerModule(QString::fromUtf8(name), mod);
        engine->registerModule(QStringLiteral("node:") + QString::fromUtf8(name), mod);
    }

    // Electron parity: `process` is also an ambient GLOBAL (owner directive) — QML
    // bindings, handlers and mirrored npm modules all see it without an import; on the
    // web target it stays undefined, which IS the platform signal.
    global.setProperty(QStringLiteral("process"), api.property(QStringLiteral("process")));
}
