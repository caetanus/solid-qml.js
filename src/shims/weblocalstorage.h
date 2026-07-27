#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>

class QQmlEngine;

// Persistent backend for a WHATWG `localStorage` shim under QML's V4 engine (which has no DOM
// Storage). Synchronous string key/value store, persisted to a JSON file. install() registers
// it as a real JS global `localStorage` wrapped by a small spec-compliant adapter (null on
// miss, string coercion, indexed/named access) — so app code uses the standard API verbatim.
class WebLocalStorage final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int length READ length NOTIFY changed)

public:
    explicit WebLocalStorage(const QString &filePath = {}, QObject *parent = nullptr);

    // Install `localStorage` as a JS global on the engine. Call once, before loading QML.
    static void install(QQmlEngine *engine, const QString &filePath = {});

    // Per-origin storage budget (WHATWG suggests ~5 MiB); setItem past it fails like a real
    // QuotaExceededError instead of growing without bound.
    static constexpr int kQuotaBytes = 5 * 1024 * 1024;

    // Storage API primitives (the JS adapter layers null/coercion/indexing on top).
    Q_INVOKABLE QVariant getItem(const QString &key) const;
    // Returns false when the write would exceed the quota (the adapter throws QuotaExceededError).
    Q_INVOKABLE bool setItem(const QString &key, const QString &value);
    Q_INVOKABLE void removeItem(const QString &key);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QVariant key(int index) const; // key name at index, or invalid (→ null)
    Q_INVOKABLE bool contains(const QString &key) const { return m_data.contains(key); }

    int length() const { return m_data.size(); }

signals:
    void changed();

private:
    static QString defaultFilePath();
    void load();
    void save() const;

    QString m_filePath;
    QMap<QString, QString> m_data; // sorted keys → deterministic key(index)
};
