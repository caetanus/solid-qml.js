#include "webplatform.h"

#include <QDateTime>
#include <QDebug>
#include <QJSEngine>
#include <QQmlEngine>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>

#include <cstring>

WebPlatform::WebPlatform(QObject *parent)
    : QObject(parent)
    , m_originMs(QDateTime::currentMSecsSinceEpoch())
{
    m_timer.start();
}

QString WebPlatform::btoa(const QString &binary) const
{
    QByteArray bytes;
    bytes.reserve(binary.size());
    for (const QChar c : binary)
        bytes.append(char(c.unicode() & 0xFF)); // WHATWG btoa: each code unit is one byte
    return QString::fromLatin1(bytes.toBase64());
}

QString WebPlatform::atob(const QString &base64) const
{
    const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
    QString out;
    out.reserve(bytes.size());
    for (const char c : bytes)
        out.append(QChar(uchar(c)));
    return out;
}

QByteArray WebPlatform::randomBytes(int n) const
{
    if (n <= 0)
        return {};
    QByteArray out(n, Qt::Uninitialized);
    auto *rng = QRandomGenerator::system();
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const quint32 v = rng->generate();
        std::memcpy(out.data() + i, &v, 4);
    }
    if (i < n) {
        const quint32 v = rng->generate();
        std::memcpy(out.data() + i, &v, n - i);
    }
    return out;
}

