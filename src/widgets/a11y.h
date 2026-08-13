#pragma once

#include <QString>

class QObject;

// Accessibility helpers for the CSS primitives.
//
// Interactive widgets get their accessibility for free from the QtQuick.Templates control they
// wrap (a Button IS a T.Button, a CheckBox IS a T.CheckBox, …). The CSS primitives — text runs,
// headings and images — wrap no control, so they must declare their own semantics or they are
// invisible to screen readers. That is what this does: a role + an accessible name, derived from
// the same `cssPrimitive` the cascade already uses for styling.
//
// Generic containers (<div>) deliberately get NO role: a role on every box would flood the
// accessibility tree with meaningless "grouping" nodes; AT walks through them to the content.
//
// Declared here and DEFINED in a11y.cpp on purpose: the implementation needs QtQuick's private
// QQuickAccessibleAttached, which must not leak into a header the generated AOT code includes.
namespace SolidWidgets::A11y {

// Mark an item as a text run — Heading for the h1…h6 primitives, StaticText otherwise — named by
// the text it renders.
void describeText(QObject *item, const QString &cssPrimitive, const QString &text);

// Mark an item as an image, named by its alt text (empty alt = decorative, AT skips it).
void describeImage(QObject *item, const QString &alt);

} // namespace SolidWidgets::A11y
