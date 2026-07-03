#include "qmlcss/componentcache.h"
#include "qmlcss/csstext.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"
#include "qmlcss/valueparser.h"

#include <QFont>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTextOption>
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

// QML CssText._capitalization(v) -> QFont::Capitalization.
QFont::Capitalization capitalizationFor(const QString &v)
{
    const QString s = v.trimmed().toLower();
    if (s == QLatin1String("uppercase"))
        return QFont::AllUppercase;
    if (s == QLatin1String("lowercase"))
        return QFont::AllLowercase;
    if (s == QLatin1String("capitalize"))
        return QFont::Capitalize;
    return QFont::MixedCase;
}

} // namespace

CssText::CssText(QQuickItem *parent)
    : QQuickItem(parent)
{
}

bool CssText::hasCssIdentity() const
{
    // QML: cssId || cssPart || cssPrimitive (type selector) || cssClass length.
    if (!m_cssId.isEmpty() || !m_cssPart.isEmpty() || !m_cssPrimitive.isEmpty())
        return true;
    return !toStringList(m_cssClass).isEmpty();
}

void CssText::setCssId(const QString &v)
{
    if (m_cssId == v)
        return;
    m_cssId = v;
    emit cssIdChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss(); // QML: onCssIdChanged -> cssTheme.loadCss(root)
}

void CssText::setCssAlternateId(const QVariant &v)
{
    if (m_cssAlternateId == v)
        return;
    m_cssAlternateId = v;
    emit cssAlternateIdChanged();
}

void CssText::setCssClass(const QVariant &v)
{
    if (m_cssClass == v)
        return;
    m_cssClass = v;
    emit cssClassChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss(); // QML: onCssClassChanged -> cssTheme.loadCss(root)
}

void CssText::setCssState(const QVariant &v)
{
    if (m_cssState == v)
        return;
    m_cssState = v;
    emit cssStateChanged();
    maybeLoadCss();
}

void CssText::setCssPrimitive(const QString &v)
{
    if (m_cssPrimitive == v)
        return;
    m_cssPrimitive = v;
    emit cssPrimitiveChanged();
    emit hasCssIdentityChanged();
    maybeLoadCss(); // QML: onCssPrimitiveChanged -> cssTheme.loadCss(root)
}

void CssText::setCssPart(const QString &v)
{
    if (m_cssPart == v)
        return;
    m_cssPart = v;
    emit cssPartChanged();
    emit hasCssIdentityChanged();
}

void CssText::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();

    applyToText();
    applyShadow();
    applyBackground();

    // QML: onStyleChanged -> _notifyParent()
    if (m_layout)
        m_layout->notifyParentLayout(this);
    emit inheritedChanged();
}

void CssText::setText(const QString &v)
{
    if (m_text == v)
        return;
    m_text = v;
    if (m_label)
        m_label->setProperty("text", m_text);
    emit textChanged();
}

void CssText::setDefaultColor(const QColor &v)
{
    if (m_defaultColor == v)
        return;
    m_defaultColor = v;
    emit defaultColorChanged();
    applyToText();
}

void CssText::setDefaultFontFamily(const QString &v)
{
    if (m_defaultFontFamily == v)
        return;
    m_defaultFontFamily = v;
    emit defaultFontFamilyChanged();
    applyToText();
}

void CssText::setDefaultFontSize(qreal v)
{
    if (qFuzzyCompare(m_defaultFontSize, v))
        return;
    m_defaultFontSize = v;
    emit defaultFontSizeChanged();
    applyToText();
}

// --- CSS inheritance chain (QML _cssParent + inherited* getters) -----------------------------

QQuickItem *CssText::cssParentItem() const
{
    // QML: a directly-styled parent (inheritedColor non-empty) wins; else the container at
    // parent.parent (whichever exposes inheritedColor).
    QQuickItem *p = parentItem();
    if (p) {
        const QVariant ic = p->property("inheritedColor");
        if (ic.isValid() && !ic.toString().isEmpty())
            return p;
    }
    QQuickItem *pp = p ? p->parentItem() : nullptr;
    if (pp && pp->property("inheritedColor").isValid())
        return pp;
    return nullptr;
}

QString CssText::inheritedValue(const char *styleKey, const char *getterName) const
{
    const QString own = m_style.value(QLatin1String(styleKey)).toString();
    if (!own.isEmpty())
        return own;
    QQuickItem *anc = cssParentItem();
    return anc ? anc->property(getterName).toString() : QString();
}

