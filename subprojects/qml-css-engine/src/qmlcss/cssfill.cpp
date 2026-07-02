#include "qmlcss/cssfill.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"

#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlListReference>
#include <QUrl>

// Equivalent QML this C++ composes (see cssfill.h for the annotated version):
//
//   import QtQuick
//   import qmlcss
//   Item {                                              // <- this CssFill
//       Shape { anchors.fill: parent; visible: bgIsImage; fillColor: solidColor }   // <- m_bgSolid
//       Image { anchors.fill: parent; visible: bgIsImage && source                  // <- m_image
//               source: bgImageSource; fillMode: fillModeFor(background-size)
//               horizontalAlignment: AlignHCenter; verticalAlignment: AlignVCenter
//               smooth: true; asynchronous: true; cache: true
//               opacity: background-image-opacity || 1 }
//       CssRect { anchors.fill: parent; cssPrimitive: ""; style: root.style          // <- m_rect
//                 radius: root.radius; defaultColor: bgIsImage ? "transparent" : root.defaultColor
//                 defaultBorderColor: root.defaultBorderColor
//                 defaultBorderWidth: root.defaultBorderWidth }
//       Item { id: contentHolder; anchors.fill: parent }                             // <- m_contentHolder
//   }

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

// QML: value && value.trim().toLowerCase().indexOf("url(") === 0
bool isCssUrl(const QString &value)
{
    return value.trimmed().toLower().startsWith(QLatin1String("url("));
}

// QML: imageSource(value) — strip url( … ), unquote, and normalise a bare absolute path to file://.
QString imageSource(const QString &value)
{
    if (!isCssUrl(value))
        return QString();
    QString s = value.trimmed();
    s = s.mid(4, s.length() - 5).trimmed();
    if (s.length() >= 2
        && ((s.front() == QLatin1Char('"') && s.back() == QLatin1Char('"'))
            || (s.front() == QLatin1Char('\'') && s.back() == QLatin1Char('\'')))) {
        s = s.mid(1, s.length() - 2);
    }
    if (s.isEmpty())
        return QString();
    if (s.startsWith(QLatin1String("qrc:/")) || s.startsWith(QLatin1String("file:/"))
        || s.startsWith(QLatin1String("http://")) || s.startsWith(QLatin1String("https://")))
        return s;
    if (s.startsWith(QLatin1Char('/')))
        return QStringLiteral("file://") + s;
    return s;
}

// QML: fillModeFor(size). Numeric values are QQuickImage::FillMode.
// Stretch=0, PreserveAspectFit=1, PreserveAspectCrop=2.
int fillModeFor(const QString &size)
{
    const QString s = (size.isEmpty() ? QStringLiteral("cover") : size).toLower();
    if (s == QLatin1String("contain"))
        return 1; // PreserveAspectFit
    if (s == QLatin1String("stretch") || s == QLatin1String("100% 100%"))
        return 0; // Stretch
    return 2;     // cover -> PreserveAspectCrop
}

// The solid-colour background bar behind the image: a REAL QtQuick Shape (Rectangle avoided per
// the owner's rule). Its PathRectangle tracks the Shape's own size, so C++ only manages width/
// height (layoutChildren) and the fill colour (recompute).
const char *kBgSolidShell = R"QML(
import QtQuick
import QtQuick.Shapes

Shape {
    id: s
    property alias fillColor: sp.fillColor
    preferredRendererType: Shape.CurveRenderer
    ShapePath {
        id: sp
        strokeWidth: 0
        strokeColor: "transparent"
        fillColor: "transparent"
        PathRectangle { x: 0; y: 0; width: s.width; height: s.height }
    }
}
)QML";

const char *kImageShell = R"QML(
import QtQuick

Image {
    horizontalAlignment: Image.AlignHCenter
    verticalAlignment: Image.AlignVCenter
    smooth: true
    asynchronous: true
    cache: true
}
)QML";

} // namespace

CssFill::CssFill(QQuickItem *parent)
    : QQuickItem(parent)
{
    // The content holder must exist BEFORE the QML engine appends declared children to `content`
    // (which happens during parsing, before componentComplete). A plain Item, sized to us, hosting
    // the layout participants — exactly the QML `contentHolder`.
    m_contentHolder = new QQuickItem(this);
    connect(m_contentHolder, &QQuickItem::childrenChanged, this, [this]() { requestRelayout(); });
}

bool CssFill::hasCssIdentity() const
{
    if (!m_cssId.isEmpty() || !m_cssPart.isEmpty() || !m_cssPrimitive.isEmpty())
        return true;
    return !toStringList(m_cssClass).isEmpty();
}

