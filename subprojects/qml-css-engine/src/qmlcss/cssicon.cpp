#include "qmlcss/cssicon.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QUrl>

// Equivalent QML (see cssicon.h for the annotated version):
//   Image { id: iconImage; anchors.centerIn: parent; width/height: iconSize;
//           sourceSize.width/height: iconSize; source: iconSource;
//           fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true; visible: false }
//   MultiEffect { anchors.fill: iconImage; source: iconImage;
//                 visible: iconImage.status === Image.Ready;
//                 colorization: colorize ? 1.0 : 0.0; colorizationColor: color }

CssIcon::CssIcon(QQuickItem *parent)
    : QQuickItem(parent)
{
}

// ---------- source URL resolution -------------------------------------------

// Literal C++ port of the QML `sourceFromCssUrl(value)` function:
//   strip url(...), strip surrounding quotes, prefix absolute paths with file://.
QString CssIcon::sourceFromCssUrl(const QString &value) const
{
    QString s = value.trimmed();

    // QML: if (s.toLowerCase().indexOf("url(") === 0) s = s.substring(4, s.length - 1).trim()
    if (s.toLower().startsWith(QLatin1String("url(")))
        s = s.mid(4, s.length() - 5).trimmed(); // mid(4, len-5) strips "url(" + ")"

    // QML: strip surrounding " or ' quotes
    if (s.length() >= 2) {
        const QChar first = s.front(), last = s.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"'))
            || (first == QLatin1Char('\'') && last == QLatin1Char('\'')))
            s = s.mid(1, s.length() - 2);
    }

    if (s.isEmpty())
        return {};

    // Already absolute scheme — return as-is.
    if (s.startsWith(QLatin1String("qrc:/"))
        || s.startsWith(QLatin1String("file:/"))
        || s.startsWith(QLatin1String("image://")))
        return s;

    // Absolute POSIX path — prefix with file://.
    if (s.startsWith(QLatin1Char('/')))
        return QStringLiteral("file://") + s;

    return s;
}

// Literal port of the QML `iconSource` computed property:
//   cssIconName  -> "image://themeicon/<name>|<hexcolor-without-#>"
//   cssIconValue -> sourceFromCssUrl(cssIconValue)
//   fallbackIconName -> "image://themeicon/<name>|<hexcolor-without-#>"
//   else         -> fallbackSource
//
// Contrast.toHex(color) in JS = "#" + hex(r) + hex(g) + hex(b); .slice(1) strips the "#".
// In Qt C++: color.name(QColor::HexRgb) gives "#rrggbb"; .mid(1) strips the "#".
QString CssIcon::resolveIconSource() const
{
    // QML: readonly property string cssIconValue: style["icon"] || style["icon-image"] || ""
    const QString cssIconValue = !m_style.value(QStringLiteral("icon")).toString().isEmpty()
        ? m_style.value(QStringLiteral("icon")).toString()
        : m_style.value(QStringLiteral("icon-image")).toString();

    // QML: readonly property string cssIconName: style["icon-name"] || ""
    const QString cssIconName = m_style.value(QStringLiteral("icon-name")).toString();

    // QML: if (cssIconName.length > 0) return "image://themeicon/<name>|<hexcolor>"
    if (!cssIconName.isEmpty())
        return QStringLiteral("image://themeicon/") + cssIconName
               + QLatin1Char('|') + m_color.name(QColor::HexRgb).mid(1);

    // QML: if (cssIconValue.length > 0) return sourceFromCssUrl(cssIconValue)
    if (!cssIconValue.isEmpty())
        return sourceFromCssUrl(cssIconValue);

    // QML: if (fallbackIconName.length > 0) return "image://themeicon/<name>|<hexcolor>"
    if (!m_fallbackIconName.isEmpty())
        return QStringLiteral("image://themeicon/") + m_fallbackIconName
               + QLatin1Char('|') + m_color.name(QColor::HexRgb).mid(1);

    // QML: return fallbackSource
    return m_fallbackSource;
}

// ---------- composed child updates ------------------------------------------

void CssIcon::updateIconSource()
{
    if (!m_image)
        return;
    const QString src = resolveIconSource();
    m_image->setProperty("source", src.isEmpty() ? QString() : src);
}

void CssIcon::updateColorize()
{
    if (!m_effect)
        return;
    // QML: colorization: root.colorize ? 1.0 : 0.0
    m_effect->setProperty("colorization", m_colorize ? 1.0 : 0.0);
    // QML: colorizationColor: root.color
    m_effect->setProperty("colorizationColor", m_color);
}

void CssIcon::updateAutoIconSize()
{
    if (m_iconSizeExplicit)
        return;
    const int newSize = qMin(static_cast<int>(width()), static_cast<int>(height()));
    if (m_iconSize == newSize)
        return;
    m_iconSize = newSize;
    emit iconSizeChanged();
    layoutChildren();
}

