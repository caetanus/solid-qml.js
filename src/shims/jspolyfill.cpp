#include "jspolyfill.h"

#include <QDebug>
#include <QJSEngine>
#include <QQmlEngine>

// Additive ES2019-ES2023 standard-library backfill for the builtin types V4 (ES2016) supports.
// Every method is added only when absent and inside try/catch, so an unsupported/frozen target can
// never abort the rest. Implementations follow the ECMAScript spec semantics; where it is simpler
// and equivalent, missing methods delegate to existing engine builtins (e.g. replaceAll/matchAll
// build a global RegExp so the engine's own replace/exec drives the result).
static const char *kPolyfill = R"JS(
(function () {
    function def(proto, name, fn) {
        if (!proto || proto[name]) return;
        try { Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true }); } catch (e) {}
    }
    function reEscape(s) { return String(s).replace(/[.*+?^${}()|[\]\\]/g, "\\$&"); }

    // ---- String.prototype (ES2019: trimStart/trimEnd; ES2020: matchAll; ES2021: replaceAll; ES2022: at) ----
    def(String.prototype, "trimStart", function () { return String(this).replace(/^\s+/, ""); });
    def(String.prototype, "trimEnd", function () { return String(this).replace(/\s+$/, ""); });
    def(String.prototype, "trimLeft", String.prototype.trimStart);
    def(String.prototype, "trimRight", String.prototype.trimEnd);
    def(String.prototype, "replaceAll", function (search, replacement) {
        var s = String(this);
        if (search instanceof RegExp) {
            if (String(search.flags).indexOf("g") === -1)
                throw new TypeError("String.prototype.replaceAll called with a non-global RegExp argument");
            return s.replace(search, replacement);
        }
        // Drive the engine's own replace via a global RegExp so $-patterns and function replacers
        // (and empty-search semantics) match the spec exactly.
        return s.replace(new RegExp(reEscape(search), "g"), replacement);
    });
    def(String.prototype, "matchAll", function (regexp) {
        var s = String(this), re;
        if (regexp instanceof RegExp) {
            if (String(regexp.flags).indexOf("g") === -1)
                throw new TypeError("String.prototype.matchAll called with a non-global RegExp argument");
            re = new RegExp(regexp.source, regexp.flags);
        } else {
            re = new RegExp(regexp === undefined ? "" : reEscape(regexp), "g");
        }
        var matches = [], m;
        while ((m = re.exec(s)) !== null) { matches.push(m); if (m[0] === "") re.lastIndex++; }
        return matches[Symbol.iterator]();
    });
    def(String.prototype, "at", function (index) {
        var s = String(this), n = s.length, i = Math.trunc(+index) || 0;
        if (i < 0) i += n;
        return (i < 0 || i >= n) ? undefined : s.charAt(i);
    });

    // ---- Array.prototype (ES2019: flat/flatMap; ES2022: at; ES2023: findLast*/change-by-copy) ----
    def(Array.prototype, "at", function (index) {
        var len = this.length >>> 0, i = Math.trunc(+index) || 0;
        if (i < 0) i += len;
        return (i < 0 || i >= len) ? undefined : this[i];
    });
    def(Array.prototype, "flat", function (depth) {
        var d = depth === undefined ? 1 : (Math.trunc(+depth) || 0);
        return (function rec(arr, d) {
            var out = [];
            for (var i = 0; i < arr.length; i++) {
                if (!(i in arr)) continue;
                if (d > 0 && Array.isArray(arr[i])) out = out.concat(rec(arr[i], d - 1));
                else out.push(arr[i]);
            }
            return out;
        })(Object(this), d);
    });
    def(Array.prototype, "flatMap", function (cb, thisArg) {
        var o = Object(this), out = [];
        for (var i = 0; i < (o.length >>> 0); i++) {
            if (!(i in o)) continue;
            var v = cb.call(thisArg, o[i], i, o);
            if (Array.isArray(v)) out = out.concat(v); else out.push(v);
        }
        return out;
    });
    def(Array.prototype, "findLast", function (cb, thisArg) {
        var o = Object(this);
        for (var i = (o.length >>> 0) - 1; i >= 0; i--) if (cb.call(thisArg, o[i], i, o)) return o[i];
        return undefined;
    });
    def(Array.prototype, "findLastIndex", function (cb, thisArg) {
        var o = Object(this);
        for (var i = (o.length >>> 0) - 1; i >= 0; i--) if (cb.call(thisArg, o[i], i, o)) return i;
        return -1;
    });
    def(Array.prototype, "toReversed", function () { return Array.prototype.slice.call(this).reverse(); });
    def(Array.prototype, "toSorted", function (cmp) { return Array.prototype.slice.call(this).sort(cmp); });
    def(Array.prototype, "toSpliced", function () {
        var a = Array.prototype.slice.call(this);
        Array.prototype.splice.apply(a, arguments);
        return a;
    });
    def(Array.prototype, "with", function (index, value) {
        var a = Array.prototype.slice.call(this), i = Math.trunc(+index) || 0;
        if (i < 0) i += a.length;
        if (i < 0 || i >= a.length) throw new RangeError("Invalid index : " + index);
        a[i] = value;
        return a;
    });

    // ---- TypedArray.prototype.at (ES2022) — patch the shared %TypedArray% prototype once ----
    var taProto = (typeof Uint8Array !== "undefined") ? Object.getPrototypeOf(Uint8Array.prototype) : null;
    def(taProto, "at", function (index) {
        var len = this.length >>> 0, i = Math.trunc(+index) || 0;
        if (i < 0) i += len;
        return (i < 0 || i >= len) ? undefined : this[i];
    });

    // ---- Object statics (ES2019: fromEntries; ES2022: hasOwn) ----
    def(Object, "fromEntries", function (iterable) {
        var o = {};
        for (var pair of iterable) o[pair[0]] = pair[1];
        return o;
    });
    def(Object, "hasOwn", function (obj, key) { return Object.prototype.hasOwnProperty.call(obj, key); });

    // ---- Promise.allSettled (ES2020) ----
    if (typeof Promise === "function") {
        def(Promise, "allSettled", function (iterable) {
            var ps = Array.prototype.map.call(Array.from ? Array.from(iterable) : iterable, function (p) {
                return Promise.resolve(p).then(
                    function (value) { return { status: "fulfilled", value: value }; },
                    function (reason) { return { status: "rejected", reason: reason }; });
            });
            return Promise.all(ps);
        });
    }
})()
)JS";

void JsPolyfill::install(QQmlEngine *engine)
{
    if (!engine)
        return;
    const QJSValue result = engine->evaluate(QString::fromUtf8(kPolyfill));
    if (result.isError())
        qWarning().noquote() << "js polyfill failed:" << result.toString();
}
