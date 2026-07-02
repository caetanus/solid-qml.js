#include "qmlcss/contrast.h"
#include "qmlcss/csstheme.h"

#include <QColor>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>

// ---------------------------------------------------------------------------
// Internal helpers — mirror the private JS functions in Contrast.js exactly.
// None of these cross the public API; rgbToHsl/hslToColor use an opaque
// internal struct because the JS {h,s,l,a} object never appears in the API.
// ---------------------------------------------------------------------------

namespace {

// Mirrors JS `function linearChannel(v)`:
//   v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
static double linearChannel(double v)
{
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// Internal HSL colour — mirrors the JS {h, s, l, a} object returned by rgbToHsl.
struct HslColor { double h, s, l, a; };

// Mirrors JS `function hueToRgb(p, q, t)`:
static double hueToRgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6.0) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6;
    return p;
}

// Mirrors JS `function rgbToHsl(c)` — same arithmetic, not Qt's QColor::toHsl()
// (Qt's formula/rounding differs; fidelity to the JS output is required).
static HslColor rgbToHsl(const QColor &c)
{
    double r = c.redF(), g = c.greenF(), b = c.blueF(), a = c.alphaF();
    double mx = std::max({r, g, b});
    double mn = std::min({r, g, b});
    double h = 0, s = 0, l = (mx + mn) / 2.0;
    double d = mx - mn;

    if (d != 0) {
        s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r)
            h = ((g - b) / d + (g < b ? 6 : 0)) / 6.0;
        else if (mx == g)
            h = ((b - r) / d + 2) / 6.0;
        else
            h = ((r - g) / d + 4) / 6.0;
    }

    return { h, s, l, a };
}

// Mirrors JS `function hslToColor(hsl)` — same arithmetic, returns Qt.rgba equivalent.
static QColor hslToColor(const HslColor &hsl)
{
    double r, g, b;
    if (hsl.s == 0) {
        r = g = b = hsl.l;
    } else {
        double q = hsl.l < 0.5 ? hsl.l * (1 + hsl.s) : hsl.l + hsl.s - hsl.l * hsl.s;
        double p = 2 * hsl.l - q;
        r = hueToRgb(p, q, hsl.h + 1.0 / 3.0);
        g = hueToRgb(p, q, hsl.h);
        b = hueToRgb(p, q, hsl.h - 1.0 / 3.0);
    }
    return QColor::fromRgbF(r, g, b, hsl.a);
}

// Mirrors JS `function toHex2(v)`:
//   Math.max(0, Math.min(255, Math.round(v * 255))).toString(16), zero-padded to 2 chars.
static QString toHex2(double v)
{
    int n = static_cast<int>(std::round(std::max(0.0, std::min(255.0, v * 255.0))));
    QString s = QString::number(n, 16);
    return s.length() == 1 ? QStringLiteral("0") + s : s;
}

// Convert a QVariant (string or QColor) to QColor — used by barBackground/effectiveBackground
// where the JS `fallback` is typed as "usually a '#AARRGGBB' string".
static QColor variantToColor(const QVariant &v)
{
    if (v.canConvert<QColor>())
        return v.value<QColor>();
    return QColor(v.toString());
}

} // namespace

// ---------------------------------------------------------------------------
// Contrast implementation
// ---------------------------------------------------------------------------

Contrast::Contrast(QObject *parent)
    : QObject(parent)
{
}

// Mirrors JS `function luminance(c)`:
//   0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
double Contrast::luminance(const QColor &c) const
{
    return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF();
}

// Mirrors JS `function relativeLuminance(c)`:
//   0.2126 * linearChannel(c.r) + 0.7152 * linearChannel(c.g) + 0.0722 * linearChannel(c.b)
double Contrast::relativeLuminance(const QColor &c) const
{
    return 0.2126 * linearChannel(c.redF())
         + 0.7152 * linearChannel(c.greenF())
         + 0.0722 * linearChannel(c.blueF());
}