void CssIcon::layoutChildren()
{
    if (!m_image)
        return;
    const int sz = m_iconSize;
    // QML: anchors.centerIn: parent → x = (parent.width - width) / 2
    const qreal cx = (width() - sz) / 2.0;
    const qreal cy = (height() - sz) / 2.0;
    m_image->setX(cx);
    m_image->setY(cy);
    m_image->setWidth(sz);
    m_image->setHeight(sz);
    // QML: sourceSize.width/height: root.iconSize — grouped, must use QQmlProperty
    QQmlProperty(m_image.data(), QStringLiteral("sourceSize.width")).write(sz);
    QQmlProperty(m_image.data(), QStringLiteral("sourceSize.height")).write(sz);

    // QML: MultiEffect { anchors.fill: iconImage } → same pos+size as the Image
    if (m_effect) {
        m_effect->setX(cx);
        m_effect->setY(cy);
        m_effect->setWidth(sz);
        m_effect->setHeight(sz);
    }
}

// ---------- property setters ------------------------------------------------

void CssIcon::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();
    updateIconSource(); // source may change based on icon/icon-image/icon-name keys
}

void CssIcon::setFallbackSource(const QString &v)
{
    if (m_fallbackSource == v)
        return;
    m_fallbackSource = v;
    emit fallbackSourceChanged();
    updateIconSource();
}

void CssIcon::setFallbackIconName(const QString &v)
{
    if (m_fallbackIconName == v)
        return;
    m_fallbackIconName = v;
    emit fallbackIconNameChanged();
    updateIconSource();
}

void CssIcon::setColor(const QColor &v)
{
    if (m_color == v)
        return;
    m_color = v;
    emit colorChanged();
    updateColorize();
    // The themeicon URL embeds the hex color — re-resolve the source.
    updateIconSource();
}

void CssIcon::setColorize(bool v)
{
    if (m_colorize == v)
        return;
    m_colorize = v;
    emit colorizeChanged();
    updateColorize();
}

void CssIcon::setIconSize(int v)
{
    m_iconSizeExplicit = true;
    if (m_iconSize == v)
        return;
    m_iconSize = v;
    emit iconSizeChanged();
    layoutChildren();
}

bool CssIcon::ready() const
{
    if (!m_image)
        return false;
    // Image.Ready == 1 (QQuickImageBase::Ready)
    return m_image->property("status").toInt() == 1;
}

// ---------- lifecycle -------------------------------------------------------

void CssIcon::componentComplete()
{
    QQuickItem::componentComplete();

    // Initialise auto iconSize from current geometry before creating children.
    if (!m_iconSizeExplicit)
        m_iconSize = qMin(static_cast<int>(width()), static_cast<int>(height()));

    if (QQmlEngine *eng = qmlEngine(this)) {
        // REAL QtQuick Image — source + texture provider for the MultiEffect.
        // visible: false because the MultiEffect is the visible renderer.
        {
            QQmlComponent comp(eng);
            comp.setData(
                "import QtQuick\n"
                "Image {\n"
                "    fillMode: Image.PreserveAspectFit\n"  // == 1
                "    smooth: true; mipmap: true\n"
                "    visible: false\n"
                "}\n",
                QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                if (QQuickItem *img = qobject_cast<QQuickItem *>(o)) {
                    img->setParentItem(this);
                    m_image = img;
                } else {
                    o->deleteLater();
                }
            } else {
                qWarning("CssIcon: failed to compose Image: %s", qPrintable(comp.errorString()));
            }
        }

        // REAL QtQuick.Effects MultiEffect — visible renderer with colorization.
        if (m_image) {
            QQmlComponent comp(eng);
            comp.setData("import QtQuick.Effects\nMultiEffect {}\n", QUrl());
            if (QObject *o = comp.create(qmlContext(this))) {
                if (QQuickItem *fx = qobject_cast<QQuickItem *>(o)) {
                    fx->setParentItem(this);
                    // QML: source: iconImage
                    fx->setProperty("source", QVariant::fromValue(m_image.data()));
                    // QML: visible: iconImage.status === Image.Ready  (initially false)
                    fx->setVisible(false);
                    m_effect = fx;
                } else {
                    o->deleteLater();
                }
            } else {
                qWarning("CssIcon: failed to compose MultiEffect: %s", qPrintable(comp.errorString()));
            }
        }
    }

    layoutChildren();
    updateIconSource();
    updateColorize();

    // QML: visible: iconImage.status === Image.Ready — connect statusChanged to update the
    // MultiEffect visibility and emit readyChanged.
    // statusChanged signal has parameter QQuickImageBase::Status (private type) — use the
    // old-style string connection so no private header is needed.
    if (m_image) {
        QObject::connect(m_image.data(), SIGNAL(statusChanged(QQuickImageBase::Status)),
                         this, SLOT(onImageStatusChanged()));
    }
}

void CssIcon::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    // Recompute auto iconSize if it hasn't been set explicitly.
    updateAutoIconSize();
    // Always relayout (even if iconSize didn't change, children need repositioning).
    layoutChildren();
}

// ---------- private slot ----------------------------------------------------

void CssIcon::onImageStatusChanged()
{
    // QML: MultiEffect { visible: iconImage.status === Image.Ready }
    // Image.Ready == 1 (QQuickImageBase::Ready)
    const bool isReady = m_image && m_image->property("status").toInt() == 1;
    if (m_effect)
        m_effect->setVisible(isReady);
    emit readyChanged();
}
