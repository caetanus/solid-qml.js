#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

// Contrast utilities, translated 1:1 from qml/qmlcss/Contrast.js (.pragma library).
// Registered as a QML singleton `Contrast` in the `qmlcss` module.
//
// Original JS comments preserved below:
//
// naturalContrastColor(fg, bg, minimumRatio):
//   Preserve the foreground's hue/saturation when possible, but move its HSL lightness away
//   from the background. If that still cannot clear the target contrast, fall back to softened
//   black or white.
//
// contrastColor(bg):
//   High-contrast foreground (icon/text) color for the given background. Pure black reads as
//   too harsh on light themes, so it's softened to a charcoal gray by blending at reduced
//   opacity over the background. Themes that want full #1a1a1a/#ffffff can set "color"
//   explicitly in their CSS, which takes precedence over this fallback at every call site.
//   Charcoal: Qt.rgba(26/255, 26/255, 26/255, 0.75) blended over bg.
//
// barBackground(cssTheme, fallback):
//   window#waybar background-color, falling back when the CSS theme has none loaded.
//   `fallback` is usually a "#AARRGGBB" string from the host app's theme map.
//   context property — parse it so .r/.g/.b/.a are usable by blendOver below.
//
// effectiveBackground(ownBg, cssTheme, fallback):
//   What an element actually sits on: its own background if opaque/translucent, otherwise
//   the bar's background (blended through if translucent).
//
// needsDarkIcon(bg):
//   Whether icons/text on this background should use the dark contrast color (as opposed to
//   white).

class CssTheme;

class Contrast : public QObject {
    Q_OBJECT

public:
    explicit Contrast(QObject *parent = nullptr);

    // Direct luminance (perceptual, NOT gamma-corrected): 0.2126·r + 0.7152·g + 0.0722·b
    Q_INVOKABLE double luminance(const QColor &c) const;

    // Relative luminance per WCAG 2.x (gamma-corrected via linearChannel).
    Q_INVOKABLE double relativeLuminance(const QColor &c) const;

    // WCAG contrast ratio: (lighter + 0.05) / (darker + 0.05). Range 1..21.
    Q_INVOKABLE double contrastRatio(const QColor &a, const QColor &b) const;

    // Alpha-composite fg over bg (Porter-Duff "over"), result is opaque (alpha = 1).
    Q_INVOKABLE QColor blendOver(const QColor &fg, const QColor &bg) const;

    // "#rrggbb" hex string. Each channel: round(v * 255), clamped, zero-padded to 2 digits.
    Q_INVOKABLE QString toHex(const QColor &c) const;

    // Softened black (0.10, 0.10, 0.12, 1) or off-white (0.96, 0.97, 1.00, 1) —
    // whichever achieves a higher contrast ratio against bg.
    Q_INVOKABLE QColor bestBlackOrWhite(const QColor &bg) const;

    // Preserve fg hue/saturation; adjust HSL lightness to clear minimumRatio against bg.
    // Falls back to bestBlackOrWhite when no lightness adjustment suffices.
    // Default minimumRatio matches the JS `minimumRatio !== undefined ? minimumRatio : 4.5`.
    Q_INVOKABLE QColor naturalContrastColor(const QColor &fg, const QColor &bg,
                                            double minimumRatio = 4.5) const;

    // Average the RGB channels of all gradient stops. Returns fallback for empty/missing stops.
    // `gradient` is a QVariantMap as returned by CssTheme::parseGradient:
    //   { "stops": [ { "position": qreal, "color": QColor }, ... ] }
    Q_INVOKABLE QColor averageGradientColor(const QVariantMap &gradient,
                                            const QColor &fallback) const;

    // Background colour from a CSS style map, parsed through cssTheme.
    // Returns fallback when style/cssTheme is absent or cssTheme is not loaded.
    Q_INVOKABLE QColor styleBackgroundColor(const QVariantMap &style, CssTheme *cssTheme,
                                            const QColor &fallback) const;

    // window#waybar background-color, falling back when the CSS theme has none loaded.
    // `fallback` may be a QColor or a CSS color string (e.g. "#AARRGGBB").
    Q_INVOKABLE QColor barBackground(CssTheme *cssTheme, const QVariant &fallback) const;

    // Effective background an element sits on: its own bg (blended) if ownBg.alpha > 0,
    // otherwise the bar background.
    Q_INVOKABLE QColor effectiveBackground(const QColor &ownBg, CssTheme *cssTheme,
                                           const QVariant &fallback) const;

    // True when the perceptual luminance of bg >= 0.5 (i.e. bg is "light").
    Q_INVOKABLE bool needsDarkIcon(const QColor &bg) const;

    // "#ffffff" or softened-charcoal hex, depending on needsDarkIcon(bg).
    Q_INVOKABLE QString contrastColor(const QColor &bg) const;

    // "rgba(r, g, b, alpha)" string for canvas fills; alpha is formatted as-is (no zero-pad).
    Q_INVOKABLE QString contrastFill(const QColor &bg, double alpha) const;
};
