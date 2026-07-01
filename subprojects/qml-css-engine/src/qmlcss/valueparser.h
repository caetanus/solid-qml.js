#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace CssValueParser {

QColor parseColor(const QString &cssColor);
QStringList splitTopLevel(const QString &text, QChar sep);
QStringList splitTopLevelWhitespace(const QString &text);
bool parseLengthPx(const QString &token, double *out);
int parseDuration(const QString &cssValue, int fallbackMs);
QEasingCurve::Type parseEasing(const QString &cssValue, QEasingCurve::Type fallback);

// Parse the standard CSS `transition` shorthand
// (`<property> <duration> <timing-function> <delay>`, first comma segment) into
// { property, duration (ms), easing (QEasingCurve::Type), delay (ms) }. The first
// <time> is the duration, the second is the delay (per the CSS spec).
QVariantMap parseTransition(const QString &cssValue);

// Parse the standard CSS `animation` shorthand
// (`<name> <duration> <timing-function> <iteration-count> <direction> <delay>`, first
// comma segment, order-independent) into { name, duration (ms), delay (ms),
// easing (QEasingCurve::Type), iterations (int; -1 = infinite), direction }.
QVariantMap parseAnimation(const QString &cssValue);

} // namespace CssValueParser
