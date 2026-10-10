#include "a11y.h"

#include "primitives.h"

#include <QAccessible>
#include <QtQuick/private/qaccessiblequickitem_p.h>
#include <QtQuick/private/qquickaccessibleattached_p.h>
#include <QtQuick/private/qquickitem_p.h>

namespace SolidWidgets::A11y {

namespace {

// The interface of a Text/Image primitive. Built only when an AT client asks for it (QAccessible
// caches it per object afterwards), so only then do we pay for change notifications.
class PrimitiveAccessible : public QAccessibleQuickItem {
public:
    explicit PrimitiveAccessible(QQuickItem *item)
        : QAccessibleQuickItem(item)
    {
        // Image needs no connection: Image::setAlt notifies directly (it owns the setter).
        if (auto *t = qobject_cast<Text *>(item))
            m_textConn = QObject::connect(t, &QmlCss::CssText::textChanged, t, [t] { notifyNameChanged(t); });
    }
    ~PrimitiveAccessible() override { QObject::disconnect(m_textConn); }

    QAccessible::Role role() const override
    {
        // An author's explicit `Accessible.role` wins (attached looked up, never created).
        if (auto *a = qobject_cast<QQuickAccessibleAttached *>(QQuickAccessibleAttached::attachedProperties(item()));
            a && a->role() != QAccessible::NoRole)
            return a->role();
        if (auto *t = qobject_cast<Text *>(item())) {
            // h1…h6 are headings (AT builds a document outline from them); everything else is static text.
            const QString p = t->cssPrimitive();
            const bool heading = p.size() == 2 && p.at(0) == QLatin1Char('h')
                && p.at(1) >= QLatin1Char('1') && p.at(1) <= QLatin1Char('6');
            return heading ? QAccessible::Heading : QAccessible::StaticText;
        }
        return QAccessible::Graphic; // Image
    }

    QString text(QAccessible::Text t) const override
    {
        if (t != QAccessible::Name)
            return QAccessibleQuickItem::text(t);
        // An author's explicit `Accessible.name` wins (attached looked up, never created).
        const QVariant explicitName = QQuickAccessibleAttached::property(object(), "name");
        if (!explicitName.isNull())
            return explicitName.toString();
        if (auto *x = qobject_cast<Text *>(item()))
            return x->text();
        if (auto *img = qobject_cast<Image *>(item()))
            return img->alt();
        return {};
    }

private:
    QMetaObject::Connection m_textConn;
};

// QAccessible::queryAccessibleInterface walks the object's class names most-derived first, so this
// answers for our classes before Qt Quick's generic "QQuickItem" factory would.
QAccessibleInterface *factory(const QString &classname, QObject *object)
{
    if (classname == QLatin1String(Text::staticMetaObject.className())
        || classname == QLatin1String(Image::staticMetaObject.className())) {
        if (auto *item = qobject_cast<QQuickItem *>(object))
            return new PrimitiveAccessible(item);
    }
    return nullptr;
}

} // namespace

void markAccessible(QQuickItem *item)
{
    QQuickItemPrivate::get(item)->setAccessible();
    // What the Accessible attached ctor did for a late-created element while an AT is live.
    if (QAccessible::isActive()) {
        QAccessibleEvent ev(item, QAccessible::ObjectCreated);
        QAccessible::updateAccessibility(&ev);
    }
}

void notifyNameChanged(QQuickItem *item)
{
    if (QAccessible::isActive()) {
        QAccessibleEvent ev(item, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&ev);
    }
}

void installAccessibleFactory()
{
    static const bool installed = [] {
        QAccessible::installFactory(factory);
        return true;
    }();
    Q_UNUSED(installed);
}

} // namespace SolidWidgets::A11y
