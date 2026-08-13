#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QQmlEngine;

// Misc browser/host globals missing from V4, grouped: btoa/atob, TextEncoder/TextDecoder,
// Intl.Segmenter, crypto.getRandomValues/randomUUID, performance.now, and structuredClone. The C++
// backend supplies the primitives that need native support (base64, UTF-8, secure random,
// monotonic clock); install() layers the WHATWG JS classes/objects on top as real globals. Pure
// ECMAScript prototype polyfills are handled by the npm mirror compat pass.
class WebPlatform final : public QObject {
    Q_OBJECT

public:
    explicit WebPlatform(QObject *parent = nullptr);

    static void install(QQmlEngine *engine);

    Q_INVOKABLE QString btoa(const QString &binary) const;   // bytes (latin1) → base64
    Q_INVOKABLE QString atob(const QString &base64) const;   // base64 → bytes (latin1)
    Q_INVOKABLE QByteArray utf8Encode(const QString &text) const { return text.toUtf8(); }
    Q_INVOKABLE QString utf8Decode(const QByteArray &bytes) const { return QString::fromUtf8(bytes); }
    Q_INVOKABLE QByteArray randomBytes(int n) const;         // cryptographically secure
    Q_INVOKABLE QString randomUUID() const;                  // v4 UUID
    Q_INVOKABLE double now() const { return m_timer.nsecsElapsed() / 1.0e6; }
    Q_INVOKABLE double timeOrigin() const { return double(m_originMs); }
    // name -> capture-group index for a regex pattern, via Qt's PCRE2 (QRegularExpression). V4's JS
    // RegExp captures named groups positionally but never populates `.exec().groups`; __SqRegExp uses
    // this map to synthesise it. Empty if the pattern has no named groups or PCRE2 can't parse it.
    Q_INVOKABLE QVariantMap namedCaptureGroups(const QString &pattern) const;

    // Clipboard — the system clipboard via QClipboard (there was no app-level clipboard path at
    // all: cut/copy/paste worked only INSIDE the native text controls). Exposed to JS as the
    // WHATWG `navigator.clipboard` (async), so app code reads like it does on the web.
    Q_INVOKABLE QString clipboardText() const;
    Q_INVOKABLE void setClipboardText(const QString &text) const;

private:
    QElapsedTimer m_timer;
    qint64 m_originMs = 0;
};
