#include "a11y.h"

#include <QAccessible>
#include <QtQuick/private/qquickaccessibleattached_p.h>

namespace SolidWidgets::A11y {

namespace {

void describe(QObject *item, QAccessible::Role role, const QString &name)
{
    // Must go through qmlAttachedPropertiesObject<>(obj, /*create=*/true): that is where Qt's own
    // QQuickAccessibleAttached::attachedProperties() looks the object up (with create=false). The
    // standalone qmlAttachedProperties() factory would build an instance nothing can retrieve.
    auto *acc = qobject_cast<QQuickAccessibleAttached *>(
        qmlAttachedPropertiesObject<QQuickAccessibleAttached>(item, /*create=*/true));
    if (!acc)
        return;
    acc->setRole(role);
    acc->setName(name);
}

} // namespace

void describeText(QObject *item, const QString &cssPrimitive, const QString &text)
{
    // h1…h6 are headings (AT builds a document outline from them); everything else is static text.
    const bool heading = cssPrimitive.size() == 2 && cssPrimitive.at(0) == QLatin1Char('h')
        && cssPrimitive.at(1) >= QLatin1Char('1') && cssPrimitive.at(1) <= QLatin1Char('6');
    describe(item, heading ? QAccessible::Heading : QAccessible::StaticText, text);
}

void describeImage(QObject *item, const QString &alt)
{
    describe(item, QAccessible::Graphic, alt);
}

} // namespace SolidWidgets::A11y
