#include "qmlcss/csshr.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"

#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>

#include <algorithm>

namespace {

// Mirror of csstheme.cpp's identity coercion, used only for the hasCssIdentity check.
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

CssHr::CssHr(QQuickItem *parent)
    : QQuickItem(parent)
{
    // Faithful to the QML: implicitWidth 0, implicitHeight max(1, line.width) — line defaults
    // to width 1 (borderSide of an empty style), so implicitHeight starts at 1.
    setImplicitWidth(0);
    setImplicitHeight(1);
}

bool CssHr::hasCssIdentity() const
{
    if (!m_cssId.isEmpty() || !m_cssPart.isEmpty() || !m_cssPrimitive.isEmpty())
        return true;
    // Array.isArray(cssClass) ? length>0 : String(cssClass).length>0
    return !toStringList(m_cssClass).isEmpty();
}

void CssHr::setCssId(const QString &v)
{
    if (m_cssId == v)
        return;
    m_cssId = v;
    emit cssIdChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss(); // QML: onCssIdChanged -> cssTheme.loadCss(root)
}

void CssHr::setCssAlternateId(const QVariant &v)
{
    if (m_cssAlternateId == v)
        return;
    m_cssAlternateId = v;
    emit cssAlternateIdChanged();
}

void CssHr::setCssClass(const QVariant &v)
{
    if (m_cssClass == v)
        return;
    m_cssClass = v;
    emit cssClassChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss(); // QML: onCssClassChanged -> cssTheme.loadCss(root)
}

void CssHr::setCssState(const QVariant &v)
{
    if (m_cssState == v)
        return;
    m_cssState = v;
    emit cssStateChanged();
    maybeLoadCss(); // QML: onCssStateChanged -> cssTheme.loadCss(root)
}

void CssHr::setCssPrimitive(const QString &v)
{
    if (m_cssPrimitive == v)
        return;
    m_cssPrimitive = v;
    emit cssPrimitiveChanged();
    emit hasCssIdentityChanged();
}

void CssHr::setCssPart(const QString &v)
{
    if (m_cssPart == v)
        return;
    m_cssPart = v;
    emit cssPartChanged();
    emit hasCssIdentityChanged();
}

void CssHr::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();

    // QML: visible: !(style && style["display"] === "none")
    setVisible(m_style.value(QStringLiteral("display")).toString() != QLatin1String("none"));

    // QML: line is a binding on style — recompute + reapply to the Rectangle, and refresh
    // implicitHeight (which itself notifies the parent layout).
    recomputeLine();

    // QML: onStyleChanged -> cssLayout.notifyParentLayout(root)
    if (m_layout)
        m_layout->notifyParentLayout(this);
}

void CssHr::recomputeLine()
{
    // QML: readonly property var line: cssTheme.borderSide(style || ({}), "top", 1, "#ededed")
    QVariantMap next;
    if (m_theme) {
        next = m_theme->borderSide(m_style, QStringLiteral("top"), 1, QColor(QStringLiteral("#ededed")));
    } else {
        // Same result borderSide would give for an empty style, so behaviour is identical
        // before cssTheme is resolved in componentComplete.
        next.insert(QStringLiteral("width"), 1.0);
        next.insert(QStringLiteral("color"), QColor(QStringLiteral("#ededed")));
        next.insert(QStringLiteral("style"), QStringLiteral("solid"));
        next.insert(QStringLiteral("visible"), true);
    }

    const bool changed = next != m_line;
    m_line = next;

    const qreal lineWidth = m_line.value(QStringLiteral("width")).toReal();
    const qreal h = std::max<qreal>(1, lineWidth);
    // QML: implicitHeight: Math.max(1, root.line.width)
    setImplicitHeight(h);

    if (m_shape) {
        // QML Shape: fillColor = line.color; visible = line.visible; height = max(1, line.width).
        // The ShapePath's PathRectangle is driven through the aliases exposed on the Shape.
        m_shape->setProperty("fillColor", m_line.value(QStringLiteral("color")).value<QColor>());
        m_shape->setProperty("visible", m_line.value(QStringLiteral("visible")).toBool());
        m_shape->setHeight(h);
        m_shape->setProperty("rectHeight", h);
    }

    if (changed)
        emit lineChanged();
}

void CssHr::layoutChild()
{
    // C++ equivalent of the QML anchors { left; right; top } (full width, pinned to top).
    if (!m_shape)
        return;
    m_shape->setX(0);
    m_shape->setY(0);
    m_shape->setWidth(width());
    // Keep the composed PathRectangle full-width too (the ShapePath fill geometry).
    m_shape->setProperty("rectWidth", width());
}

void CssHr::maybeLoadCss()
{
    // QML: if (cssTheme && root.hasCssIdentity) cssTheme.loadCss(root)
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
}

void CssHr::componentComplete()
{
    QQuickItem::componentComplete();

    // Resolve the engine singletons the QML found as context properties `cssTheme`/`cssLayout`.
    if (QQmlContext *ctx = qmlContext(this)) {
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    }

    // Create the REAL QtQuick Shape via the Qt type-system (header-free) and parent it to us.
    // This is the composition mandate: we do not paint — the Shape (ShapePath + PathRectangle)
    // does. Rectangle is deliberately avoided (owner's rule: it is capped for several effects).
    if (QQmlEngine *eng = qmlEngine(this)) {
        QQmlComponent comp(eng);
        comp.setData(
            "import QtQuick\n"
            "import QtQuick.Shapes\n"
            "Shape {\n"
            "    property alias fillColor: sp.fillColor\n"
            "    property alias rectWidth: pr.width\n"
            "    property alias rectHeight: pr.height\n"
            "    ShapePath {\n"
            "        id: sp\n"
            "        strokeWidth: 0\n"
            "        strokeColor: \"transparent\"\n"
            "        fillColor: \"#ededed\"\n"
            "        PathRectangle { id: pr; x: 0; y: 0; width: 1; height: 1 }\n"
            "    }\n"
            "}\n",
            QUrl());
        if (QObject *o = comp.create(qmlContext(this))) {
            if (QQuickItem *shape = qobject_cast<QQuickItem *>(o)) {
                shape->setParentItem(this);
                m_shape = shape;
            } else {
                o->deleteLater();
            }
        }
    }

    // Push the current line into the freshly-created Shape and size the child.
    recomputeLine();
    layoutChild();

    // React to implicitHeight / visible changes exactly like the QML on*Changed handlers.
    connect(this, &QQuickItem::implicitHeightChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });

    // QML Component.onCompleted.
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this); // sets `style` -> setStyle recomputes line + notifies layout
    if (m_layout)
        m_layout->notifyParentLayout(this);
}

void CssHr::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    // Keep the child full-width whenever our width changes (anchors.right equivalent).
    layoutChild();
}
