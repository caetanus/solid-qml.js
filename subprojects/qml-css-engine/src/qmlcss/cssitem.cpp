#include "qmlcss/cssitem.h"

#include "qmlcss/csstheme.h"

#include <QFont>
#include <QJSValue>
#include <QQmlContext>
#include <QQmlEngine>

namespace {

// Mirror of csstheme.cpp's identity coercion, used only for hasCssIdentity().
QStringList toStringList(const QVariant &v)
{
    if (!v.isValid())
        return {};
    if (v.metaType().id() == qMetaTypeId<QJSValue>())
        return toStringList(v.value<QJSValue>().toVariant());
    if (v.metaType().id() == QMetaType::QStringList)
        return v.toStringList();
    if (v.metaType().id() == QMetaType::QVariantList) {
        QStringList out;
        const QVariantList list = v.toList();
        for (const QVariant &e : list) {
            const QString s = e.toString().trimmed();
            if (!s.isEmpty())
                out << s;
        }
        return out;
    }
    return v.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

} // namespace

CssItem::CssItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    // QML: visible: false; width: 0; height: 0
    setVisible(false);
    setWidth(0);
    setHeight(0);
}

bool CssItem::hasCssIdentity() const
{
    // QML: cssId.length > 0 || cssPart.length > 0
    //   || (Array.isArray(cssClass) ? cssClass.length > 0 : String(cssClass).length > 0)
    return !m_cssId.isEmpty() || !m_cssPart.isEmpty() || !toStringList(m_cssClass).isEmpty();
}

void CssItem::setCssId(const QString &v)
{
    if (m_cssId == v)
        return;
    m_cssId = v;
    emit cssIdChanged();
    maybeLoadCss(); // QML: onCssIdChanged -> if (cssTheme && hasCssIdentity()) cssTheme.loadCss(root)
}

void CssItem::setCssAlternateId(const QVariant &v)
{
    if (m_cssAlternateId == v)
        return;
    m_cssAlternateId = v;
    emit cssAlternateIdChanged();
}

void CssItem::setCssClass(const QVariant &v)
{
    if (m_cssClass == v)
        return;
    m_cssClass = v;
    emit cssClassChanged();
    maybeLoadCss(); // QML: onCssClassChanged -> if (cssTheme && hasCssIdentity()) cssTheme.loadCss(root)
}

void CssItem::setCssPrimitive(const QString &v)
{
    if (m_cssPrimitive == v)
        return;
    m_cssPrimitive = v;
    emit cssPrimitiveChanged();
}

void CssItem::setCssPart(const QString &v)
{
    if (m_cssPart == v)
        return;
    m_cssPart = v;
    emit cssPartChanged();
}

void CssItem::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();
    apply(); // QML: onStyleChanged: root._apply()
}

QString CssItem::inferPrimitive() const
{
    // QML: _inferPrimitive() — detect parent type by inspecting its available properties.
    QQuickItem *p = parentItem();
    if (!p)
        return QStringLiteral("item");
    const QMetaObject *mo = p->metaObject();
    // QML: if (p.text !== undefined && p.font !== undefined) return "text"
    if (mo->indexOfProperty("text") >= 0 && mo->indexOfProperty("font") >= 0)
        return QStringLiteral("text");
    // QML: if (p.radius !== undefined && p.color !== undefined) return "rect"
    if (mo->indexOfProperty("radius") >= 0 && mo->indexOfProperty("color") >= 0)
        return QStringLiteral("rect");
    return QStringLiteral("item");
}

void CssItem::apply()
{
    // QML: if (!p || !cssTheme) return
    if (!m_theme)
        return;
    QQuickItem *p = parentItem();
    if (!p)
        return;

    const QString prim = m_cssPrimitive.isEmpty() ? inferPrimitive() : m_cssPrimitive;
    const QMetaObject *mo = p->metaObject();

    if (prim == QLatin1String("text")) {
        // QML: if (s["color"] !== undefined && p.color !== undefined) p.color = parseColor(...)
        const QString colorVal = m_style.value(QStringLiteral("color")).toString();
        if (!colorVal.isEmpty() && mo->indexOfProperty("color") >= 0)
            p->setProperty("color", m_theme->parseColor(colorVal));

        // QML: font is a value type — read, mutate, write back (otherwise the write is lost).
        const QString fontFamily = m_style.value(QStringLiteral("font-family")).toString();
        const QString fontSize   = m_style.value(QStringLiteral("font-size")).toString();
        if (mo->indexOfProperty("font") >= 0 && (!fontFamily.isEmpty() || !fontSize.isEmpty())) {
            QFont f = p->property("font").value<QFont>();
            if (!fontFamily.isEmpty())
                f.setFamily(fontFamily);
            if (!fontSize.isEmpty())
                f.setPointSizeF(m_theme->parseFontSize(fontSize, f.pointSizeF()));
            p->setProperty("font", f);
        }
    } else if (prim == QLatin1String("rect")) {
        // QML: background-color → p.color
        const QString bgColor = m_style.value(QStringLiteral("background-color")).toString();
        if (!bgColor.isEmpty() && mo->indexOfProperty("color") >= 0)
            p->setProperty("color", m_theme->parseColor(bgColor));

        // QML: border-radius → p.radius (via parseLength)
        const QString borderRadius = m_style.value(QStringLiteral("border-radius")).toString();
        if (!borderRadius.isEmpty() && mo->indexOfProperty("radius") >= 0) {
            const qreal cur = p->property("radius").toReal();
            p->setProperty("radius", m_theme->parseLength(borderRadius, cur));
        }

        // QML: Rectangle.border is a QObject (QQuickPen); sub-props write directly.
        if (mo->indexOfProperty("border") >= 0) {
            QObject *border = p->property("border").value<QObject *>();
            if (border) {
                const QString borderColor = m_style.value(QStringLiteral("border-color")).toString();
                if (!borderColor.isEmpty())
                    border->setProperty("color", m_theme->parseColor(borderColor));
                const QString borderWidth = m_style.value(QStringLiteral("border-width")).toString();
                if (!borderWidth.isEmpty()) {
                    const qreal cur = border->property("width").toReal();
                    border->setProperty("width", m_theme->parseLength(borderWidth, cur));
                }
            }
        }
    }
    // "item" primitive: nothing to apply (no visual CSS props map to plain Item).
}

void CssItem::maybeLoadCss()
{
    // QML: if (cssTheme && hasCssIdentity()) cssTheme.loadCss(root)
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
}

void CssItem::componentComplete()
{
    QQuickItem::componentComplete();

    // QML: cssTheme is a context property (not a singleton).
    if (QQmlContext *ctx = qmlContext(this)) {
        m_theme = qobject_cast<CssTheme *>(
            ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
    }

    // QML Component.onCompleted: if (cssTheme && hasCssIdentity()) cssTheme.loadCss(root)
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
}