QString CssText::inheritedColor() const { return inheritedValue("color", "inheritedColor"); }
QString CssText::inheritedFontFamily() const { return inheritedValue("font-family", "inheritedFontFamily"); }
QString CssText::inheritedFontSize() const { return inheritedValue("font-size", "inheritedFontSize"); }
QString CssText::inheritedFontWeight() const { return inheritedValue("font-weight", "inheritedFontWeight"); }
QString CssText::inheritedLineHeight() const { return inheritedValue("line-height", "inheritedLineHeight"); }
QString CssText::inheritedLetterSpacing() const { return inheritedValue("letter-spacing", "inheritedLetterSpacing"); }
QString CssText::inheritedTextTransform() const { return inheritedValue("text-transform", "inheritedTextTransform"); }
QString CssText::inheritedTextAlign() const { return inheritedValue("text-align", "inheritedTextAlign"); }

// --- apply resolved CSS onto the composed Text -----------------------------------------------

void CssText::applyToText()
{
    if (!m_label)
        return;

    QQuickItem *t = m_label;
    auto styleStr = [&](const char *k) { return m_style.value(QLatin1String(k)).toString(); };

    // text
    t->setProperty("text", m_text);

    // color <- inheritedColor, else default.
    const QString ic = inheritedColor();
    const QColor color = (!ic.isEmpty() && m_theme) ? m_theme->parseColor(ic) : m_defaultColor;
    t->setProperty("color", color);

    // --- font (family/pointSize/weight/letterSpacing/wordSpacing/capitalization/underline/strike) ---
    QFont font;
    const QString fam = inheritedFontFamily();
    font.setFamily(m_theme ? m_theme->resolveFontFamily(fam, m_defaultFontFamily)
                           : (fam.isEmpty() ? m_defaultFontFamily : fam));
    const QString fs = inheritedFontSize();
    const qreal pointSize = (!fs.isEmpty() && m_theme) ? m_theme->parseFontSize(fs, m_defaultFontSize)
                                                       : m_defaultFontSize;
    font.setPointSizeF(pointSize);
    if (m_theme)
        font.setWeight(static_cast<QFont::Weight>(m_theme->fontWeight(inheritedFontWeight())));
    const QString ls = inheritedLetterSpacing();
    font.setLetterSpacing(QFont::AbsoluteSpacing, (!ls.isEmpty() && m_theme) ? m_theme->parseLength(ls, 0) : 0);
    const QString ws = styleStr("word-spacing");
    font.setWordSpacing((!ws.isEmpty() && m_theme) ? m_theme->parseLength(ws, 0) : 0);
    font.setCapitalization(capitalizationFor(inheritedTextTransform()));
    const QString decoration = styleStr("text-decoration");
    font.setUnderline(decoration.contains(QLatin1String("underline")));
    font.setStrikeOut(decoration.contains(QLatin1String("line-through")));
    t->setProperty("font", QVariant::fromValue(font));

    // line-height: mode + value (px). QML fallback px = font pixel size (or pointSize*96/72).
    const QString lh = inheritedLineHeight();
    const bool proportional = m_theme ? m_theme->isProportionalLineHeight(lh) : true;
    t->setProperty("lineHeightMode", proportional ? 0 : 1); // Text.ProportionalHeight : Text.FixedHeight
    const qreal fallbackPx = pointSize * 96.0 / 72.0;
    t->setProperty("lineHeight", m_theme ? m_theme->parseLineHeight(lh, fallbackPx) : fallbackPx);

    // Transpiled JSX text is literal — never rich text.
    t->setProperty("textFormat", 0); // Text.PlainText
    t->setProperty("verticalAlignment", static_cast<int>(Qt::AlignVCenter));

    // padding (QML boxSideLength side order: left=3, right=1, top=0, bottom=2).
    if (m_theme) {
        t->setProperty("leftPadding", m_theme->boxSideLength(m_style, QStringLiteral("padding"), 3));
        t->setProperty("rightPadding", m_theme->boxSideLength(m_style, QStringLiteral("padding"), 1));
        t->setProperty("topPadding", m_theme->boxSideLength(m_style, QStringLiteral("padding"), 0));
        t->setProperty("bottomPadding", m_theme->boxSideLength(m_style, QStringLiteral("padding"), 2));
    }

    // white-space: nowrap -> NoWrap, else WordWrap.
    t->setProperty("wrapMode", static_cast<int>(styleStr("white-space") == QLatin1String("nowrap")
                                                    ? QTextOption::NoWrap
                                                    : QTextOption::WordWrap));
    // text-overflow: ellipsis -> ElideRight, else ElideNone.
    t->setProperty("elide", static_cast<int>(styleStr("text-overflow") == QLatin1String("ellipsis")
                                                  ? Qt::ElideRight
                                                  : Qt::ElideNone));

    // text-align -> horizontalAlignment.
    const QString align = inheritedTextAlign();
    const Qt::Alignment halign = align == QLatin1String("center")
        ? Qt::AlignHCenter
        : (align == QLatin1String("right") ? Qt::AlignRight : Qt::AlignLeft);
    t->setProperty("horizontalAlignment", static_cast<int>(halign));

    // display:none never writes `visible` (imperative writes sever author bindings, e.g. the
    // <Show> guard); the layout skips it by style, painting dies via opacity 0.
    if (styleStr("display") == QLatin1String("none") || styleStr("visibility") == QLatin1String("hidden"))
        setOpacity(0);
    else
        setOpacity(m_style.contains(QStringLiteral("opacity")) ? m_style.value(QStringLiteral("opacity")).toReal()
                                                               : 1.0);

    mirrorImplicit();
}

