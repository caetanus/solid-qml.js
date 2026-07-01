#pragma once

#include <QByteArray>
#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class QNetworkAccessManager;

struct CssSimpleSelector {
    bool universal = false;
    QString element;
    QString id;
    QStringList classes;
    // Pseudo-element after `::` (e.g. "before"/"after"), used for decorative
    // sub-overlays. Empty for ordinary selectors.
    QString pseudoElement;
    int specificity = 0;
};

struct CssRule {
    QString requiredAncestorId;
    CssSimpleSelector selector;
    QVariantMap properties;
    // Subset of `properties` declared `!important` — they win the cascade over
    // non-important declarations regardless of specificity.
    QVariantMap importantProperties;
};

// A single `(<feature>: <px>)` constraint of a media query (e.g. max-width 720).
struct CssMediaFeature {
    QString name; // min-width / max-width / min-height / max-height
    double value = 0;
};

// A parsed `@media` condition: OR of comma queries, each an AND of feature constraints.
struct CssMediaQuery {
    QList<QList<CssMediaFeature>> orGroups;
};

// An `@media` block: its condition plus the rules it contributes when the condition holds.
struct CssMediaGroup {
    CssMediaQuery query;
    QList<CssRule> rules;
};

// A parsed `@font-face` rule: the declared family name and the (first) source URL. The engine
// downloads the URL (cached on disk) and registers the bytes with QFontDatabase so
// `font-family: "<family>"` resolves without the font being installed on the system.
struct CssFontFace {
    QString family;
    QString url;
};

class CssTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loaded READ isLoaded NOTIFY loadedChanged)
    // Current viewport (window) size — exposed so QML can resolve `vw`/`vh` units.
    Q_PROPERTY(qreal viewportWidth READ viewportWidth NOTIFY viewportChanged)
    Q_PROPERTY(qreal viewportHeight READ viewportHeight NOTIFY viewportChanged)
    // Bumped each time a downloaded `@font-face` registers. Text bindings that resolve a
    // font-family reference it so they re-evaluate once the (async) web font arrives.
    Q_PROPERTY(int fontRevision READ fontRevision NOTIFY fontRevisionChanged)

