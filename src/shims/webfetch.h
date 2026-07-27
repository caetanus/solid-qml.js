#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QNetworkAccessManager;
class QNetworkReply;
class QQmlEngine;

// One in-flight request, wrapping a QNetworkReply. The JS `fetch` shim connects to its
// signals and resolves/rejects the Promise. Like real fetch, an HTTP response (any status)
// "finishes"; only transport failure / timeout / abort "fails".
class WebFetchReply final : public QObject {
    Q_OBJECT

public:
    WebFetchReply(QNetworkReply *reply, QObject *parent = nullptr);

    Q_INVOKABLE void abort();

signals:
    // body is raw bytes (QByteArray → an ArrayBuffer in V4), NOT a decoded string — so binary
    // payloads survive intact. The JS Response layer decodes UTF-8 lazily for text()/json().
    void finished(int status, const QString &statusText, const QVariantMap &headers, const QByteArray &body);
    void failed(const QString &error, bool aborted);
    void downloadProgress(qint64 received, qint64 total);

private:
    QNetworkReply *m_reply = nullptr;
    bool m_aborted = false;
};

// fetch() transport backed by QNetworkAccessManager, exposed to the V4 engine as the global
// `Http`. install() also evaluates the WHATWG JS layer that defines the real globals —
// `fetch`, `Headers`, `Request`, `Response`, `AbortController`, `AbortSignal` — on top of it.
class WebFetch final : public QObject {
    Q_OBJECT

public:
    explicit WebFetch(QObject *parent = nullptr);

    static void install(QQmlEngine *engine);

    // request(url, { method, headers (object), body, timeout }) -> WebFetchReply.
    Q_INVOKABLE WebFetchReply *request(const QString &url, const QVariantMap &options);

private:
    QNetworkAccessManager *m_nam;
};