void CssText::applyShadow()
{
    // QML: _dropShadow = (cssTheme.loaded && style["text-shadow"]) ? parseBoxShadow(...) : ({})
    QVariantMap shadow;
    const QString ts = m_style.value(QStringLiteral("text-shadow")).toString();
    if (m_theme && m_theme->isLoaded() && !ts.isEmpty())
        shadow = m_theme->parseBoxShadow(ts);
    const bool hasShadow = shadow.contains(QStringLiteral("color"));

    // Lazy composition: only a label that actually declares text-shadow pays for the effect.
    if (!m_shadow && hasShadow && m_label && isComponentComplete()) {
        if (QQmlEngine *eng = qmlEngine(this)) {
            QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csstext-81a20bf2"),
                "import QtQuick.Effects\nMultiEffect { autoPaddingEnabled: true; visible: false }");
            if (QObject *o = comp->create(qmlContext(this))) {
                if (QQuickItem *fx = qobject_cast<QQuickItem *>(o)) {
                    fx->setParent(this);
                    fx->setParentItem(this);
                    fx->setProperty("source", QVariant::fromValue(m_label.data()));
                    m_shadow = fx;
                    layoutChild(); // size the fresh effect to the current box
                } else {
                    o->deleteLater();
                }
            }
        }
    }
    if (!m_shadow)
        return;

    // Mirror CssDropShadow.qml onto the composed MultiEffect (sourcing the Text).
    m_shadow->setProperty("shadowEnabled", hasShadow);
    m_shadow->setProperty("shadowColor", hasShadow ? shadow.value(QStringLiteral("color")).value<QColor>()
                                                    : QColor(Qt::transparent));
    m_shadow->setProperty("shadowHorizontalOffset", hasShadow ? shadow.value(QStringLiteral("x")).toReal() : 0.0);
    m_shadow->setProperty("shadowVerticalOffset", hasShadow ? shadow.value(QStringLiteral("y")).toReal() : 0.0);
    // MultiEffect.shadowBlur is 0..1 over blurMax (32px default).
    m_shadow->setProperty("shadowBlur",
                          hasShadow ? std::min<qreal>(1.0, shadow.value(QStringLiteral("blur")).toReal() / 32.0) : 0.0);

    // The MultiEffect draws the Text (plus shadow) when active — ABOVE the still-visible label
    // (same glyphs, so no double vision). Hiding the label empties the effect's source texture
    // on the RHI path, blanking the text entirely.
    m_shadow->setVisible(hasShadow);
    if (m_label)
        m_label->setVisible(true);
}

void CssText::layoutChild()
{
    if (!m_label)
        return;
    m_label->setX(0);
    m_label->setY(0);
    // When a width/height is assigned (by the layout engine), fill it so wrap/elide/valign apply;
    // otherwise let the Text keep its own implicit size so a standalone label never collapses.
    const qreal w = width();
    m_label->setWidth(w > 0.5 ? w : m_label->implicitWidth());
    const qreal h = height();
    m_label->setHeight(h > 0.5 ? h : m_label->implicitHeight());

    if (m_shadow) {
        m_shadow->setX(0);
        m_shadow->setY(0);
        m_shadow->setWidth(m_label->width());
        m_shadow->setHeight(m_label->height());
    }
    if (m_bg) {
        m_bg->setX(0);
        m_bg->setY(0);
        m_bg->setWidth(m_label->width());
        m_bg->setHeight(m_label->height());
    }
}

void CssText::mirrorImplicit()
{
    if (!m_label)
        return;
    // We are the layout participant; our implicit size is the composed Text's.
    setImplicitWidth(m_label->implicitWidth());
    setImplicitHeight(m_label->implicitHeight());
    // Keep the Text at its natural size while unmanaged.
    if (width() <= 0.5)
        m_label->setWidth(m_label->implicitWidth());
    if (height() <= 0.5)
        m_label->setHeight(m_label->implicitHeight());
}

void CssText::maybeLoadCss()
{
    // QML: if (cssTheme && hasCssIdentity()) cssTheme.loadCss(root)
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this);
}

