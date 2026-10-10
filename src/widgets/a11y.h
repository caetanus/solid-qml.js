#pragma once

class QQuickItem;

// Accessibility helpers for the CSS primitives.
//
// Interactive widgets get their accessibility for free from the QtQuick.Templates control they
// wrap (a Button IS a T.Button, a CheckBox IS a T.CheckBox, …). The CSS primitives — text runs,
// headings and images — wrap no control, so they must declare their own semantics or they are
// invisible to screen readers: Heading for h1…h6 (read off the same `cssPrimitive` the cascade
// styles by), StaticText otherwise, named by the rendered text; Graphic for <img>, named by alt
// (empty alt = decorative, AT skips it).
//
// LAZY: an element pays one bool (QQuickItemPrivate::isAccessible — what keeps it in Qt Quick's
// accessible tree) and nothing else. No Accessible attached object, no connection: role and name
// are produced by our QAccessible factory the first time an AT client queries the item, and read
// live from the item from then on.
//
// Generic containers (<div>) deliberately get NO role: a role on every box would flood the
// accessibility tree with meaningless "grouping" nodes; AT walks through them to the content.
//
// Declared here and DEFINED in a11y.cpp on purpose: the implementation needs QtQuick's private
// headers, which must not leak into a header the generated AOT code includes.
namespace SolidWidgets::A11y {

// Keep `item` in the accessible tree at the cost of one bool: no attached object, no connection.
void markAccessible(QQuickItem *item);

// Tell a live AT client that `item`'s accessible name changed (no-op while no AT is active).
void notifyNameChanged(QQuickItem *item);

// Install the factory that builds the primitives' interfaces on first query. Idempotent; called
// from SolidWidgets::registerTypes().
void installAccessibleFactory();

} // namespace SolidWidgets::A11y
