# C++ API reference

Headers live under `src/qmlcss/`. After adding `qml_css_engine_dep` as a Meson
dependency the include path is set automatically:

```cpp
#include "qmlcss/csstheme.h"
#include "qmlcss/csslayout.h"
#include "qmlcss/valueparser.h"   // low-level parsing utilities (rarely needed directly)
```

---

## `CssTheme`

```cpp
class CssTheme : public QObject
```

The CSS parser and cascade engine. Owns the active rule set, `@keyframes` dictionary,
`@media` groups, and the registry of styled objects (the reverse-slot push model).

### Construction

```cpp
explicit CssTheme(QObject *parent = nullptr);
```

### Properties (Q_PROPERTY)

| Property | Type | Signal | Description |
|----------|------|--------|-------------|
| `loaded` | `bool` | `loadedChanged()` | `true` after the first successful `loadLayered`/`loadFromString` |
| `viewportWidth` | `qreal` | `viewportChanged()` | Current viewport width (set by `setViewport`) |
| `viewportHeight` | `qreal` | `viewportChanged()` | Current viewport height |

### Loading stylesheets

```cpp
void load(const QString &path);
```
Convenience: calls `loadLayered({path})`.

---

```cpp
void loadLayered(const QStringList &paths);
```
Load several stylesheets as one cascade (not Q_INVOKABLE — call from C++). Each file's
`@import` statements are resolved relative to its own directory. Files are concatenated
in order so later entries override earlier ones. All present files are watched for
hot-reload. Skips missing paths silently.

---

```cpp
Q_INVOKABLE void loadLayeredString(const QString &generatedCss);
```
Append a **generated CSS string** as the highest-priority layer. The string is
re-appended after every file-change reload, so build-generated inline-style rules
always win the cascade. Idempotent (no-op when the string and the loaded state are
unchanged). Callable from QML.

---

```cpp
void loadFromString(const QString &css);
```
Parse and apply a raw CSS string immediately (replaces the current cascade). Useful
for tests and dynamic CSS.

---

### Viewport

```cpp
Q_INVOKABLE void setViewport(qreal width, qreal height);
```
Report the current window size so `@media (min/max-width/height: …)` blocks are
evaluated correctly and `vw`/`vh` units resolve. Re-filters active rules and re-pushes
to every registered item. Cheap no-op when the size is unchanged. Callable from QML.

---

### Style resolution

These methods are Q_INVOKABLE (usable from QML) and are the low-level API that
`loadCss()` uses internally.

```cpp
Q_INVOKABLE QVariantMap resolve(
    const QString &id,
    const QStringList &classes = {},
    const QString &pseudoElement = {}) const;
```
Resolve the merged style for element `id` with state `classes`. `pseudoElement`
(`"before"` or `"after"`) selects overlay rules; leave empty for ordinary selectors.

---

```cpp
Q_INVOKABLE QVariantMap resolveWith(
    const QString &contextId,
    const QString &id,
    const QStringList &classes = {},
    const QString &pseudoElement = {}) const;
```
Resolve with **ancestor context** — matches descendant rules like `#workspaces button`.
`contextId` is the ancestor's CSS id.

---

```cpp
Q_INVOKABLE QVariantMap resolveExact(
    const QString &id,
    const QStringList &classes = {},
    const QString &pseudoElement = {},
    const QString &requiredClass = {}) const;
```
Like `resolve()` but excludes universal (`*`) rules. When `requiredClass` is set,
further restricts to rules whose selector explicitly contains that class (used
internally by `resolvePart`).

---

```cpp
Q_INVOKABLE QVariantMap resolvePart(
    const QString &id,
    const QString &part,
    const QStringList &classes = {},
    const QString &pseudoElement = {}) const;
```
Resolve a named **part** of a module: returns only rules whose selector requires
`part` as a class (e.g., `#cpu.graph { … }` for `resolvePart("cpu", "graph")`).
The bare `#id` base is excluded, so a sub-element does not inherit the container's
own background.

---

### The reverse slot — `loadCss`

```cpp
Q_INVOKABLE void loadCss(QObject *target);
```
Register `target` with the engine and immediately apply the resolved style to its
`style` property. On every subsequent theme reload or `cssClass`/`cssState` change,
the engine re-pushes styles automatically (no re-call needed). Dead targets are pruned
via `QObject::destroyed`.

The target **must** carry the CssQmlItem signature (`cssId` + `style` properties);
a target that lacks them is rejected with a `qWarning` and not registered.

---

### Parsing helpers (Q_INVOKABLE)

All callable from QML via `cssTheme.<method>(…)`.