void CssText::componentComplete()
{
    QQuickItem::componentComplete();

    if (QQmlContext *ctx = qmlContext(this)) {
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    }

    if (QQmlEngine *eng = qmlEngine(this)) {
        // The REAL QtQuick Text, via the Qt type-system (we forward everything onto it).
        {
            QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csstext-bf5e2923"),
                "import QtQuick\nText {}");
            if (QObject *o = comp->create(qmlContext(this))) {
                if (QQuickItem *label = qobject_cast<QQuickItem *>(o)) {
                    label->setParentItem(this);
                    m_label = label;
                } else {
                    o->deleteLater();
                }
            } else {
                qWarning("CssText: failed to compose Text: %s", qPrintable(comp->errorString()));
            }
        }

        // The text-shadow MultiEffect is composed LAZILY in applyShadow(): a MultiEffect
        // drags in ShaderEffectSources + blur items even while invisible, and most labels
        // never declare text-shadow.
    }

    if (m_label) {
        m_label->setProperty("text", m_text);
        // The composed Text's implicit size drives ours; re-flow the parent on change.
        connect(m_label, &QQuickItem::implicitWidthChanged, this, [this]() {
            mirrorImplicit();
            layoutChild();
        });
        connect(m_label, &QQuickItem::implicitHeightChanged, this, [this]() {
            mirrorImplicit();
            layoutChild();
        });
    }

    // notifyParentLayout on implicit/visible change (QML on*Changed -> _notifyParent), like CssHr.
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
    // font.family re-resolves once a downloaded @font-face registers (QML leading fontRevision dep).
    if (m_theme)
        connect(m_theme, &CssTheme::fontRevisionChanged, this, [this]() {
            applyToText();
            applyShadow();
        });
    // An ancestor's inherited text props re-propagate to us (CSS inheritance).
    if (QQuickItem *anc = cssParentItem())
        connect(anc, SIGNAL(inheritedChanged()), this, SLOT(onAncestorInheritedChanged()));

    layoutChild();
    applyToText();
    applyShadow();
    applyBackground(); // a pre-completion style may already carry a background

    // QML Component.onCompleted.
    if (m_theme && hasCssIdentity())
        m_theme->loadCss(this); // sets `style` -> setStyle re-applies + notifies layout
    if (m_layout)
        m_layout->notifyParentLayout(this);
}

void CssText::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    layoutChild();
}

// QML equivalent (what the underlay would look like declared in CssText.qml):
//   Shape {
//       z: -1; visible: bgColor.a > 0
//       ShapePath {
//           strokeColor: "transparent"; strokeWidth: 0; fillColor: bgColor
//           PathRectangle { width: parent.width; height: parent.height; radius: bgRadius }
//       }
//   }
void CssText::applyBackground()
{
    // `background-color` wins; a plain-colour `background` shorthand also paints (gradient
    // strings parse invalid and are skipped — text gradients are not mapped).
    QString bgValue = m_style.value(QStringLiteral("background-color")).toString().trimmed();
    if (bgValue.isEmpty())
        bgValue = m_style.value(QStringLiteral("background")).toString().trimmed();
    const QColor color = bgValue.isEmpty() ? QColor() : CssValueParser::parseColor(bgValue);

    if (!color.isValid() || color.alpha() == 0) {
        if (m_bg)
            m_bg->setProperty("bgColor", QColor(Qt::transparent));
        return;
    }

    if (!m_bg && isComponentComplete()) {
        if (QQmlEngine *eng = qmlEngine(this)) {
            QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csstext-f477b5b7"),
                R"(import QtQuick
import QtQuick.Shapes
Shape {
    id: bgroot
    objectName: "cssTextBg"
    property color bgColor: "transparent"
    property real bgRadius: 0
    z: -1
    visible: bgColor.a > 0
    preferredRendererType: Shape.CurveRenderer
    ShapePath {
        strokeColor: "transparent"
        strokeWidth: 0
        fillColor: bgroot.bgColor
        PathRectangle { x: 0; y: 0; width: bgroot.width; height: bgroot.height; radius: bgroot.bgRadius }
    }
})");
            if (QObject *o = comp->create(qmlContext(this))) {
                if (QQuickItem *bg = qobject_cast<QQuickItem *>(o)) {
                    bg->setParent(this); // C++ ownership (mirrors CssRect's composed scene)
                    bg->setParentItem(this);
                    m_bg = bg;
                } else {
                    o->deleteLater();
                }
            } else {
                qWarning("CssText: failed to compose background Shape: %s", qPrintable(comp->errorString()));
            }
        }
    }
    if (!m_bg)
        return;

    double radius = 0;
    CssValueParser::parseLengthPx(m_style.value(QStringLiteral("border-radius")).toString(), &radius);
    m_bg->setProperty("bgColor", color);
    m_bg->setProperty("bgRadius", radius);
    layoutChild(); // size the fresh underlay to the current box
}

void CssText::onAncestorInheritedChanged()
{
    applyToText();
    applyShadow();
    emit inheritedChanged();
}