public:
    explicit CssTheme(QObject *parent = nullptr);

    bool isLoaded() const { return m_loaded; }
    qreal viewportWidth() const { return m_viewportWidth; }
    qreal viewportHeight() const { return m_viewportHeight; }
    int fontRevision() const { return m_fontRevision; }

    void load(const QString &path);
    // Load several stylesheets as one cascade, in order (e.g. [baseStyleSheet, styleSheet]):
    // each file's @imports are expanded relative to it, then they're concatenated so later
    // files override earlier ones. Missing paths are skipped. All are watched for reload.
    void loadLayered(const QStringList &paths);
    // Set the "generated" CSS layer — content produced by the build (e.g. inline-style
    // rules the transpiler synthesised into `#id { ... }`), passed as a string rather than a
    // file. It is appended LAST (highest priority) to the layered stylesheet and re-applied
    // on every reload. Invokable so the generated QML can hand it over at startup.
    Q_INVOKABLE void loadLayeredString(const QString &generatedCss);
    void loadFromString(const QString &css);

    // Report the viewport (window) size so `@media (min/max-width|height: …)` blocks switch
    // on/off live. Re-filters the active rules and re-pushes to every styled object. Cheap
    // no-op when unchanged.
    Q_INVOKABLE void setViewport(qreal width, qreal height);

    // Resolve styles for an element (top-level selectors only). `pseudoElement`
    // selects a `::before`/`::after` overlay; empty (default) matches only ordinary
    // selectors, so existing callers never pick up overlay rules by accident.
    // `primitive` matches a type selector (e.g. `button`) against the element's cssPrimitive.
    Q_INVOKABLE QVariantMap resolve(const QString &id, const QStringList &classes = {},
                                    const QString &pseudoElement = {}, const QString &primitive = {}) const;

    // Resolve with ancestor context — matches descendant selectors like "#workspaces button.focused"
    Q_INVOKABLE QVariantMap resolveWith(const QString &contextId, const QString &id, const QStringList &classes = {},
                                        const QString &pseudoElement = {}) const;

    // Resolve only rules that explicitly target this id — universal (*) rules are excluded.
    // Use this when you want to apply id-specific overrides on top of an already-resolved base.
    // `requiredClass`, when set, additionally restricts to rules carrying that class
    // (this is what powers resolvePart).
    Q_INVOKABLE QVariantMap resolveExact(const QString &id, const QStringList &classes = {},
                                         const QString &pseudoElement = {}, const QString &requiredClass = {}) const;

    // Resolve a named "part" of a module: only rules whose selector REQUIRES `part`
    // as a class, e.g. resolvePart("cpu", "graph") for `#cpu.graph { ... }`. Unlike
    // resolve/resolveExact it does NOT include the bare `#cpu` base, so a part can
    // use standard props (background-color, color, width) without inheriting the
    // module's own. Extra `classes` allow part state, e.g. `#cpu.graph:active`;
    // `pseudoElement` selects a `::before`/`::after` overlay of the part.
    Q_INVOKABLE QVariantMap resolvePart(const QString &id, const QString &part, const QStringList &classes = {},
                                        const QString &pseudoElement = {}) const;

    // Parse a CSS color string to a QColor
    Q_INVOKABLE QColor parseColor(const QString &cssColor) const;

    // Parse a CSS length ("11px", "8") to pixels, or `fallback` when unparseable.
    Q_INVOKABLE qreal parseLength(const QString &value, qreal fallback) const;
    // Resolve a CSS font-size to POINTS (px→pt ×72/96, em/rem×fallbackPt, pt/bare→pt),
    // centrally — components consume the result directly, no per-component unit math.
    Q_INVOKABLE qreal parseFontSize(const QString &value, qreal fallbackPt) const;
    // Resolve a CSS font-family list (`"Helvetica Neue", Arial, sans-serif`) to one Qt
    // family installed on the host. QFont takes a single family; CSS takes a fallback list.
    Q_INVOKABLE QString resolveFontFamily(const QString &value, const QString &fallback = {}) const;
    Q_INVOKABLE int fontWeight(const QString &value) const;
    Q_INVOKABLE bool isProportionalLineHeight(const QString &value) const;
    Q_INVOKABLE qreal parseLineHeight(const QString &value, qreal fallbackPx) const;
    Q_INVOKABLE qreal boxSideLength(const QVariantMap &style, const QString &prefix, int side) const;
    Q_INVOKABLE bool hasSideBorder(const QVariantMap &style) const;
    Q_INVOKABLE QVariantMap borderSide(const QVariantMap &style, const QString &side,
                                       qreal defaultWidth, const QColor &defaultColor) const;

    // Parse `linear-gradient(<angle|to side>, <color> [<pos%>], ...)`.
    // Returns { "type": "linear", "angle": <deg, CSS convention>,
    //           "stops": [ { "position": <0..1>, "color": <QColor> }, ... ] }
    // or an empty map when the value is not a gradient.
    Q_INVOKABLE QVariantMap parseGradient(const QString &cssValue) const;

    // Parse a `background` value into its stacked layers (the FIRST listed paints on TOP).
    // Each entry is a gradient map (as parseGradient) or { "type": "color", "color": QColor }.
    // Handles a single layer too. Used by CssRect to render radial gradients and multi-layer
    // backgrounds the single-gradient path can't.
    Q_INVOKABLE QVariantList parseGradientLayers(const QString &cssValue) const;

    // Parse the `border` shorthand (`<width> <style> <color>`, any subset/order) into
    // { width (px), style, color (QColor) }. Lets components honour `border: 1px solid #...`
    // without the author having to split it into border-width/border-color.
    Q_INVOKABLE QVariantMap parseBorder(const QString &cssValue) const;

    // Parse `box-shadow: [inset] <x> <y> <blur> [spread] <color>`.
    // Returns { "x","y","blur","spread" (px), "color" (QColor), "inset" (bool) }
    // or an empty map when unset/none. (First shadow only.)
    Q_INVOKABLE QVariantMap parseBoxShadow(const QString &cssValue) const;

    // Parse a full comma-separated `box-shadow` list into one map per shadow (same
    // shape as parseBoxShadow). Standard CSS allows several shadows — e.g. a sunken
    // bevel is a dark `inset` plus a light `inset` — so a light bevel edge needs no
    // custom property.
    Q_INVOKABLE QVariantList parseBoxShadowList(const QString &cssValue) const;
    // Parse CSS-ish duration values: "180ms", "0.76s", or bare milliseconds.
    Q_INVOKABLE int parseDuration(const QString &cssValue, int fallbackMs) const;

    // Parse CSS/QML easing names into QEasingCurve::Type integer values.
    Q_INVOKABLE int parseEasing(const QString &cssValue, int fallbackType) const;

    // Parse the standard CSS `transition` shorthand into
    // { property, duration (ms), easing (QEasingCurve::Type), delay (ms) }.
    Q_INVOKABLE QVariantMap parseTransition(const QString &cssValue) const;

    // Parse the standard CSS `animation` shorthand into { name, duration (ms), delay (ms),
    // easing (QEasingCurve::Type), iterations (int; -1 = infinite), direction }.
    Q_INVOKABLE QVariantMap parseAnimation(const QString &cssValue) const;

    // Parse the CSS `transform` value into { rotate (degrees), scale, scaleX, scaleY,
    // translateX (px), translateY (px) }. Supports rotate()/scale()/scaleX/scaleY/
    // translate()/translateX/translateY; unrecognised functions are ignored. Absent
    // components default to the identity (rotate 0, scale 1, translate 0).
    Q_INVOKABLE QVariantMap parseTransform(const QString &cssValue) const;

    // Frames of a `@keyframes <name>` block: a list of { offset (0..1), properties },
    // sorted by offset. Empty when no such keyframes were defined.
    Q_INVOKABLE QVariantList keyframes(const QString &name) const;

    // Reverse-slot CSS application. The target carries its own identity, so we read it
    // straight off the object: `cssId`, the optional waybar-compat alias `cssAlternateId`
    // (string or list), the state `cssClass` (string or list), and `cssPrimitive` (or the
    // type itself). loadCss resolves the merged rules (primary id wins over the alias),
    // APPLIES them to `target`, and REGISTERS `target` so the engine re-applies on every
    // theme (re)load — the binding-free, reload-safe path: the engine PUSHES to each live
    // object (an inverted/"reverse" slot, engine→object), so no QML binding is needed and
    // an imperative apply can't go stale. A component re-calls loadCss(this) when its own
    // cssClass changes. Dead targets are pruned automatically.
    Q_INVOKABLE void loadCss(QObject *target);

    // A CSS "prelude" prepended (lowest priority) to the layered stylesheet on every load.
    // It carries config-derived defaults translated to CSS — e.g. a group's drawer
    // `transition-duration` becomes `#<name> { transition: <ms>ms }` — so the theme CSS
    // stays the single source of truth and can override them. Stored across reloads; the
    // caller re-loads (loadLayered) to apply. Setting the same value is a no-op.
    void setStylePrelude(const QString &css);