```cpp
Q_INVOKABLE QColor parseColor(const QString &cssColor) const;
```
Parse a CSS colour string to `QColor`. Supports named keywords, `#rgb`, `#rrggbb`,
`#rrggbbaa`, `#aarrggbb`, `rgb(…)`, `rgba(…)`.

---

```cpp
Q_INVOKABLE qreal parseLength(const QString &value, qreal fallback) const;
```
Parse a CSS length (`"11px"`, `"8"`) to pixels. Returns `fallback` when unparseable.

---

```cpp
Q_INVOKABLE qreal parseFontSize(const QString &value, qreal fallbackPt) const;
```
Resolve a CSS font-size string to **points** for QML's `font.pointSize`. Handles `px`
(×72/96), `pt` and bare numbers (direct), `em`/`rem` (×`fallbackPt`).

---

```cpp
Q_INVOKABLE QVariantMap parseGradient(const QString &cssValue) const;
```
Parse a single `linear-gradient(…)` or `radial-gradient(…)` value.

Returns (linear): `{ "type": "linear", "angle": <deg>, "stops": [ { "position": 0..1, "color": QColor }, … ] }`  
Returns (radial): `{ "type": "radial", "cx": 0..1, "cy": 0..1, "stops": [ … ] }`  
Returns empty map when the value is not a recognised gradient.

---

```cpp
Q_INVOKABLE QVariantList parseGradientLayers(const QString &cssValue) const;
```
Parse a `background` value with potentially several comma-separated layers. The first
listed layer paints on top. Each entry is either a gradient map (as above) or
`{ "type": "color", "color": QColor }`.

---

```cpp
Q_INVOKABLE QVariantMap parseBorder(const QString &cssValue) const;
```
Parse the `border` shorthand (`<width> <style> <color>`, any subset/order).  
Returns `{ "width": <px>, "style": <string>, "color": QColor }` for the parts present.

---

```cpp
Q_INVOKABLE QVariantMap parseBoxShadow(const QString &cssValue) const;
```
Parse `box-shadow: [inset] <x> <y> <blur> [<spread>] <color>` (first shadow only).  
Returns `{ "x", "y", "blur", "spread" (px), "color": QColor, "inset": bool }` or
empty map when the value is `none` or unset.

---

```cpp
Q_INVOKABLE QVariantList parseBoxShadowList(const QString &cssValue) const;
```
Parse the full comma-separated `box-shadow` list. Each entry has the same shape as
`parseBoxShadow`.

---

```cpp
Q_INVOKABLE int parseDuration(const QString &cssValue, int fallbackMs) const;
```
Parse a CSS duration: `"180ms"`, `"0.76s"`, or a bare number (milliseconds).
Returns `fallbackMs` on failure.

---

```cpp
Q_INVOKABLE int parseEasing(const QString &cssValue, int fallbackType) const;
```
Parse a CSS/QML timing-function name to a `QEasingCurve::Type` integer.
Supported names: `linear`, `ease`, `ease-in`, `ease-out`, `ease-in-out`, `out-cubic`,
and other Qt easing names. Returns `fallbackType` for unknown names.

---

```cpp
Q_INVOKABLE QVariantMap parseTransition(const QString &cssValue) const;
```
Parse the `transition` shorthand (first comma segment).  
Returns `{ "property": string, "duration": int (ms), "easing": int (QEasingCurve::Type), "delay": int (ms) }`.
The first `<time>` is duration, the second is delay (per CSS spec).

---

```cpp
Q_INVOKABLE QVariantMap parseAnimation(const QString &cssValue) const;
```
Parse the `animation` shorthand (first comma segment, order-independent).  
Returns `{ "name": string, "duration": int (ms), "delay": int (ms), "easing": int, "iterations": int (-1 = infinite), "direction": string }`.

---

```cpp
Q_INVOKABLE QVariantMap parseTransform(const QString &cssValue) const;
```
Parse a `transform` value. Supported functions: `rotate()`, `scale()`, `scaleX()`,
`scaleY()`, `translate()`, `translateX()`, `translateY()`. Angle units: `deg`, `rad`,
`turn`.  
Returns `{ "rotate": deg, "scale": 1, "scaleX": 1, "scaleY": 1, "translateX": px, "translateY": px }`.
Absent components default to the identity.

---

```cpp
Q_INVOKABLE QVariantList keyframes(const QString &name) const;
```
Return the frames of a `@keyframes <name>` block as
`[ { "offset": 0..1, "properties": { … } }, … ]`, sorted by offset.
Returns an empty list when no such keyframes were defined.

---

### Style prelude (C++ only)

