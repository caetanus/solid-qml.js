// sqruntime.h — the tiny JS-semantics runtime for AOT-emitted C++ (M-AOT-0+).
//
// This is NOT a JavaScript interpreter. It is a header of thin, per-expression inlines that let the
// C++ emitter render JS operators (`+`, `String()`, truthiness) with the coercions the QML/V4 path
// would apply, so `"" + count` and `count + 1` mean the same thing on both back-ends. State is held
// as QVariant (decision #1: QVariant-first, faithful to JS; typed Q_PROPERTY is a later measured
// optimization), so these operate on QVariant. Zero-overhead rule respected: the hot path stays the
// C++ CSS layout engine — these are leaf coercions on individual bindings, not a dispatch loop.
#pragma once

#include <QString>
#include <QVariant>
#include <cmath>

namespace sq {

// True for a QVariant whose JS type is String (so `+` concatenates rather than adds).
inline bool isString(const QVariant &v)
{
    return v.typeId() == QMetaType::QString || v.typeId() == QMetaType::QChar;
}

// JS `String(v)` — the coercion `"" + v` performs. Numbers print without a trailing ".0" for
// integral values (ECMAScript Number→String), booleans as "true"/"false", null/undefined faithfully.
inline QString str(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::UnknownType:
        return QStringLiteral("undefined");
    case QMetaType::Nullptr:
        return QStringLiteral("null");
    case QMetaType::Bool:
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QMetaType::QString:
    case QMetaType::QChar:
        return v.toString();
    default:
        break;
    }
    if (v.canConvert<double>()) {
        const double d = v.toDouble();
        if (std::isnan(d))
            return QStringLiteral("NaN");
        if (std::isinf(d))
            return d < 0 ? QStringLiteral("-Infinity") : QStringLiteral("Infinity");
        // Integral doubles print as integers ("1", not "1.0"); otherwise the shortest round-trip.
        if (d == std::floor(d) && std::abs(d) < 1e15)
            return QString::number(static_cast<long long>(d));
        return QString::number(d, 'g', 15);
    }
    return v.toString();
}

// JS `Number(v)` — numeric coercion. Non-numeric strings/objects become NaN like JS.
inline double num(const QVariant &v)
{
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? 1.0 : 0.0;
    if (v.typeId() == QMetaType::UnknownType)
        return std::nan("");
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? d : std::nan("");
}

// JS `+` operator: string concatenation if either side is a string, else numeric addition.
inline QVariant add(const QVariant &a, const QVariant &b)
{
    if (isString(a) || isString(b))
        return str(a) + str(b);
    return num(a) + num(b);
}

inline QVariant sub(const QVariant &a, const QVariant &b) { return num(a) - num(b); }
inline QVariant mul(const QVariant &a, const QVariant &b) { return num(a) * num(b); }
inline QVariant div(const QVariant &a, const QVariant &b) { return num(a) / num(b); }

// JS truthiness (for <Show>/conditional bindings): 0, NaN, "", null, undefined, false are falsy.
inline bool truthy(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::UnknownType:
    case QMetaType::Nullptr:
        return false;
    case QMetaType::Bool:
        return v.toBool();
    case QMetaType::QString:
        return !v.toString().isEmpty();
    default:
        break;
    }
    if (v.canConvert<double>()) {
        const double d = v.toDouble();
        return d != 0.0 && !std::isnan(d);
    }
    return v.isValid();
}

} // namespace sq
