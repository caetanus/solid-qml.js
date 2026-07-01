#include "valueparser.h"

#include <QtGlobal>

namespace CssValueParser {

QColor parseColor(const QString &colorStr)
{
    const QString s = colorStr.trimmed();
    if (s.compare(QLatin1String("transparent"), Qt::CaseInsensitive) == 0) {
        return QColor(0, 0, 0, 0);
    }
    if (s.startsWith(QLatin1String("#"))) {
        return QColor(s);
    }
    if (s.startsWith(QLatin1String("rgb("))) {
        const QString inner = s.mid(4, s.length() - 5);
        const QStringList parts = inner.split(QLatin1Char(','));
        if (parts.size() == 3) {
            return QColor(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(), parts[2].trimmed().toInt());
        }
    }
    if (s.startsWith(QLatin1String("rgba("))) {
        const QString inner = s.mid(5, s.length() - 6);
        const QStringList parts = inner.split(QLatin1Char(','));
        if (parts.size() == 4) {
            return QColor(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(), parts[2].trimmed().toInt(),
                          static_cast<int>(parts[3].trimmed().toFloat() * 255.0f));
        }
    }
    return QColor(s);
}

QStringList splitTopLevel(const QString &text, QChar sep)
{
    QStringList out;
    int depth = 0;
    int start = 0;
    for (int i = 0; i < text.length(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('(')) {
            ++depth;
        } else if (c == QLatin1Char(')')) {
            --depth;
        } else if (depth == 0 && c == sep) {
            out.append(text.mid(start, i - start));
            start = i + 1;
        }
    }
    out.append(text.mid(start));
    return out;
}

QStringList splitTopLevelWhitespace(const QString &text)
{
    QStringList out;
    int depth = 0;
    QString current;
    for (const QChar c : text) {
        if (c == QLatin1Char('(')) {
            ++depth;
            current.append(c);
        } else if (c == QLatin1Char(')')) {
            --depth;
            current.append(c);
        } else if (depth == 0 && c.isSpace()) {
            if (!current.isEmpty()) {
                out.append(current);
                current.clear();
            }
        } else {
            current.append(c);
        }
    }
    if (!current.isEmpty()) {
        out.append(current);
    }
    return out;
}

bool parseLengthPx(const QString &token, double *out)
{
    // Normalize CSS length units to canonical (device-independent) pixels. px and a bare
    // number are 1:1; pt → px at 96dpi (×96/72); em/rem → px against a 16px base (no font
    // context here — fonts resolve em/rem themselves). This is the single length chokepoint
    // every layout consumer (padding/margin/width/radius/border/box-shadow) goes through.
    QString t = token.trimmed().toLower();
    if (t.isEmpty()) {
        return false;
    }
    double factor = 1.0;
    if (t.endsWith(QLatin1String("px"))) {
        t.chop(2);
    } else if (t.endsWith(QLatin1String("pt"))) {
        t.chop(2);
        factor = 96.0 / 72.0;
    } else if (t.endsWith(QLatin1String("rem"))) {
        t.chop(3);
        factor = 16.0;
    } else if (t.endsWith(QLatin1String("em"))) {
        t.chop(2);
        factor = 16.0;
    }
    bool ok = false;
    const double v = t.trimmed().toDouble(&ok);
    if (ok && out != nullptr) {
        *out = v * factor;
    }
    return ok;
}

int parseDuration(const QString &cssValue, int fallbackMs)
{
    const QString s = cssValue.trimmed().toLower();
    if (s.isEmpty()) {
        return fallbackMs;
    }

    bool ok = false;
    const double value = s.endsWith(QStringLiteral("ms"))
        ? s.left(s.length() - 2).trimmed().toDouble(&ok)
        : (s.endsWith(QLatin1Char('s'))
            ? s.left(s.length() - 1).trimmed().toDouble(&ok) * 1000.0
            : s.toDouble(&ok));

    return ok ? qMax(0, qRound(value)) : fallbackMs;
}

QEasingCurve::Type parseEasing(const QString &cssValue, QEasingCurve::Type fallback)
{
    QString n = cssValue.trimmed().toLower();
    n.remove(QLatin1Char('-'));
    n.remove(QLatin1Char('_'));
    n.remove(QLatin1Char(' '));

    if (n.isEmpty()) return fallback;
    if (n == QStringLiteral("linear")) return QEasingCurve::Linear;
    if (n == QStringLiteral("ease") || n == QStringLiteral("easeinout") || n == QStringLiteral("inoutquad")) return QEasingCurve::InOutQuad;
    if (n == QStringLiteral("easein") || n == QStringLiteral("inquad")) return QEasingCurve::InQuad;
    if (n == QStringLiteral("easeout") || n == QStringLiteral("outquad")) return QEasingCurve::OutQuad;
    if (n == QStringLiteral("outinquad")) return QEasingCurve::OutInQuad;
    if (n == QStringLiteral("incubic")) return QEasingCurve::InCubic;
    if (n == QStringLiteral("outcubic")) return QEasingCurve::OutCubic;
    if (n == QStringLiteral("inoutcubic")) return QEasingCurve::InOutCubic;
    if (n == QStringLiteral("inquart")) return QEasingCurve::InQuart;
    if (n == QStringLiteral("outquart")) return QEasingCurve::OutQuart;
    if (n == QStringLiteral("inoutquart")) return QEasingCurve::InOutQuart;
    if (n == QStringLiteral("inquint")) return QEasingCurve::InQuint;
    if (n == QStringLiteral("outquint")) return QEasingCurve::OutQuint;
    if (n == QStringLiteral("inoutquint")) return QEasingCurve::InOutQuint;
    if (n == QStringLiteral("insine")) return QEasingCurve::InSine;
    if (n == QStringLiteral("outsine")) return QEasingCurve::OutSine;
    if (n == QStringLiteral("inoutsine")) return QEasingCurve::InOutSine;
    if (n == QStringLiteral("inexpo")) return QEasingCurve::InExpo;
    if (n == QStringLiteral("outexpo")) return QEasingCurve::OutExpo;
    if (n == QStringLiteral("inoutexpo")) return QEasingCurve::InOutExpo;
    if (n == QStringLiteral("incirc")) return QEasingCurve::InCirc;
    if (n == QStringLiteral("outcirc")) return QEasingCurve::OutCirc;
    if (n == QStringLiteral("inoutcirc")) return QEasingCurve::InOutCirc;
    if (n == QStringLiteral("inelastic")) return QEasingCurve::InElastic;
    if (n == QStringLiteral("outelastic")) return QEasingCurve::OutElastic;
    if (n == QStringLiteral("inoutelastic")) return QEasingCurve::InOutElastic;
    if (n == QStringLiteral("inback")) return QEasingCurve::InBack;
    if (n == QStringLiteral("outback")) return QEasingCurve::OutBack;
    if (n == QStringLiteral("inoutback")) return QEasingCurve::InOutBack;
    if (n == QStringLiteral("inbounce")) return QEasingCurve::InBounce;
    if (n == QStringLiteral("outbounce")) return QEasingCurve::OutBounce;
    if (n == QStringLiteral("inoutbounce")) return QEasingCurve::InOutBounce;
    return fallback;
}

QVariantMap parseTransition(const QString &cssValue)
{
    QVariantMap out;
    const QStringList segments = splitTopLevel(cssValue, QLatin1Char(','));
    const QString first = (segments.isEmpty() ? cssValue : segments.first()).trimmed();
    if (first.isEmpty())
        return out;

    QString property;
    int duration = 0;
    int delay = 0;
    int timesSeen = 0;
    QEasingCurve::Type easing = QEasingCurve::InOutQuad;

    const QStringList tokens = splitTopLevelWhitespace(first);
    for (const QString &tok : tokens) {
        const QString low = tok.toLower();

        // A <time> token ends in "ms"/"s" AND parses as a number (so a property
        // like "colors" or a keyword like "ease" is not mistaken for a time).
        const bool hasMs = low.endsWith(QStringLiteral("ms"));
        bool isTime = false;
        if (hasMs || low.endsWith(QLatin1Char('s'))) {
            bool ok = false;
            low.left(low.length() - (hasMs ? 2 : 1)).toDouble(&ok);
            isTime = ok;
        }
        if (isTime) {
            const int ms = parseDuration(low, 0);
            if (timesSeen == 0)
                duration = ms;
            else
                delay = ms;
            ++timesSeen;
            continue;
        }

        // A timing-function: a known easing keyword, or a cubic-bezier()/steps()
        // function. parseEasing returns the sentinel fallback for non-keywords, so
        // an unknown ident falls through to be treated as the property name.
        if (low.startsWith(QStringLiteral("cubic-bezier")) || low.startsWith(QStringLiteral("steps"))) {
            easing = parseEasing(low, QEasingCurve::InOutQuad);
            continue;
        }
        const QEasingCurve::Type probe = parseEasing(low, QEasingCurve::TCBSpline);
        if (probe != QEasingCurve::TCBSpline) {
            easing = probe;
            continue;
        }

        if (property.isEmpty())
            property = tok;
    }

    out.insert(QStringLiteral("property"), property.isEmpty() ? QStringLiteral("all") : property);
    out.insert(QStringLiteral("duration"), duration);
    out.insert(QStringLiteral("delay"), delay);
    out.insert(QStringLiteral("easing"), static_cast<int>(easing));
    return out;
}

QVariantMap parseAnimation(const QString &cssValue)
{
    QVariantMap out;
    const QStringList segments = splitTopLevel(cssValue, QLatin1Char(','));
    const QString first = (segments.isEmpty() ? cssValue : segments.first()).trimmed();
    if (first.isEmpty())
        return out;

    QString name;
    QString direction = QStringLiteral("normal");
    int duration = 0;
    int delay = 0;
    int timesSeen = 0;
    int iterations = 1;
    QEasingCurve::Type easing = QEasingCurve::InOutQuad;

    const QStringList tokens = splitTopLevelWhitespace(first);
    for (const QString &tok : tokens) {
        const QString low = tok.toLower();

        // <time> (duration first, then delay).
        const bool hasMs = low.endsWith(QStringLiteral("ms"));
        bool isTime = false;
        if (hasMs || low.endsWith(QLatin1Char('s'))) {
            bool ok = false;
            low.left(low.length() - (hasMs ? 2 : 1)).toDouble(&ok);
            isTime = ok;
        }
        if (isTime) {
            const int ms = parseDuration(low, 0);
            if (timesSeen == 0)
                duration = ms;
            else
                delay = ms;
            ++timesSeen;
            continue;
        }

        if (low == QLatin1String("infinite")) {
            iterations = -1;
            continue;
        }
        if (low == QLatin1String("normal") || low == QLatin1String("reverse")
            || low == QLatin1String("alternate") || low == QLatin1String("alternate-reverse")) {
            direction = low;
            continue;
        }

        // A bare number is the iteration count.
        bool numOk = false;
        const double n = low.toDouble(&numOk);
        if (numOk) {
            iterations = qMax(0, qRound(n));
            continue;
        }

        // A timing-function keyword / function.
        if (low.startsWith(QStringLiteral("cubic-bezier")) || low.startsWith(QStringLiteral("steps"))) {
            easing = parseEasing(low, QEasingCurve::InOutQuad);
            continue;
        }
        const QEasingCurve::Type probe = parseEasing(low, QEasingCurve::TCBSpline);
        if (probe != QEasingCurve::TCBSpline) {
            easing = probe;
            continue;
        }

        // Otherwise the @keyframes name (first wins).
        if (name.isEmpty())
            name = tok;
    }

    out.insert(QStringLiteral("name"), name);
    out.insert(QStringLiteral("duration"), duration);
    out.insert(QStringLiteral("delay"), delay);
    out.insert(QStringLiteral("easing"), static_cast<int>(easing));
    out.insert(QStringLiteral("iterations"), iterations);
    out.insert(QStringLiteral("direction"), direction);
    return out;
}

} // namespace CssValueParser