```cpp
void setStylePrelude(const QString &css);
```
Set a CSS string that is prepended at the **lowest** priority on every load. Useful
for injecting config-derived defaults (e.g. transition durations from application
settings) that the user's CSS can override. Has no effect until the next `loadLayered`
call.

---

## `CssLayoutEngine`

```cpp
class CssLayoutEngine : public QObject
```

The box-model layout engine. Evaluates flex/grid/block layout, all CSS length
expressions, and drives transform-keyframe animation interpolation.

### Construction

```cpp
explicit CssLayoutEngine(CssTheme *theme, QObject *parent = nullptr);
```
Requires a `CssTheme` instance to resolve viewport dimensions for `vw`/`vh` units.

### Layout methods

```cpp
Q_INVOKABLE void requestLayout(QQuickItem *root, QQuickItem *content);
```
Queue a (re)layout of `root`'s children (held by `content`). Requests are coalesced:
multiple calls within the same event-loop turn produce one layout pass.

---

```cpp
Q_INVOKABLE void layout(QQuickItem *root, QQuickItem *content);
```
Run a layout pass immediately (bypasses coalescing). Prefer `requestLayout` in
response to geometry/style changes.

---

### Length evaluation

```cpp
Q_INVOKABLE qreal length(const QString &value, qreal avail) const;
```
Evaluate a CSS length expression against an available axis size `avail`. Returns
`NaN` (`qQNaN()`) when the value is absent, `"auto"`, or otherwise non-numeric.
Supports `px`, `%`, `vw`, `vh`, `vmin`, `vmax`, `calc()`, `min()`, `max()`,
`clamp()`.

---

### Animation helpers

```cpp
Q_INVOKABLE QVariantList buildAnimStops(const QVariantList &frames) const;
```
Normalise a list of `@keyframes` frames (as returned by `CssTheme::keyframes()`) into
animation stops suitable for interpolation:
`[ { "offset": 0..1, "rotate": deg, "scale": 1, "tx": px, "ty": px }, … ]`.
Synthesises a `0%` and `100%` stop at the identity transform when the keyframes do not
include them. Returns an empty list when `frames` is empty.

---

```cpp
Q_INVOKABLE void applyAnim(QQuickItem *root, const QVariantList &stops, qreal progress) const;
```
Interpolate the `stops` at `progress` (0..1) and write the result into `root`'s
`_animRotate`, `_animScale`, `_animTx`, `_animTy` properties. Called by `CssRect`
on every animation tick.

---

## `CssValueParser` namespace

Low-level parsing utilities in `src/qmlcss/valueparser.h`. Used internally by
`CssTheme`; exposed for consumers that need parsing without a full `CssTheme` instance.

```cpp
namespace CssValueParser {

QColor parseColor(const QString &cssColor);

QStringList splitTopLevel(const QString &text, QChar sep);
QStringList splitTopLevelWhitespace(const QString &text);

bool parseLengthPx(const QString &token, double *out);
int parseDuration(const QString &cssValue, int fallbackMs);
QEasingCurve::Type parseEasing(const QString &cssValue, QEasingCurve::Type fallback);
QVariantMap parseTransition(const QString &cssValue);
QVariantMap parseAnimation(const QString &cssValue);

} // namespace CssValueParser
```

`splitTopLevel` splits on `sep` while respecting nested parentheses (so commas inside
`rgba()` or `linear-gradient()` are not treated as list separators).
`splitTopLevelWhitespace` does the same for whitespace tokens.

## Registration — `QMLCss.h`

One include, one call registers every type under `import qmlcss 1.0`:

```cpp
#include "qmlcss/QMLCss.h"
QmlCss::registerTypes();
```

## Newer `CssTheme` API

- `resolveWithAncestors(ancestors, id, classes, pseudo, primitive)` — C++-side resolve
  with an explicit outer→inner ancestor chain (what `applyCssTo` builds from the item
  tree; powers web-parity descendant scoping).
- `setLayoutEngine(CssLayoutEngine *)` — wired automatically by the layout engine's
  constructor; lets bulk apply passes hibernate the layout.
- `loadLayeredString(css)` — the runtime override layer (theme switching): appended
  last, highest priority, re-applied to every live element in one batched pass.

## `CssLayoutEngine` batching

`beginBatch()` / `endBatch()` — while a batch is open, `requestLayout()` only records;
`endBatch()` runs ONE flush for everything. Bulk passes inside the theme bracket
themselves; brackets nest (a depth counter). Outside a batch the flush is synchronous.

## `QmlCss::cachedComponent`

`cachedComponent(engine, key, qml)` (componentcache.h) — one compiled `QQmlComponent`
per (engine, key), reused for every `create()`. Use it for any composed snippet; a
per-instance `QQmlComponent::setData` recompiles the same source every time.
