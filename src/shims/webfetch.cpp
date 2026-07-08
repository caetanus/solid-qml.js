#include "webfetch.h"

#include <QDebug>
#include <QJSEngine>
#include <QJSValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QUrl>

WebFetchReply::WebFetchReply(QNetworkReply *reply, QObject *parent)
    : QObject(parent)
    , m_reply(reply)
{
    m_reply->setParent(this);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &WebFetchReply::downloadProgress);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (m_aborted) {
            emit failed(QStringLiteral("aborted"), true);
        } else if (status == 0 && m_reply->error() == QNetworkReply::NoError) {
            // Local schemes (file://, qrc://) carry no HTTP status — a clean read IS a 200
            // (browsers report file: fetches the same way).
            emit finished(200, QStringLiteral("OK"), QVariantMap(), QString::fromUtf8(m_reply->readAll()));
        } else if (status > 0) {
            // A genuine HTTP response — resolve even for 4xx/5xx (fetch semantics).
            const QString statusText = m_reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
            QVariantMap headers;
            const auto pairs = m_reply->rawHeaderPairs();
            for (const auto &p : pairs) {
                const QString key = QString::fromUtf8(p.first).toLower();
                const QString val = QString::fromUtf8(p.second);
                // Per spec, repeated headers combine with ", ".
                headers.insert(key, headers.contains(key) ? headers.value(key).toString() + QStringLiteral(", ") + val : val);
            }
            emit finished(status, statusText, headers, QString::fromUtf8(m_reply->readAll()));
        } else {
            emit failed(m_reply->errorString(), false);
        }
        deleteLater();
    });
}

void WebFetchReply::abort()
{
    m_aborted = true;
    if (m_reply)
        m_reply->abort();
}