signals:
    void loadedChanged();
    void viewportChanged();
    void fontRevisionChanged();

private slots:
    void onCssFileChanged(const QString &path);
    // Connected to a registered target's `cssClass` NOTIFY: re-applies that target's
    // style when its state classes change (focused/urgent/hover/…). Uses sender().
    void reapplyForSender();

private:
    QVariantMap resolveImpl(const QString &contextId, const QString &id, const QStringList &classes,
                            const QString &pseudoElement, const QString &primitive = {}) const;

    // Merge the waybar alias(es) under the primary id (primary wins), for `classes`.
    QVariantMap resolveMerged(const QString &cssId, const QStringList &alternateIds,
                              const QStringList &classes, const QString &primitive = {}) const;
    // Read the target's identity properties, resolve, and push the style onto it (the
    // actual "apply the rules" step). Re-reads the object's current cssClass each time.
    void applyCssTo(QObject *target) const;
    // Reverse slot: re-push to every live registered target on (re)load; prune the dead.
    void reapplyAll();

    // Rebuild m_rules = base rules + the rules of every @media block whose condition matches
    // the current viewport. Called on (re)load and on viewport change.
    void rebuildRules();

    // Register a parsed @font-face: load from the on-disk cache if present, else download it
    // (async, HTTP/2 off) and cache it. Deduped by URL across reloads so a font is fetched once.
    void registerFontFace(const CssFontFace &face);
    // Add raw font bytes to QFontDatabase, then bump fontRevision + re-push styles so text using
    // the now-available family re-resolves.
    void registerFontData(const QByteArray &bytes);

    QList<CssRule> m_rules;        // active cascade (base + matching @media), what resolve() reads
    QList<CssRule> m_baseRules;    // rules outside any @media (always active)
    QList<CssMediaGroup> m_mediaGroups; // @media blocks, applied when their query matches
    qreal m_viewportWidth = 0;
    qreal m_viewportHeight = 0;
    // Parsed @keyframes by name (each a list of { offset, properties }).
    QHash<QString, QVariantList> m_keyframes;
    // Registered loadCss targets. Identity is re-read off each object at apply time, so
    // a target's cssClass change is picked up on the next apply (reload or loadCss(this)).
    QList<QPointer<QObject>> m_bindings;
    bool m_loaded = false;
    QStringList m_requestedLayer;  // the paths last passed to loadLayered (for reloads)
    QString m_stylePrelude;        // config-derived CSS prepended (lowest priority) on load
    QString m_generatedCss;        // build-generated CSS appended (highest priority) on load
    QByteArray m_contentHash;
    QFileSystemWatcher *m_watcher = nullptr;
    QNetworkAccessManager *m_nam = nullptr; // lazily created for @font-face downloads
    QSet<QString> m_fontFacesSeen;          // font URLs already loaded/queued (dedupe reloads)
    int m_fontRevision = 0;                 // ++ on each newly-registered downloaded font
};