// Mirrors JS `function contrastRatio(a, b)`:
//   (max(la, lb) + 0.05) / (min(la, lb) + 0.05)
double Contrast::contrastRatio(const QColor &a, const QColor &b) const
{
    double la = relativeLuminance(a);
    double lb = relativeLuminance(b);
    double lighter = std::max(la, lb);
    double darker  = std::min(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

// Mirrors JS `function blendOver(fg, bg)`:
//   Qt.rgba(fg.r*a + bg.r*(1-a), fg.g*a + bg.g*(1-a), fg.b*a + bg.b*(1-a), 1)
QColor Contrast::blendOver(const QColor &fg, const QColor &bg) const
{
    double a = fg.alphaF();
    return QColor::fromRgbF(
        fg.redF()   * a + bg.redF()   * (1 - a),
        fg.greenF() * a + bg.greenF() * (1 - a),
        fg.blueF()  * a + bg.blueF()  * (1 - a),
        1.0);
}

// Mirrors JS `function toHex(c)`:
//   "#" + toHex2(c.r) + toHex2(c.g) + toHex2(c.b)
QString Contrast::toHex(const QColor &c) const
{
    return QStringLiteral("#") + toHex2(c.redF()) + toHex2(c.greenF()) + toHex2(c.blueF());
}

// Mirrors JS `function bestBlackOrWhite(bg)`:
//   black = Qt.rgba(0.10, 0.10, 0.12, 1)
//   white = Qt.rgba(0.96, 0.97, 1.00, 1)
//   contrastRatio(white, bg) >= contrastRatio(black, bg) ? white : black
QColor Contrast::bestBlackOrWhite(const QColor &bg) const
{
    const QColor black = QColor::fromRgbF(0.10, 0.10, 0.12, 1.0);
    const QColor white = QColor::fromRgbF(0.96, 0.97, 1.00, 1.0);
    return contrastRatio(white, bg) >= contrastRatio(black, bg) ? white : black;
}

// Mirrors JS `function naturalContrastColor(fg, bg, minimumRatio)` exactly.
// Default minimumRatio = 4.5 mirrors `minimumRatio !== undefined ? minimumRatio : 4.5`.
QColor Contrast::naturalContrastColor(const QColor &fg, const QColor &bg, double minimumRatio) const
{
    if (contrastRatio(fg, bg) >= minimumRatio)
        return fg;

    const HslColor f = rgbToHsl(fg);
    const HslColor b = rgbToHsl(bg);

    std::array<double, 5> candidates;
    if (b.l < 0.5) {
        candidates = {
            std::max(f.l, std::min(0.96, b.l + 0.45)),
            0.72, 0.82, 0.92, 0.97
        };
    } else {
        candidates = {
            std::min(f.l, std::max(0.08, b.l - 0.45)),
            0.34, 0.24, 0.14, 0.08
        };
    }

    QColor best = fg;
    double bestRatio = contrastRatio(fg, bg);
    for (double lCandidate : candidates) {
        const HslColor candidateHsl = { f.h, f.s, std::max(0.0, std::min(1.0, lCandidate)), f.a };
        const QColor adjusted = hslToColor(candidateHsl);
        const double ratio = contrastRatio(adjusted, bg);
        if (ratio > bestRatio) {
            best = adjusted;
            bestRatio = ratio;
        }
        if (ratio >= minimumRatio)
            return adjusted;
    }

    const QColor fallbackColor = bestBlackOrWhite(bg);
    return contrastRatio(fallbackColor, bg) > bestRatio ? fallbackColor : best;
}

// Mirrors JS `function averageGradientColor(gradient, fallback)`:
//   Average r/g/b/a across all stops; return fallback when stops absent/empty.
QColor Contrast::averageGradientColor(const QVariantMap &gradient, const QColor &fallback) const
{
    if (gradient.isEmpty())
        return fallback;
    const QVariantList stops = gradient.value(QStringLiteral("stops")).toList();
    if (stops.isEmpty())
        return fallback;

    double r = 0, g = 0, b = 0, a = 0;
    for (const QVariant &sv : stops) {
        const QVariantMap stop = sv.toMap();
        const QColor c = stop.value(QStringLiteral("color")).value<QColor>();
        r += c.redF(); g += c.greenF(); b += c.blueF(); a += c.alphaF();
    }
    const int count = stops.size();
    return QColor::fromRgbF(r / count, g / count, b / count, a / count);
}

// Mirrors JS `function styleBackgroundColor(style, cssTheme, fallback)`.
QColor Contrast::styleBackgroundColor(const QVariantMap &style, CssTheme *cssTheme,
                                      const QColor &fallback) const
{
    if (style.isEmpty() || !cssTheme || !cssTheme->isLoaded())
        return fallback;

    const QString bgColor = style.value(QStringLiteral("background-color")).toString();
    if (!bgColor.isEmpty())
        return cssTheme->parseColor(bgColor);

    const QString bgValue = !style.value(QStringLiteral("background")).toString().isEmpty()
        ? style.value(QStringLiteral("background")).toString()
        : style.value(QStringLiteral("background-image")).toString();

    if (bgValue.isEmpty())
        return fallback;
    if (!bgValue.contains(QStringLiteral("gradient")))
        return cssTheme->parseColor(bgValue);

    return averageGradientColor(cssTheme->parseGradient(bgValue), fallback);
}

// Mirrors JS `function barBackground(cssTheme, fallback)`:
//   window#waybar background-color, falling back when the CSS theme has none loaded.
//   `fallback` is usually a "#AARRGGBB" string from the host app's theme map.
//   context property — parse it so .r/.g/.b/.a are usable by blendOver below.
QColor Contrast::barBackground(CssTheme *cssTheme, const QVariant &fallback) const
{
    if (!cssTheme || !cssTheme->isLoaded()) {
        // JS: cssTheme ? cssTheme.parseColor(fallback) : fallback
        if (cssTheme)
            return cssTheme->parseColor(fallback.toString());
        return variantToColor(fallback);
    }
    const QVariantMap style = cssTheme->resolve(QStringLiteral("waybar"));
    const QString bgColor = style.value(QStringLiteral("background-color")).toString();
    return bgColor.isEmpty()
        ? cssTheme->parseColor(fallback.toString())
        : cssTheme->parseColor(bgColor);
}

// Mirrors JS `function effectiveBackground(ownBg, cssTheme, fallback)`:
//   ownBg.a > 0 ? blendOver(ownBg, bar) : bar
QColor Contrast::effectiveBackground(const QColor &ownBg, CssTheme *cssTheme,
                                     const QVariant &fallback) const
{
    const QColor bar = barBackground(cssTheme, fallback);
    return ownBg.alphaF() > 0 ? blendOver(ownBg, bar) : bar;
}

// Mirrors JS `function needsDarkIcon(bg)`:
//   return luminance(bg) >= 0.5
bool Contrast::needsDarkIcon(const QColor &bg) const
{
    return luminance(bg) >= 0.5;
}

// Mirrors JS `function contrastColor(bg)`:
//   !needsDarkIcon → "#ffffff"
//   else → toHex(blendOver(Qt.rgba(26/255, 26/255, 26/255, 0.75), bg))
QString Contrast::contrastColor(const QColor &bg) const
{
    if (!needsDarkIcon(bg))
        return QStringLiteral("#ffffff");
    return toHex(blendOver(QColor::fromRgbF(26.0 / 255.0, 26.0 / 255.0, 26.0 / 255.0, 0.75), bg));
}

// Mirrors JS `function contrastFill(bg, alpha)`:
//   luminance(bg) < 0.5
//     ? "rgba(255, 255, 255, " + alpha + ")"
//     : "rgba(26, 26, 26, " + alpha + ")"
// alpha is formatted as-is (no forced decimals), matching JS's number-to-string coercion.
QString Contrast::contrastFill(const QColor &bg, double alpha) const
{
    const QString a = QString::number(alpha);
    return luminance(bg) < 0.5
        ? QStringLiteral("rgba(255, 255, 255, ") + a + QStringLiteral(")")
        : QStringLiteral("rgba(26, 26, 26, ")    + a + QStringLiteral(")");
}
