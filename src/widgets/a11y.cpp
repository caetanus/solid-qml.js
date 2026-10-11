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

    // Qt's base returns an EMPTY State when no attached object exists — every lazy primitive would
    // look visible/onscreen to AT. With an attached object the base is right; otherwise mirror the
    // flags it computes after that check, the ones that apply to StaticText/Heading/Graphic.
    QAccessible::State state() const override
    {
        if (QQuickAccessibleAttached::attachedProperties(item()))
            return QAccessibleQuickItem::state();
        QAccessible::State s;
        const QQuickItem *it = item();
        const QRect view = viewRect();
        const QRect r = rect();
        if (view.isNull() || r.isNull() || !window() || !window()->isVisible() || !it->isVisible()
            || qFuzzyIsNull(it->opacity()))
            s.invisible = true;
        if (!view.intersects(r))
            s.offscreen = true;
        if (it->activeFocusOnTab() || role() == QAccessible::StaticText) // Qt's isTextRole()
            s.focusable = true;
        if (role() == QAccessible::StaticText)
            s.readOnly = true;
        if (it->hasActiveFocus())
            s.focused = true;
        if (!it->isEnabled()) {
            s.focusable = false;
            s.disabled = true;
        }
        return s;
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

// accname "name from content" (browser rule, see docs/keyboard-accessibility.md "Accessible name
// from content"): roles whose accessible name, absent an explicit Accessible.name/aria-label, is
// the concatenation of their descendant text. Deliberately NOT list/tablist/dialog/textbox/img —
// those either have their own naming contract (textbox: a <label>; img: alt) or would make the
// name a confusing jumble of every row's text (list/tablist/dialog).
bool roleAllowsNameFromContent(QAccessible::Role role)
{
    switch (role) {
    case QAccessible::Button:
    case QAccessible::Link:
    case QAccessible::MenuItem:
    case QAccessible::PageTab:
    case QAccessible::CheckBox:
    case QAccessible::RadioButton:
    case QAccessible::Switch:
    case QAccessible::ListItem:
    case QAccessible::Heading:
        return true;
    default:
        return false;
    }
}

// Depth-first, document-order walk of `item`'s descendants collecting the browser accname "text
// alternative": the rendered text of every CssText-derived item (Text, a widget's own label) plus
// the `alt` of every Image, joined and whitespace-collapsed. Deliberately a RUNTIME walk, not a
// transpile-time expression: it is correct for every descendant shape alike — a literal <text>, a
// reactive signal read, a <For>/<Show>/component subtree whose text is not known until it runs —
// with no special-casing, and it costs nothing until an AT client actually asks (QAccessible::
// queryAccessibleInterface → text(Name), never from a layout/paint pass).
QString nameFromContent(const QQuickItem *item)
{
    QStringList parts;
    const auto kids = item->childItems();
    for (QQuickItem *child : kids) {
        if (!child)
            continue;
        if (auto *txt = qobject_cast<QmlCss::CssText *>(child)) {
            const QString s = txt->text();
            if (!s.isEmpty())
                parts << s;
            continue; // a text run has no further accessible content of its own
        }
        if (auto *img = qobject_cast<Image *>(child)) {
            const QString a = img->alt();
            if (!a.isEmpty())
                parts << a;
            continue;
        }
        const QString sub = nameFromContent(child);
        if (!sub.isEmpty())
            parts << sub;
    }
    return parts.join(QLatin1Char(' '));
}

// Any accessible plain QQuickItem (a Div/Fieldset/… — no dedicated primitive class, so this is the
// generic one reached at the base "QQuickItem" classname). Behaves exactly like Qt Quick's own
// generic QAccessibleQuickItem (role()/state()/etc. are all inherited, unchanged) EXCEPT for the
// Name text: when there is no explicit name (no `Accessible.name`/`aria-label`) and the role allows
// "name from content", the name is computed lazily from descendant text/alt on every query —
// reactive by construction (there is nothing to invalidate), zero cost while unused.
//
// The role/explicit-name check deliberately happens HERE, in text() (called lazily, well after
// this interface exists), not in the factory function that constructs it: querying the attached
// Accessible.role from inside the FACTORY call itself (i.e. from within QAccessible::
// queryAccessibleInterface's own factory-selection loop) observed the role as not-yet-applied even
// though the exact same lookup — called from text()/role(), i.e. once the interface object is
// handed back to the caller — sees it correctly; so this class is built unconditionally for any
// accessible plain QQuickItem, and decides AT TEXT-QUERY TIME, matching the proven-working pattern
// PrimitiveAccessible above already uses for the Text/Image primitives.
class ContentNameAccessible : public QAccessibleQuickItem {
public:
    explicit ContentNameAccessible(QQuickItem *item)
        : QAccessibleQuickItem(item)
    {
    }

    QString text(QAccessible::Text t) const override
    {
        if (t != QAccessible::Name)
            return QAccessibleQuickItem::text(t);
        const QString explicitName = QAccessibleQuickItem::text(t);
        if (!explicitName.isEmpty())
            return explicitName;
        if (!roleAllowsNameFromContent(role()))
            return explicitName;
        return nameFromContent(item()).simplified();
    }
};

// QAccessible::queryAccessibleInterface walks the object's class names most-derived first, so this
// answers for our classes before Qt Quick's generic "QQuickItem" factory would.
QAccessibleInterface *factory(const QString &classname, QObject *object)
{
    if (classname == QLatin1String(Text::staticMetaObject.className())
        || classname == QLatin1String(Image::staticMetaObject.className())) {
        // Mirror Qt's own factory: an item taken out of the tree (Accessible.ignored) has no interface.
        if (auto *item = qobject_cast<QQuickItem *>(object); item && QQuickItemPrivate::get(item)->isAccessible)
            return new PrimitiveAccessible(item);
        return nullptr;
    }
    // Generic plain elements (<div role="button">, …): no dedicated primitive class, so this is
    // reached only at the base "QQuickItem" classname — AFTER any more specific factory (ours or a
    // Controls style's) already had its turn. Returning nullptr here falls through to Qt Quick's
    // own generic QQuickItem factory exactly as before this existed.
    if (classname == QLatin1String("QQuickItem")) {
        auto *item = qobject_cast<QQuickItem *>(object);
        if (!item || !QQuickItemPrivate::get(item)->isAccessible)
            return nullptr;
        return new ContentNameAccessible(item);
    }
    return nullptr;
}

} // namespace

void markAccessible(QQuickItem *item)
{
    // An author's Accessible attached object already enabled the item in its ctor, and an
    // `Accessible.ignored: true` binding (run before componentComplete) cleared it again — it stands.
    if (qmlAttachedPropertiesObject<QQuickAccessibleAttached>(item, /*create=*/false))
        return;
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
