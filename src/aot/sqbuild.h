// sqbuild.h — construction helpers for AOT-emitted C++ scenes.
//
// The generated code builds the widget tree in C++ (no QML parsing), so it must reproduce, by hand,
// the three things the QML engine does for each element: (1) associate it with a QML context so it
// resolves `cssTheme`/`cssLayout` and can compose its own internal QML snippets (Button's label,
// etc.); (2) run the QQmlParserStatus lifecycle (classBegin before props, componentComplete after the
// subtree is assembled — the moment CssRect resolves style and lays out); (3) append children into
// the element's `content` default slot (NOT setParentItem — that bypasses the CSS content holder).
//
// componentComplete() is protected on QQuickItem but PUBLIC on the QQmlParserStatus interface it
// implements, so we reach it through an interface cast. Faithful lifecycle order (mirrors QML):
//   new → setContextForObject → classBegin → set props → build+append children → componentComplete
// Children are completed before their parent (bottom-up), so a container's layout pass sees each
// child's final implicit size.
#pragma once

#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlListProperty>
#include <QQmlParserStatus>
#include <QVariant>
#include <QVariantList>

namespace sq {

// Associate a C++-constructed object with the engine's context, then run classBegin(). Call right
// after `new`, before assigning any property.
inline void begin(QObject *item, QQmlContext *ctx)
{
    QQmlEngine::setContextForObject(item, ctx);
    if (auto *ps = qobject_cast<QQmlParserStatus *>(item))
        ps->classBegin();
}

// Run componentComplete() — the point where a CssRect/CssFill resolves its style and lays out its
// (already-appended, already-completed) children. Call last, after the whole subtree is assembled.
inline void complete(QObject *item)
{
    if (auto *ps = qobject_cast<QQmlParserStatus *>(item))
        ps->componentComplete();
}

// Append a child into a parent's `content` default slot (the CSS layout content holder). Works for
// any Css* type exposing `QQmlListProperty<QObject> content()` (CssRect/CssFill and their widgets).
template <class Parent>
inline void append(Parent *parent, QObject *child)
{
    auto slot = parent->content();
    slot.append(&slot, child);
}

// The `cssClass: ["a", "b"]` literal, as the QVariant(QVariantList) the setter expects.
inline QVariant classes(std::initializer_list<const char *> names)
{
    QVariantList list;
    for (const char *n : names)
        list.append(QString::fromUtf8(n));
    return QVariant::fromValue(list);
}

} // namespace sq