WebFetch::WebFetch(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

WebFetchReply *WebFetch::request(const QString &url, const QVariantMap &options)
{
    QNetworkRequest request{ QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Qt's QNAM HTTP/2 backend mishandles connection reuse after the peer sends GOAWAY / closes an
    // idle keep-alive channel (GitHub's API does this after a handful of requests), leaving a
    // dangling read on the dead socket ("QSslSocket: device not open") that then stalls every
    // subsequent fetch. HTTP/1.1's keep-alive path recovers cleanly by opening a fresh socket.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(options.value(QStringLiteral("timeout"), 30000).toInt());

    const QVariantMap headers = options.value(QStringLiteral("headers")).toMap();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());

    const QString method = options.value(QStringLiteral("method"), QStringLiteral("GET")).toString().toUpper();
    const QByteArray body = options.value(QStringLiteral("body")).toString().toUtf8();

    // sendCustomRequest is HTTP-only; local schemes (file://, qrc://) go through the plain GET
    // path so fetch() can read app assets (subtitles, templates) like a browser reads file: pages.
    const QUrl parsed(url);
    QNetworkReply *reply = (!parsed.scheme().startsWith(QLatin1String("http")) && method == QLatin1String("GET"))
        ? m_nam->get(request)
        : m_nam->sendCustomRequest(request, method.toUtf8(), body);
    return new WebFetchReply(reply, this);
}

// The WHATWG layer: Headers / Request / Response / AbortController / AbortSignal and fetch(),
// built on the C++ `Http` backend. V4 has classes, Map, Promise, Proxy, typed arrays.
static const char *kFetchShim = R"JS(
(function (Http) {
    function abortError() { var e = new Error("The operation was aborted."); e.name = "AbortError"; return e; }

    class Headers {
        constructor(init) {
            this._m = new Map();
            if (init) {
                if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
                else if (Array.isArray(init)) init.forEach(p => this.append(p[0], p[1]));
                else Object.keys(init).forEach(k => this.append(k, init[k]));
            }
        }
        append(n, v) { var k = String(n).toLowerCase(); var e = this._m.get(k); this._m.set(k, e == null ? String(v) : e + ", " + v); }
        set(n, v) { this._m.set(String(n).toLowerCase(), String(v)); }
        get(n) { var v = this._m.get(String(n).toLowerCase()); return v == null ? null : v; }
        has(n) { return this._m.has(String(n).toLowerCase()); }
        delete(n) { this._m.delete(String(n).toLowerCase()); }
        forEach(cb, t) { this._m.forEach((v, k) => cb.call(t, v, k, this)); }
        keys() { return this._m.keys(); }
        values() { return this._m.values(); }
        entries() { return this._m.entries(); }
    }

    class AbortSignal {
        constructor() { this.aborted = false; this.reason = undefined; this._l = []; this.onabort = null; }
        addEventListener(type, cb) { if (type === "abort") this._l.push(cb); }
        removeEventListener(type, cb) { if (type === "abort") { var i = this._l.indexOf(cb); if (i >= 0) this._l.splice(i, 1); } }
        _fire() { if (this.aborted) return; this.aborted = true; var e = { type: "abort" }; this._l.slice().forEach(cb => cb(e)); if (this.onabort) this.onabort(e); }
    }
    class AbortController {
        constructor() { this.signal = new AbortSignal(); }
        abort(reason) { this.signal.reason = reason; this.signal._fire(); }
    }

    class Response {
        constructor(body, init) {
            init = init || {};
            this._body = body == null ? "" : String(body);
            this.status = init.status !== undefined ? init.status : 200;
            this.statusText = init.statusText !== undefined ? init.statusText : "";
            this.ok = this.status >= 200 && this.status < 300;
            this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
            this.url = init.url || "";
            this.redirected = !!init.redirected;
            this.type = "basic";
            this.bodyUsed = false;
        }
        _consume() { if (this.bodyUsed) return Promise.reject(new TypeError("Body already consumed")); this.bodyUsed = true; return Promise.resolve(this._body); }
        text() { return this._consume(); }
        json() { return this._consume().then(t => JSON.parse(t)); }
        arrayBuffer() {
            return this._consume().then(t => {
                var b = new ArrayBuffer(t.length), u = new Uint8Array(b);
                for (var i = 0; i < t.length; ++i) u[i] = t.charCodeAt(i) & 0xff;
                return b;
            });
        }
        blob() { var self = this; return this._consume().then(t => ({ size: t.length, type: self.headers.get("content-type") || "", text: () => Promise.resolve(t) })); }
        clone() { var r = new Response(this._body, { status: this.status, statusText: this.statusText, headers: this.headers, url: this.url, redirected: this.redirected }); return r; }
    }

    class Request {
        constructor(input, init) {
            init = init || {};
            var base = (input instanceof Request) ? input : {};
            this.url = (input instanceof Request) ? input.url : String(input);
            this.method = String(init.method || base.method || "GET").toUpperCase();
            this.headers = new Headers(init.headers || base.headers);
            this.body = init.body !== undefined ? init.body : base.body;
            this.signal = init.signal || base.signal || null;
        }
    }

    function fetch(input, init) {
        init = init || {};
        var req = (input instanceof Request) ? input : new Request(input, init);
        var signal = init.signal || req.signal;
        var hdr = {};
        req.headers.forEach((v, k) => { hdr[k] = v; });
        return new Promise(function (resolve, reject) {
            if (signal && signal.aborted) { reject(abortError()); return; }
            var options = { method: req.method, headers: hdr };
            if (req.body !== undefined && req.body !== null) options.body = String(req.body);
            if (init.timeout) options.timeout = init.timeout;
            var reply = Http.request(req.url, options);
            var aborted = false;
            if (signal) signal.addEventListener("abort", function () { aborted = true; reply.abort(); });
            reply.finished.connect(function (status, statusText, headers, body) {
                resolve(new Response(body, { status: status, statusText: statusText, headers: headers, url: req.url }));
            });
            reply.failed.connect(function (error, wasAborted) {
                reject((aborted || wasAborted) ? abortError() : new TypeError("Network request failed: " + error));
            });
        });
    }

    // XMLHttpRequest backed by the same QNetworkAccessManager fetch transport — QML's built-in XHR
    // is weak, but its global is frozen (non-configurable) so we can't replace it in place. We expose
    // this under a distinct name; the npm mirror rewrites `XMLHttpRequest` references to it. Async
    // only (the spec's synchronous mode can't be honoured over an async transport — we warn + run
    // async). Drives the standard readyState/event sequence so libraries like axios/superagent work.
    class XMLHttpRequest {
        constructor() {
            this.readyState = 0; this.status = 0; this.statusText = "";
            this.responseText = ""; this.response = ""; this.responseURL = ""; this.responseType = "";
            this.timeout = 0; this.withCredentials = false;
            this.onreadystatechange = null; this.onloadstart = null; this.onprogress = null;
            this.onload = null; this.onerror = null; this.onabort = null; this.ontimeout = null; this.onloadend = null;
            this.upload = { addEventListener() {}, removeEventListener() {}, onprogress: null, onload: null, onerror: null };
            this._reqHeaders = new Headers(); this._respHeaders = null; this._controller = null;
            this._listeners = {}; this._sent = false; this._aborted = false;
            this._method = "GET"; this._url = ""; this._async = true;
        }
        addEventListener(type, cb) { (this._listeners[type] || (this._listeners[type] = [])).push(cb); }
        removeEventListener(type, cb) { var l = this._listeners[type]; if (l) { var i = l.indexOf(cb); if (i >= 0) l.splice(i, 1); } }
        _emit(type, extra) {
            var ev = Object.assign({ type: type, target: this, currentTarget: this, lengthComputable: false, loaded: 0, total: 0 }, extra || {});
            var on = this["on" + type]; if (typeof on === "function") on.call(this, ev);
            var l = this._listeners[type]; if (l) l.slice().forEach(cb => cb.call(this, ev));
        }
        _ready(s) { this.readyState = s; this._emit("readystatechange"); }
        open(method, url, async) {
            this._method = String(method || "GET").toUpperCase(); this._url = String(url);
            this._async = async === undefined ? true : !!async;
            this._reqHeaders = new Headers(); this._respHeaders = null; this._aborted = false; this._sent = false;
            this.status = 0; this.statusText = ""; this.responseText = ""; this.response = "";
            this._ready(1);
        }
        setRequestHeader(name, value) {
            if (this.readyState !== 1) throw new Error("InvalidStateError: setRequestHeader() before open()");
            this._reqHeaders.append(name, value);
        }
        getResponseHeader(name) { return this._respHeaders ? this._respHeaders.get(name) : null; }
        getAllResponseHeaders() {
            if (!this._respHeaders) return "";
            var out = []; this._respHeaders.forEach((v, k) => out.push(k + ": " + v));
            return out.length ? out.join("\r\n") + "\r\n" : "";
        }
        overrideMimeType() {}
        abort() {
            this._aborted = true;
            if (this._controller) this._controller.abort();
            this.readyState = 0;
            if (this._sent) { this._emit("abort"); this._emit("loadend"); }
        }
        send(body) {
            if (this.readyState !== 1) throw new Error("InvalidStateError: send() before open()");
            if (!this._async) console.warn("XMLHttpRequest: synchronous requests are not supported; running asynchronously");
            this._sent = true;
            var self = this, hdr = {};
            this._reqHeaders.forEach((v, k) => { hdr[k] = v; });
            this._controller = new AbortController();
            var init = { method: this._method, headers: hdr, signal: this._controller.signal };
            if (body != null && this._method !== "GET" && this._method !== "HEAD") init.body = body;
            if (this.timeout) init.timeout = this.timeout;
            var timedOut = false, timer = this.timeout ? setTimeout(() => { timedOut = true; self._controller.abort(); }, this.timeout) : null;
            this._emit("loadstart");
            fetch(this._url, init).then(function (res) {
                if (self._aborted) return;
                if (timer) clearTimeout(timer);
                self.status = res.status; self.statusText = res.statusText;
                self.responseURL = res.url || self._url; self._respHeaders = res.headers;
                self._ready(2); self._ready(3);
                var rt = self.responseType;
                return (rt === "arraybuffer" ? res.arrayBuffer() : res.text()).then(function (data) {
                    if (self._aborted) return;
                    if (rt === "arraybuffer") { self.response = data; self.responseText = ""; }
                    else if (rt === "json") { self.responseText = data; try { self.response = JSON.parse(data); } catch (e) { self.response = null; } }
                    else { self.responseText = data; self.response = data; }
                    self._ready(4); self._emit("load"); self._emit("loadend");
                });
            }).catch(function (err) {
                if (timer) clearTimeout(timer);
                if (self._aborted && !timedOut) return;
                self.readyState = 4; self._emit("readystatechange");
                self._emit(timedOut ? "timeout" : "error", { error: err }); self._emit("loadend");
            });
        }
    }
    XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
    XMLHttpRequest.prototype.UNSENT = 0; XMLHttpRequest.prototype.OPENED = 1; XMLHttpRequest.prototype.HEADERS_RECEIVED = 2;
    XMLHttpRequest.prototype.LOADING = 3; XMLHttpRequest.prototype.DONE = 4;

    return { fetch: fetch, Headers: Headers, Request: Request, Response: Response, AbortController: AbortController, AbortSignal: AbortSignal, __SqXMLHttpRequest: XMLHttpRequest };
})(__httpBackend)
)JS";

void WebFetch::install(QQmlEngine *engine)
{
    if (!engine)
        return;
    auto *backend = new WebFetch(engine);
    QQmlEngine::setObjectOwnership(backend, QQmlEngine::CppOwnership);

    QJSValue global = engine->globalObject();
    global.setProperty(QStringLiteral("__httpBackend"), engine->newQObject(backend));

    const QJSValue api = engine->evaluate(QString::fromUtf8(kFetchShim));
    if (api.isError()) {
        qWarning().noquote() << "fetch: shim failed:" << api.toString();
        return;
    }
    for (const QString &name : { QStringLiteral("fetch"), QStringLiteral("Headers"), QStringLiteral("Request"),
                                 QStringLiteral("Response"), QStringLiteral("AbortController"), QStringLiteral("AbortSignal"),
                                 QStringLiteral("__SqXMLHttpRequest") })
        global.setProperty(name, api.property(name));
}
