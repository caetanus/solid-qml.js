#pragma once

class QQmlEngine;

// Additive ECMAScript standard-library polyfills, installed once at engine setup. V4's baseline is
// ECMAScript 7th edition (ES2016); this layers on the widely-used prototype/static methods added by
// later editions (ES2019-ES2023) for the builtin types V4 already supports — bringing mirrored npm
// packages and app code up to "current JS" for those types.
//
// Strictly ADDITIVE: each method is installed only if missing (`if (proto[name]) return;`), so the
// engine's existing (frozen, non-configurable) builtins are never touched — V4 forbids replacing
// them anyway (a *broken* builtin like Function.prototype.apply must be fixed by call-site rewrite,
// not here). Pure-JS implementations evaluated in the engine's single realm, so the methods are
// visible to QML JS and to imported .mjs modules alike.
class JsPolyfill {
public:
    static void install(QQmlEngine *engine);
};
