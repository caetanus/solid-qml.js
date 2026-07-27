#include "weblocalstorage.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QSaveFile>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QStandardPaths>

WebLocalStorage::WebLocalStorage(const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_filePath(filePath.isEmpty() ? defaultFilePath() : filePath)
{
    load();
}

QString WebLocalStorage::defaultFilePath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.local/share/solid-qml");
    return dir + QStringLiteral("/localstorage.json");
}

void WebLocalStorage::load()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return; // first run / no store yet
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
        m_data.insert(it.key(), it.value().toString());
}

void WebLocalStorage::save() const
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    // QSaveFile writes to a temp sibling and atomically renames on commit(), so a crash mid-write
    // never leaves a truncated/corrupt store (the previous QFile+Truncate path could).
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning().noquote() << "localStorage: cannot write" << m_filePath;
        return;
    }
    QJsonObject obj;
    for (auto it = m_data.constBegin(); it != m_data.constEnd(); ++it)
        obj.insert(it.key(), it.value());
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    if (!file.commit())
        qWarning().noquote() << "localStorage: commit failed for" << m_filePath;
}

QVariant WebLocalStorage::getItem(const QString &key) const
{
    const auto it = m_data.constFind(key);
    return it == m_data.constEnd() ? QVariant() : QVariant(it.value());
}

bool WebLocalStorage::setItem(const QString &key, const QString &value)
{
    if (m_data.value(key) == value && m_data.contains(key))
        return true;
    // Enforce the per-origin quota on the resulting store size (UTF-8 bytes of keys + values), so a
    // runaway writer fails loudly instead of ballooning the JSON file.
    qint64 total = 0;
    for (auto it = m_data.constBegin(); it != m_data.constEnd(); ++it)
        if (it.key() != key)
            total += it.key().toUtf8().size() + it.value().toUtf8().size();
    total += key.toUtf8().size() + value.toUtf8().size();
    if (total > kQuotaBytes) {
        qWarning().noquote() << "localStorage: quota exceeded (" << total << ">" << kQuotaBytes << "bytes)";
        return false;
    }
    m_data.insert(key, value);
    save();
    emit changed();
    return true;
}

void WebLocalStorage::removeItem(const QString &key)
{
    if (m_data.remove(key) > 0) {
        save();
        emit changed();
    }
}

void WebLocalStorage::clear()
{
    if (m_data.isEmpty())
        return;
    m_data.clear();
    save();
    emit changed();
}

QVariant WebLocalStorage::key(int index) const
{
    if (index < 0 || index >= m_data.size())
        return QVariant();
    return std::next(m_data.constBegin(), index).key();
}

// The WHATWG adapter: wraps the C++ backend in a spec-faithful `localStorage` — getItem
// returns null on miss, values coerce to string, and (where V4 has Proxy) `localStorage.foo`
// / `localStorage['foo']` read/write/delete map onto the store too. Falls back to the plain
// method object when Proxy is unavailable.
static const char *kAdapterSource = R"JS(
(function (b) {
    var api = {
        getItem: function (k) { var v = b.getItem(String(k)); return (v === undefined || v === null) ? null : String(v); },
        setItem: function (k, v) {
            if (!b.setItem(String(k), v === undefined ? "undefined" : String(v))) {
                var e = new Error("Failed to execute 'setItem' on 'Storage': the quota has been exceeded.");
                e.name = "QuotaExceededError";
                throw e;
            }
        },
        removeItem: function (k) { b.removeItem(String(k)); },
        clear: function () { b.clear(); },
        key: function (i) { var k = b.key(i | 0); return (k === undefined || k === null) ? null : String(k); }
    };
    Object.defineProperty(api, "length", { get: function () { return b.length; }, enumerable: false });
    if (typeof Proxy === "undefined")
        return api;
    return new Proxy(api, {
        get: function (t, p) {
            if (typeof p === "symbol" || p in t) return t[p];
            return t.getItem(p);
        },
        set: function (t, p, v) {
            if (p in t) t[p] = v; else t.setItem(p, v);
            return true;
        },
        has: function (t, p) { return (p in t) || b.contains(String(p)); },
        deleteProperty: function (t, p) { t.removeItem(p); return true; },
        ownKeys: function () {
            var out = [];
            for (var i = 0; i < b.length; ++i) out.push(String(b.key(i)));
            return out;
        },
        getOwnPropertyDescriptor: function (t, p) {
            if (b.contains(String(p)))
                return { configurable: true, enumerable: true, writable: true, value: t.getItem(p) };
            return undefined;
        }
    });
})(__localStorageBackend)
)JS";

void WebLocalStorage::install(QQmlEngine *engine, const QString &filePath)
{
    if (!engine)
        return;
    auto *backend = new WebLocalStorage(filePath, engine);
    QQmlEngine::setObjectOwnership(backend, QQmlEngine::CppOwnership);

    QJSValue global = engine->globalObject();
    global.setProperty(QStringLiteral("__localStorageBackend"), engine->newQObject(backend));

    const QJSValue storage = engine->evaluate(QString::fromUtf8(kAdapterSource));
    if (storage.isError()) {
        qWarning().noquote() << "localStorage: adapter failed:" << storage.toString();
        return;
    }
    global.setProperty(QStringLiteral("localStorage"), storage);
}