QString WebPlatform::randomUUID() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Bridge the one ECMAScript regex escape PCRE2 rejects: `\uXXXX` / `\u{H..}` -> `\x{H..}`. Only `\u`
// differs for capture-group parsing; every other escape is copied verbatim (so `\\`, `\x{}`, `\d`,
// `\k<>` pass through untouched and an escaped backslash is never mis-read as the start of `\u`).
static QString ecmaToPcrePattern(const QString &p)
{
    const auto isHex = [](QChar c) { return c.isDigit() || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F'); };
    QString out;
    out.reserve(p.size());
    for (int i = 0; i < p.size();) {
        if (p.at(i) == u'\\' && i + 1 < p.size()) {
            const QChar n = p.at(i + 1);
            if (n == u'u' && i + 2 < p.size() && p.at(i + 2) == u'{') {
                int j = i + 3;
                QString hex;
                while (j < p.size() && p.at(j) != u'}') { hex += p.at(j); ++j; }
                if (j < p.size() && !hex.isEmpty()) { out += u"\\x{" + hex + u"}"; i = j + 1; continue; }
            } else if (n == u'u' && i + 5 < p.size()) {
                const QString hex = p.mid(i + 2, 4);
                if (isHex(hex.at(0)) && isHex(hex.at(1)) && isHex(hex.at(2)) && isHex(hex.at(3))) {
                    out += u"\\x{" + hex + u"}"; i += 6; continue;
                }
            }
            out += p.at(i); out += n; i += 2; // copy the escape pair verbatim
            continue;
        }
        out += p.at(i);
        ++i;
    }
    return out;
}

QVariantMap WebPlatform::namedCaptureGroups(const QString &pattern) const
{
    QVariantMap out;
    const QRegularExpression re(ecmaToPcrePattern(pattern));
    if (!re.isValid())
        return out; // PCRE2 couldn't parse it; caller leaves groups unset (native ECMAScript behaviour)
    // namedCaptureGroups() is indexed by capture-group number; entry i is that group's name ("" if
    // unnamed). Group numbering by opening-paren order matches ECMAScript, so the index lines up with
    // V4's positional captures (m[i]).
    const QStringList names = re.namedCaptureGroups();
    for (int i = 0; i < names.size(); ++i)
        if (!names.at(i).isEmpty())
            out.insert(names.at(i), i);
    return out;
}

// WHATWG layer on the C++ backend. Typed arrays / ArrayBuffer are native; QByteArray returned
// from C++ surfaces as an ArrayBuffer in V4 (and an ArrayBuffer argument converts back).
static const char *kPlatformShim = R"JS(
(function (B) {
    var IntlObject = typeof Intl === "undefined" ? {} : Intl;
    if (!IntlObject.Segmenter) {
        IntlObject.Segmenter = function () {};
        IntlObject.Segmenter.prototype.segment = function (input) {
            return String(input).split("").map(function (segment) { return { segment: segment }; });
        };
    }

    function viewBuffer(input) {
        if (input instanceof ArrayBuffer) return input;
        if (ArrayBuffer.isView(input)) return input.buffer.slice(input.byteOffset, input.byteOffset + input.byteLength);
        return input;
    }

    class TextEncoder {
        get encoding() { return "utf-8"; }
        encode(s) { return new Uint8Array(B.utf8Encode(s === undefined ? "" : String(s))); }
        encodeInto(s, dest) {
            var enc = this.encode(s), n = Math.min(enc.length, dest.length);
            dest.set(enc.subarray(0, n));
            return { read: String(s).length, written: n };
        }
    }
    class TextDecoder {
        constructor(label) { this._label = (label || "utf-8").toLowerCase(); }
        get encoding() { return "utf-8"; }
        decode(input) { return (input === undefined || input === null) ? "" : B.utf8Decode(viewBuffer(input)); }
    }

    var crypto = {
        getRandomValues: function (ta) {
            if (!ta || !ArrayBuffer.isView(ta)) throw new TypeError("getRandomValues expects a typed array");
            var bytes = new Uint8Array(B.randomBytes(ta.byteLength));
            new Uint8Array(ta.buffer, ta.byteOffset, ta.byteLength).set(bytes);
            return ta;
        },
        randomUUID: function () { return B.randomUUID(); }
    };

    var performance = { now: function () { return B.now(); }, timeOrigin: B.timeOrigin() };

    function structuredClone(value) {
        var seen = new Map();
        function clone(v) {
            if (v === null || typeof v !== "object") return v;
            if (seen.has(v)) return seen.get(v);
            if (v instanceof Date) return new Date(v.getTime());
            if (v instanceof RegExp) return new RegExp(v.source, v.flags);
            if (v instanceof ArrayBuffer) return v.slice(0);
            if (ArrayBuffer.isView(v)) {
                if (v instanceof DataView) return new DataView(v.buffer.slice(0), v.byteOffset, v.byteLength);
                return new v.constructor(v);
            }
            var out;
            if (Array.isArray(v)) { out = []; seen.set(v, out); for (var i = 0; i < v.length; ++i) out[i] = clone(v[i]); return out; }
            if (v instanceof Map) { out = new Map(); seen.set(v, out); v.forEach(function (val, key) { out.set(clone(key), clone(val)); }); return out; }
            if (v instanceof Set) { out = new Set(); seen.set(v, out); v.forEach(function (val) { out.add(clone(val)); }); return out; }
            out = {}; seen.set(v, out);
            for (var k in v) if (Object.prototype.hasOwnProperty.call(v, k)) out[k] = clone(v[k]);
            return out;
        }
        return clone(value);
    }

    // Factory the npm mirror rewrites `new RegExp(...)` to. V4 captures named groups positionally but
    // never populates `match.groups`; when a pattern actually has named groups we wrap the instance's
    // exec to synthesise `.groups` from the PCRE2-derived name->index map. Zero overhead otherwise
    // (the cheap `(?<name>` check avoids even calling into C++), so it's a safe blanket rewrite.
    var NAMED_GROUP = /\(\?<[A-Za-z_$]/;
    function __SqRegExp(pattern, flags) {
        var re = flags === undefined ? new RegExp(pattern) : new RegExp(pattern, flags);
        var source = typeof pattern === "string" ? pattern : (pattern && pattern.source) || re.source;
        if (typeof source !== "string" || !NAMED_GROUP.test(source)) return re;
        var map = B.namedCaptureGroups(source);
        var names = Object.keys(map);
        if (names.length === 0) return re;
        var nativeExec = RegExp.prototype.exec;
        // Plain `re.exec = fn` is silently refused: the inherited RegExp.prototype.exec is non-writable
        // and the sloppy-mode [[Set]] honours that. defineProperty installs an own property directly.
        Object.defineProperty(re, "exec", {
            value: function (str) {
                var m = nativeExec.call(this, str);
                if (m && m.groups === undefined) {
                    var groups = {};
                    for (var i = 0; i < names.length; ++i) groups[names[i]] = m[map[names[i]]];
                    m.groups = groups;
                }
                return m;
            },
            writable: true, configurable: true
        });
        return re;
    }

    return {
        btoa: function (s) { return B.btoa(String(s)); },
        atob: function (s) { return B.atob(String(s)); },
        TextEncoder: TextEncoder,
        TextDecoder: TextDecoder,
        crypto: crypto,
        Intl: IntlObject,
        performance: performance,
        structuredClone: structuredClone,
        __SqRegExp: __SqRegExp
    };
})(__platformBackend)
)JS";

void WebPlatform::install(QQmlEngine *engine)
{
    if (!engine)
        return;
    auto *backend = new WebPlatform(engine);
    QQmlEngine::setObjectOwnership(backend, QQmlEngine::CppOwnership);

    QJSValue global = engine->globalObject();
    global.setProperty(QStringLiteral("__platformBackend"), engine->newQObject(backend));

    const QJSValue api = engine->evaluate(QString::fromUtf8(kPlatformShim));
    if (api.isError()) {
        qWarning().noquote() << "platform shim failed:" << api.toString();
        return;
    }
    for (const QString &name : { QStringLiteral("btoa"), QStringLiteral("atob"),
                                 QStringLiteral("TextEncoder"), QStringLiteral("TextDecoder"),
                                 QStringLiteral("crypto"), QStringLiteral("Intl"),
                                 QStringLiteral("performance"),
                                 QStringLiteral("structuredClone"),
                                 QStringLiteral("__SqRegExp") })
        global.setProperty(name, api.property(name));
}