void CssFill::setCssId(const QString &v)
{
    if (m_cssId == v)
        return;
    m_cssId = v;
    emit cssIdChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssFill::setCssAlternateId(const QVariant &v)
{
    if (m_cssAlternateId == v)
        return;
    m_cssAlternateId = v;
    emit cssAlternateIdChanged();
}

void CssFill::setCssClass(const QVariant &v)
{
    if (m_cssClass == v)
        return;
    m_cssClass = v;
    emit cssClassChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssFill::setCssState(const QVariant &v)
{
    if (m_cssState == v)
        return;
    m_cssState = v;
    emit cssStateChanged();
    maybeLoadCss();
}

void CssFill::setCssPrimitive(const QString &v)
{
    if (m_cssPrimitive == v)
        return;
    m_cssPrimitive = v;
    emit cssPrimitiveChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss();
}

void CssFill::setCssPart(const QString &v)
{
    if (m_cssPart == v)
        return;
    m_cssPart = v;
    emit cssPartChanged();
    emit hasCssIdentityChanged();
}

void CssFill::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();
    recompute();
    requestRelayout();
    if (m_layout)
        m_layout->notifyParentLayout(this);
    emit inheritedChanged();
}

void CssFill::setRadius(qreal v)
{
    if (qFuzzyCompare(m_radius, v))
        return;
    m_radius = v;
    emit radiusChanged();
    recompute();
}

void CssFill::setDefaultColor(const QColor &v)
{
    if (m_defaultColor == v)
        return;
    m_defaultColor = v;
    emit defaultColorChanged();
    recompute();
}

void CssFill::setDefaultBorderColor(const QColor &v)
{
    if (m_defaultBorderColor == v)
        return;
    m_defaultBorderColor = v;
    emit defaultBorderColorChanged();
    recompute();
}

void CssFill::setDefaultBorderWidth(qreal v)
{
    if (qFuzzyCompare(m_defaultBorderWidth, v))
        return;
    m_defaultBorderWidth = v;
    emit defaultBorderWidthChanged();
    recompute();
}

void CssFill::setTransitionMs(int v)
{
    if (m_transitionMs == v)
        return;
    m_transitionMs = v; // deprecated no-op (kept for source compat)
    emit transitionMsChanged();
}

void CssFill::setTransitionEasingType(int v)
{
    if (m_transitionEasingType == v)
        return;
    m_transitionEasingType = v; // deprecated no-op
    emit transitionEasingTypeChanged();
}

// --- inherited text properties (CSS inheritance), same chain as CssRect ----------------------

static QObject *cssInheritingAncestor(const QQuickItem *self)
{
    QQuickItem *holder = self->parentItem();
    QQuickItem *box = holder ? holder->parentItem() : nullptr;
    if (box && box->property("inheritedColor").isValid())
        return box;
    return nullptr;
}

#define CSSFILL_INHERIT(Getter, Key)                                                                \
    QString CssFill::Getter() const                                                                \
    {                                                                                              \
        const QString own = m_style.value(QStringLiteral(Key)).toString();                        \
        if (!own.isEmpty())                                                                        \
            return own;                                                                            \
        QObject *anc = cssInheritingAncestor(this);                                                \
        return anc ? anc->property(#Getter).toString() : QString();                                \
    }

CSSFILL_INHERIT(inheritedColor, "color")
CSSFILL_INHERIT(inheritedFontFamily, "font-family")
CSSFILL_INHERIT(inheritedFontSize, "font-size")
CSSFILL_INHERIT(inheritedFontWeight, "font-weight")
CSSFILL_INHERIT(inheritedLineHeight, "line-height")
CSSFILL_INHERIT(inheritedLetterSpacing, "letter-spacing")
CSSFILL_INHERIT(inheritedTextTransform, "text-transform")
CSSFILL_INHERIT(inheritedTextAlign, "text-align")

#undef CSSFILL_INHERIT

// --- content default property (forwards to the contentHolder's `data`, the QML alias) --------

QQmlListProperty<QObject> CssFill::content()
{
    return QQmlListProperty<QObject>(
        this, nullptr,
        [](QQmlListProperty<QObject> *p, QObject *o) {
            auto *self = static_cast<CssFill *>(p->object);
            if (!self->m_contentHolder || !o)
                return;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            if (ref.isValid())
                ref.append(o);
        },
        [](QQmlListProperty<QObject> *p) -> qsizetype {
            auto *self = static_cast<CssFill *>(p->object);
            if (!self->m_contentHolder)
                return 0;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            return ref.isValid() ? ref.count() : 0;
        },
        [](QQmlListProperty<QObject> *p, qsizetype i) -> QObject * {
            auto *self = static_cast<CssFill *>(p->object);
            if (!self->m_contentHolder)
                return nullptr;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            return ref.isValid() ? ref.at(i) : nullptr;
        },
        [](QQmlListProperty<QObject> *p) {
            auto *self = static_cast<CssFill *>(p->object);
            if (!self->m_contentHolder)
                return;
            QQmlListReference ref(self->m_contentHolder.data(), "data");
            if (ref.isValid())
                ref.clear();
        });
}

void CssFill::requestRelayout()
{
    if (m_layout && m_contentHolder)
        m_layout->requestLayout(this, m_contentHolder);
}

void CssFill::maybeLoadCss()
{
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
}

void CssFill::recompute()
{
    auto str = [&](const char *k) { return m_style.value(QLatin1String(k)).toString(); };

    // QML: solidColor / backgroundValue / imageValue / bgIsImage / bgImageSource bindings.
    QColor solidColor = m_defaultColor;
    if (!str("background-color").isEmpty() && m_theme)
        solidColor = m_theme->parseColor(str("background-color"));

    const QString backgroundValue = str("background");
    QString imageValue = str("background-image");
    if (imageValue.isEmpty())
        imageValue = isCssUrl(backgroundValue) ? backgroundValue : QString();
    const bool bgIsImage = isCssUrl(imageValue);
    const QString bgImageSource = imageSource(imageValue);

    if (m_bgSolid) {
        m_bgSolid->setProperty("fillColor", solidColor);
        m_bgSolid->setVisible(bgIsImage);
    }

    if (m_image) {
        m_image->setVisible(bgIsImage && !bgImageSource.isEmpty());
        m_image->setProperty("source", QUrl(bgImageSource));
        m_image->setProperty("fillMode", fillModeFor(str("background-size")));
        const QString op = str("background-image-opacity");
        m_image->setProperty("opacity", op.isEmpty() ? 1.0 : op.toDouble());
    }

    // Forward the style + defaults onto the composed CssRect renderer. When an image is present
    // the fill is transparent so the image shows through (CssRect still renders the border).
    if (m_rect) {
        m_rect->setProperty("radius", m_radius);
        m_rect->setProperty("defaultColor", bgIsImage ? QColor(Qt::transparent) : m_defaultColor);
        m_rect->setProperty("defaultBorderColor", m_defaultBorderColor);
        m_rect->setProperty("defaultBorderWidth", m_defaultBorderWidth);
        m_rect->setProperty("style", m_style);
    }
}

void CssFill::layoutChildren()
{
    const qreal w = width();
    const qreal h = height();
    for (QQuickItem *child : {m_bgSolid.data(), m_image.data(), m_rect.data(), m_contentHolder.data()}) {
        if (!child)
            continue;
        child->setX(0);
        child->setY(0);
        child->setWidth(w);
        child->setHeight(h);
    }
}

void CssFill::componentComplete()
{
    QQuickItem::componentComplete();

    if (QQmlContext *ctx = qmlContext(this)) {
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    }

    if (QQmlEngine *eng = qmlEngine(this)) {
        // Compose bottom -> top; stackBefore(contentHolder) keeps the declared children on top.
        auto compose = [&](const char *qml) -> QQuickItem * {
            QQmlComponent comp(eng);
            comp.setData(qml, QUrl());
            QObject *o = comp.create(qmlContext(this));
            if (!o) {
                qWarning("CssFill: failed to compose layer: %s", qPrintable(comp.errorString()));
                return nullptr;
            }
            auto *item = qobject_cast<QQuickItem *>(o);
            if (!item) {
                o->deleteLater();
                return nullptr;
            }
            item->setParentItem(this);
            if (m_contentHolder)
                item->stackBefore(m_contentHolder);
            return item;
        };

        m_bgSolid = compose(kBgSolidShell);
        m_image = compose(kImageShell);
        // The CssRect renderer — our OWN registered type, composed via the type-system so the QML
        // engine runs its componentComplete (which builds its Shape/MultiEffect render subtree).
        m_rect = compose("import QtQuick\nimport qmlcss\nCssRect { cssPrimitive: \"\" }");
    }

    // React to implicit-size / visibility changes like the QML on*Changed handlers.
    connect(this, &QQuickItem::implicitWidthChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    connect(this, &QQuickItem::implicitHeightChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    // An ancestor's inherited text props re-propagate to us (CSS inheritance).
    if (QObject *anc = cssInheritingAncestor(this))
        connect(anc, SIGNAL(inheritedChanged()), this, SIGNAL(inheritedChanged()));

    layoutChildren();
    recompute();

    // QML Component.onCompleted.
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
    requestRelayout();
    if (m_layout)
        m_layout->notifyParentLayout(this);
}

void CssFill::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    layoutChildren();
    if (newGeometry.size() != oldGeometry.size())
        requestRelayout();
}
