#include "richtext_styles.h"

#include <QBrush>
#include <QFont>
#include <QSet>
#include <QTextBlockFormat>
#include <QTextCharFormat>

#include <cmath>

namespace rt {

QString defaultParagraphStyle() { return QStringLiteral("Default Paragraph Style"); }
QString textBodyStyle() { return QStringLiteral("Text Body"); }

namespace {

constexpr const char *kPropNames[kPropCount] = {
    "fontFamily", "fontSize", "fontWeight", "italic", "underline", "strike", "color", "letterSpacing", "language",
    "marginTop", "marginBottom", "marginLeft", "marginRight", "textIndent", "lineHeight", "alignment",
    "outlineLevel", "keepWithNext",
};

// The raw Qt properties behind each style property (what a rebase copies from the new base).
QList<int> rawIds(Prop p)
{
    switch (p) {
    case Prop::FontFamily: return { QTextFormat::FontFamilies, QTextFormat::FontFamily };
    case Prop::Language: return { kLanguageProperty };
    case Prop::FontSize: return { QTextFormat::FontPointSize };
    case Prop::FontWeight: return { QTextFormat::FontWeight };
    case Prop::Italic: return { QTextFormat::FontItalic };
    case Prop::Underline: return { QTextFormat::TextUnderlineStyle, QTextFormat::FontUnderline };
    case Prop::Strike: return { QTextFormat::FontStrikeOut };
    case Prop::Color: return { QTextFormat::ForegroundBrush };
    case Prop::LetterSpacing: return { QTextFormat::FontLetterSpacing, QTextFormat::FontLetterSpacingType };
    case Prop::MarginTop: return { QTextFormat::BlockTopMargin };
    case Prop::MarginBottom: return { QTextFormat::BlockBottomMargin };
    case Prop::MarginLeft: return { QTextFormat::BlockLeftMargin };
    case Prop::MarginRight: return { QTextFormat::BlockRightMargin };
    case Prop::TextIndent: return { QTextFormat::TextIndent };
    case Prop::LineHeight: return { QTextFormat::LineHeight, QTextFormat::LineHeightType };
    case Prop::Alignment: return { QTextFormat::BlockAlignment };
    case Prop::OutlineLevel: return { QTextFormat::HeadingLevel };
    case Prop::KeepWithNext: return { kKeepWithNextProperty };
    }
    return {};
}

bool isNumber(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Double: case QMetaType::Float: case QMetaType::Int: case QMetaType::UInt:
    case QMetaType::LongLong: case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

bool sameValue(const QVariant &a, const QVariant &b)
{
    if (!a.isValid() || !b.isValid())
        return a.isValid() == b.isValid();
    if (isNumber(a) && isNumber(b))
        return qAbs(a.toDouble() - b.toDouble()) < 1e-3;
    return a == b;
}

QString pxString(qreal px) { return QString::number(px, 'g', 6) + QStringLiteral("px"); }

void writeValue(QTextFormat &f, Prop p, const QVariant &v)
{
    switch (p) {
    case Prop::Language: f.setProperty(kLanguageProperty, v.toString()); break;
    case Prop::FontFamily:
        f.clearProperty(QTextFormat::FontFamily);
        f.setProperty(QTextFormat::FontFamilies, QStringList { v.toString() });
        break;
    case Prop::FontSize: f.setProperty(QTextFormat::FontPointSize, v.toDouble()); break;
    case Prop::FontWeight: f.setProperty(QTextFormat::FontWeight, v.toInt()); break;
    case Prop::Italic: f.setProperty(QTextFormat::FontItalic, v.toBool()); break;
    case Prop::Underline:
        f.clearProperty(QTextFormat::FontUnderline);
        f.setProperty(QTextFormat::TextUnderlineStyle,
                      int(v.toBool() ? QTextCharFormat::SingleUnderline : QTextCharFormat::NoUnderline));
        break;
    case Prop::Strike: f.setProperty(QTextFormat::FontStrikeOut, v.toBool()); break;
    case Prop::Color: f.setProperty(QTextFormat::ForegroundBrush, QBrush(QColor(v.toString()))); break;
    case Prop::LetterSpacing:
        f.setProperty(QTextFormat::FontLetterSpacingType, int(QFont::AbsoluteSpacing));
        f.setProperty(QTextFormat::FontLetterSpacing, v.toDouble());
        break;
    case Prop::MarginTop: f.setProperty(QTextFormat::BlockTopMargin, v.toDouble()); break;
    case Prop::MarginBottom: f.setProperty(QTextFormat::BlockBottomMargin, v.toDouble()); break;
    case Prop::MarginLeft: f.setProperty(QTextFormat::BlockLeftMargin, v.toDouble()); break;
    case Prop::MarginRight: f.setProperty(QTextFormat::BlockRightMargin, v.toDouble()); break;
    case Prop::TextIndent: f.setProperty(QTextFormat::TextIndent, v.toDouble()); break;
    case Prop::LineHeight:
        if (v.typeId() == QMetaType::QString) {
            f.setProperty(QTextFormat::LineHeightType, int(QTextBlockFormat::FixedHeight));
            f.setProperty(QTextFormat::LineHeight, lengthPx(v.toString()));
        } else {
            f.setProperty(QTextFormat::LineHeightType, int(QTextBlockFormat::ProportionalHeight));
            f.setProperty(QTextFormat::LineHeight, v.toDouble() * 100.0);
        }
        break;
    case Prop::Alignment: {
        const QString a = v.toString();
        Qt::Alignment al = Qt::AlignLeft;
        if (a == QLatin1String("center")) al = Qt::AlignHCenter;
        else if (a == QLatin1String("right")) al = Qt::AlignRight;
        else if (a == QLatin1String("justify")) al = Qt::AlignJustify;
        f.setProperty(QTextFormat::BlockAlignment, int(al));
        break;
    }
    case Prop::OutlineLevel:
        if (v.toInt() > 0) f.setProperty(QTextFormat::HeadingLevel, v.toInt());
        else f.clearProperty(QTextFormat::HeadingLevel);
        break;
    case Prop::KeepWithNext: f.setProperty(kKeepWithNextProperty, v.toBool()); break;
    }
}

void copyRaw(QTextFormat &f, const QTextFormat &from, Prop p)
{
    for (int id : rawIds(p)) {
        if (from.hasProperty(id)) f.setProperty(id, from.property(id));
        else f.clearProperty(id);
    }
}

bool isNameChar(QChar c, bool first)
{
    if (c == QLatin1Char('_') || c.isLetter())
        return true;
    if (first)
        return false;
    return c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char('-');
}

} // namespace

const char *propName(Prop p) { return kPropNames[int(p)]; }

bool propFromName(QStringView name, Prop *out)
{
    for (int i = 0; i < kPropCount; ++i) {
        if (name == QLatin1String(kPropNames[i])) {
            *out = Prop(i);
            return true;
        }
    }
    return false;
}

qreal lengthPx(QStringView v)
{
    v = v.trimmed();
    if (v.isEmpty())
        return qQNaN();
    int i = 0;
    while (i < v.size() && (v.at(i).isDigit() || v.at(i) == QLatin1Char('.') || v.at(i) == QLatin1Char('-')
                            || v.at(i) == QLatin1Char('+')))
        ++i;
    bool ok = false;
    const qreal n = v.left(i).toDouble(&ok);
    if (!ok)
        return qQNaN();
    const QStringView unit = v.mid(i).trimmed();
    if (unit == QLatin1String("in")) return n * 96.0;
    if (unit == QLatin1String("cm")) return n * 96.0 / 2.54;
    if (unit == QLatin1String("mm")) return n * 96.0 / 25.4;
    if (unit == QLatin1String("pt")) return n * 96.0 / 72.0;
    if (unit == QLatin1String("pc")) return n * 16.0;
    if (unit.isEmpty() || unit == QLatin1String("px")) return n;
    return qQNaN();
}

bool canonicalValue(Prop p, const QVariant &value, QVariant *out, QString *error)
{
    const QString name = QLatin1String(propName(p));
    const auto fail = [&](const char *what) {
        if (error) *error = name + QStringLiteral(": ") + QLatin1String(what);
        return false;
    };
    const QString s = value.typeId() == QMetaType::QString ? value.toString().trimmed() : QString();
    const auto number = [&](qreal *n) {
        if (isNumber(value)) { *n = value.toDouble(); return std::isfinite(*n); }
        bool ok = false;
        *n = s.toDouble(&ok);
        return ok && std::isfinite(*n);
    };
    const auto length = [&](qreal *px) {
        if (isNumber(value)) { *px = value.toDouble(); return std::isfinite(*px); }
        *px = lengthPx(s);
        return !qIsNaN(*px);
    };
    const auto boolean = [&](bool *b) {
        if (value.typeId() == QMetaType::Bool) { *b = value.toBool(); return true; }
        if (isNumber(value)) { *b = value.toDouble() != 0; return true; }
        if (s == QLatin1String("true")) { *b = true; return true; }
        if (s == QLatin1String("false")) { *b = false; return true; }
        return false;
    };
    switch (p) {
    case Prop::FontFamily: {
        QString f = value.toString().trimmed();
        if (f.size() >= 2 && (f.front() == QLatin1Char('\'') || f.front() == QLatin1Char('"')) && f.back() == f.front())
            f = f.mid(1, f.size() - 2);
        if (f.isEmpty()) return fail("expected a font family name");
        *out = f;
        return true;
    }
    case Prop::Language:
        *out = value.toString().trimmed().replace(QLatin1Char('_'), QLatin1Char('-'));
        return true;
    case Prop::FontSize: {
        qreal n = 0;
        if (s.endsWith(QLatin1String("px"))) {
            n = lengthPx(s) * 0.75;
            if (qIsNaN(n)) return fail("expected a size in points");
        } else if (s.endsWith(QLatin1String("pt"))) {
            bool ok = false;
            n = s.chopped(2).toDouble(&ok);
            if (!ok) return fail("expected a size in points");
        } else if (!number(&n)) {
            return fail("expected a size in points");
        }
        if (n <= 0) return fail("must be positive");
        *out = n;
        return true;
    }
    case Prop::FontWeight: {
        qreal n = 0;
        if (s == QLatin1String("bold")) n = 700;
        else if (s == QLatin1String("normal")) n = 400;
        else if (!number(&n)) return fail("expected 1..1000, \"bold\" or \"normal\"");
        if (n < 1 || n > 1000) return fail("expected 1..1000");
        *out = int(n);
        return true;
    }
    case Prop::Italic: case Prop::Underline: case Prop::Strike: case Prop::KeepWithNext: {
        bool b = false;
        if (!boolean(&b)) return fail("expected true or false");
        *out = b;
        return true;
    }
    case Prop::Color: {
        const QColor c = value.typeId() == QMetaType::QColor ? value.value<QColor>() : QColor(s);
        if (!c.isValid()) return fail("expected a colour (\"#rrggbb\")");
        *out = c.name(QColor::HexRgb);
        return true;
    }
    case Prop::LetterSpacing: case Prop::MarginTop: case Prop::MarginBottom: case Prop::MarginLeft:
    case Prop::MarginRight: case Prop::TextIndent: {
        qreal px = 0;
        if (s == QLatin1String("normal") && p == Prop::LetterSpacing) px = 0;
        else if (!length(&px)) return fail("expected a length (px, or a string with a unit)");
        *out = px;
        return true;
    }
    case Prop::LineHeight: {
        if (s == QLatin1String("normal")) { *out = 1.0; return true; }
        if (s.endsWith(QLatin1Char('%'))) {
            bool ok = false;
            const qreal n = s.chopped(1).toDouble(&ok);
            if (!ok || n <= 0) return fail("expected a percentage");
            *out = n / 100.0;
            return true;
        }
        if (isNumber(value) || (!s.isEmpty() && s.at(s.size() - 1).isDigit())) {
            qreal n = 0;
            if (!number(&n) || n <= 0) return fail("expected a positive multiple");
            *out = n;
            return true;
        }
        const qreal px = lengthPx(s);
        if (qIsNaN(px) || px <= 0) return fail("expected a multiple (1.5) or a length (\"18pt\")");
        *out = pxString(px);
        return true;
    }
    case Prop::Alignment:
        if (s == QLatin1String("left") || s == QLatin1String("start")) *out = QStringLiteral("left");
        else if (s == QLatin1String("center")) *out = QStringLiteral("center");
        else if (s == QLatin1String("right") || s == QLatin1String("end")) *out = QStringLiteral("right");
        else if (s == QLatin1String("justify")) *out = QStringLiteral("justify");
        else return fail("expected \"left\", \"center\", \"right\" or \"justify\"");
        return true;
    case Prop::OutlineLevel: {
        qreal n = 0;
        if (!number(&n) || n < 0 || n > 10 || n != std::floor(n)) return fail("expected 0..10");
        *out = int(n);
        return true;
    }
    }
    return fail("unknown property");
}

// ─── StyleSheet ─────────────────────────────────────────────────────────────────────────────────

StyleSheet StyleSheet::builtIns()
{
    // LibreOffice's default template (sizes resolved to points, spacing to px at 96 dpi). Font
    // families are left to the editor (LibreOffice's Liberation fonts are a print choice).
    const auto cm = [](qreal v) { return v * 96.0 / 2.54; };
    StyleSheet s;
    const auto para = [&](const QString &name, const QString &odf, const QString &display, const QString &parent,
                          const QString &next, const QVariantMap &props) {
        NamedStyle st;
        st.name = name;
        st.family = Family::Paragraph;
        st.parent = parent;
        st.next = next;
        st.props = props;
        st.odfName = odf;
        st.odfDisplayName = display;
        s.put(st);
    };
    para(defaultParagraphStyle(), QStringLiteral("Standard"), QString(), QString(), QString(), {});
    para(textBodyStyle(), QStringLiteral("Text_20_body"), QStringLiteral("Text body"), defaultParagraphStyle(),
         QString(), { { "marginTop", 0.0 }, { "marginBottom", cm(0.247) }, { "lineHeight", 1.15 } });
    para(QStringLiteral("Heading"), QStringLiteral("Heading"), QString(), defaultParagraphStyle(), textBodyStyle(),
         { { "marginTop", cm(0.423) }, { "marginBottom", cm(0.212) }, { "keepWithNext", true }, { "fontSize", 14.0 } });
    struct H { qreal size; bool italic; qreal top; };
    const H hs[] = { { 18.2, false, 0.423 }, { 16.1, false, 0.353 }, { 14.1, false, 0.247 },
                     { 13.3, true, 0.212 }, { 11.9, false, 0.212 }, { 11.9, true, 0.106 } };
    for (int i = 0; i < 6; ++i) {
        QVariantMap p { { "outlineLevel", i + 1 }, { "fontSize", hs[i].size }, { "fontWeight", 700 },
                        { "marginTop", cm(hs[i].top) }, { "marginBottom", cm(0.212) } };
        if (hs[i].italic) p.insert(QStringLiteral("italic"), true);
        para(QStringLiteral("Heading %1").arg(i + 1), QStringLiteral("Heading_20_%1").arg(i + 1),
             QStringLiteral("Heading %1").arg(i + 1), QStringLiteral("Heading"), textBodyStyle(), p);
    }
    para(QStringLiteral("Title"), QStringLiteral("Title"), QString(), QStringLiteral("Heading"), textBodyStyle(),
         { { "alignment", QStringLiteral("center") }, { "fontSize", 28.0 }, { "fontWeight", 700 },
           { "marginTop", 0.0 }, { "marginBottom", cm(0.212) } });
    para(QStringLiteral("Quotations"), QStringLiteral("Quotations"), QString(), defaultParagraphStyle(), QString(),
         { { "marginLeft", cm(1.0) }, { "marginRight", cm(1.0) }, { "marginTop", 0.0 }, { "marginBottom", cm(0.499) } });

    NamedStyle em;
    em.name = QStringLiteral("Emphasis");
    em.family = Family::Character;
    em.props = { { "italic", true } };
    em.odfName = QStringLiteral("Emphasis");
    s.put(em);
    NamedStyle strong;
    strong.name = QStringLiteral("Strong Emphasis");
    strong.family = Family::Character;
    strong.props = { { "fontWeight", 700 } };
    strong.odfName = QStringLiteral("Strong_20_Emphasis");
    strong.odfDisplayName = QStringLiteral("Strong Emphasis");
    s.put(strong);
    return s;
}

const NamedStyle *StyleSheet::find(const QString &name) const
{
    const auto it = m_styles.constFind(name);
    return it == m_styles.cend() ? nullptr : &*it;
}

const NamedStyle *StyleSheet::findByOdfName(const QString &odfName) const
{
    for (const NamedStyle &s : m_styles)
        if (s.odfName == odfName)
            return &s;
    return nullptr;
}

QStringList StyleSheet::names(Family family) const
{
    QStringList out;
    for (const QString &n : m_order)
        if (m_styles.value(n).family == family)
            out << n;
    return out;
}

void StyleSheet::put(const NamedStyle &s)
{
    NamedStyle st = s;
    if (st.odfName.isEmpty()) {
        st.odfName = odfEncodeName(st.name);
        if (st.odfName != st.name)
            st.odfDisplayName = st.name;
    }
    if (!m_styles.contains(st.name))
        m_order << st.name;
    m_styles.insert(st.name, st);
    m_blockCache.clear();
    m_charCache.clear();
}

void StyleSheet::remove(const QString &name)
{
    if (m_styles.remove(name)) {
        m_order.removeAll(name);
        m_blockCache.clear();
        m_charCache.clear();
    }
}

QString StyleSheet::validate(const NamedStyle &s) const
{
    if (s.name.trimmed().isEmpty())
        return QStringLiteral("a style needs a name");
    if (const NamedStyle *old = find(s.name); old && old->family != s.family)
        return QStringLiteral("\"%1\" is a %2 style").arg(s.name, old->family == Family::Paragraph
                                                             ? QStringLiteral("paragraph") : QStringLiteral("character"));
    if (s.name == defaultParagraphStyle() && !s.parent.isEmpty())
        return QStringLiteral("Default Paragraph Style has no parent");
    if (!s.parent.isEmpty()) {
        const NamedStyle *p = find(s.parent);
        if (!p)
            return QStringLiteral("unknown parent style \"%1\"").arg(s.parent);
        if (p->family != s.family)
            return QStringLiteral("parent \"%1\" is not of the same family").arg(s.parent);
        QString cur = s.parent;
        for (int depth = 0; !cur.isEmpty() && depth < 64; ++depth) {
            if (cur == s.name)
                return QStringLiteral("\"%1\" would inherit from itself").arg(s.name);
            const NamedStyle *c = find(cur);
            cur = c ? c->parent : QString();
        }
    }
    if (!s.next.isEmpty()) {
        if (s.family != Family::Paragraph)
            return QStringLiteral("only paragraph styles have a next style");
        const NamedStyle *n = find(s.next);
        if (s.next != s.name && (!n || n->family != Family::Paragraph))
            return QStringLiteral("unknown next style \"%1\"").arg(s.next);
    }
    return QString();
}

QVariantMap StyleSheet::resolved(const QString &name, bool skipDocumentFont) const
{
    // leaf → root, then merged root-first so the nearer definitions win.
    QList<const NamedStyle *> chain;
    QSet<QString> seen;
    QString cur = name;
    const NamedStyle *s = find(cur);
    if (!s)
        return QVariantMap();
    while (s && !seen.contains(s->name) && chain.size() < 64) {
        chain << s;
        seen.insert(s->name);
        QString parent = s->parent;
        if (parent.isEmpty() && s->family == Family::Paragraph && s->name != defaultParagraphStyle())
            parent = defaultParagraphStyle();
        s = parent.isEmpty() ? nullptr : find(parent);
    }
    QVariantMap out;
    for (auto it = chain.crbegin(); it != chain.crend(); ++it) {
        const bool documentDefault = skipDocumentFont && (*it)->name == defaultParagraphStyle();
        for (auto p = (*it)->props.cbegin(); p != (*it)->props.cend(); ++p) {
            if (documentDefault && (p.key() == QLatin1String("fontFamily") || p.key() == QLatin1String("fontSize")))
                continue;
            out.insert(p.key(), p.value());
        }
    }
    return out;
}

bool StyleSheet::inherits(const QString &name, const QString &ancestor) const
{
    const NamedStyle *s = find(name);
    if (!s)
        return false;
    if (s->family == Family::Paragraph && ancestor == defaultParagraphStyle())
        return true;
    QString cur = name;
    for (int depth = 0; !cur.isEmpty() && depth < 64; ++depth) {
        if (cur == ancestor)
            return true;
        const NamedStyle *c = find(cur);
        cur = c ? c->parent : QString();
    }
    return false;
}

QTextBlockFormat StyleSheet::blockFormat(const QString &paraStyle) const
{
    if (auto it = m_blockCache.constFind(paraStyle); it != m_blockCache.cend())
        return *it;
    const NamedStyle *s = find(paraStyle);
    const QVariantMap props = resolved(s && s->family == Family::Paragraph ? paraStyle : defaultParagraphStyle());
    QTextBlockFormat f;
    applyProps(f, props, true);
    ensureMinSpacing(f);
    f.setProperty(kParagraphStyleProperty, paraStyle);
    return *m_blockCache.insert(paraStyle, f);
}

QTextCharFormat StyleSheet::charFormat(const QString &paraStyle, const QString &charStyle) const
{
    const QString key = paraStyle + QChar(0x1f) + charStyle;
    if (auto it = m_charCache.constFind(key); it != m_charCache.cend())
        return *it;
    const NamedStyle *ps = find(paraStyle);
    QVariantMap props = resolved(ps && ps->family == Family::Paragraph ? paraStyle : defaultParagraphStyle(), !m_useDocumentFont);
    const NamedStyle *cs = charStyle.isEmpty() ? nullptr : find(charStyle);
    if (cs && cs->family == Family::Character) {
        const QVariantMap c = resolved(charStyle);
        for (auto it = c.cbegin(); it != c.cend(); ++it)
            props.insert(it.key(), it.value());
    }
    QTextCharFormat f;
    applyProps(f, props, false);
    if (!charStyle.isEmpty())
        f.setProperty(kCharStyleProperty, charStyle);
    return *m_charCache.insert(key, f);
}

// ─── formats ───────────────────────────────────────────────────────────────────────────────────

QString paragraphStyleOf(const QTextBlockFormat &f)
{
    const QString s = f.stringProperty(kParagraphStyleProperty);
    if (!s.isEmpty())
        return s;
    const int level = f.headingLevel();
    if (level >= 1 && level <= 6)
        return QStringLiteral("Heading %1").arg(level);
    return defaultParagraphStyle();
}

QString charStyleOf(const QTextCharFormat &f)
{
    return f.stringProperty(kCharStyleProperty);
}

QVariant readProp(const QTextFormat &f, Prop p)
{
    switch (p) {
    case Prop::Language: return f.stringProperty(kLanguageProperty);
    case Prop::FontFamily:
        if (f.hasProperty(QTextFormat::FontFamilies)) {
            const QStringList l = f.property(QTextFormat::FontFamilies).toStringList();
            return l.isEmpty() ? QVariant() : QVariant(l.first());
        }
        return f.hasProperty(QTextFormat::FontFamily) ? QVariant(f.stringProperty(QTextFormat::FontFamily)) : QVariant();
    case Prop::FontSize:
        return f.hasProperty(QTextFormat::FontPointSize) ? QVariant(f.doubleProperty(QTextFormat::FontPointSize)) : QVariant();
    case Prop::FontWeight:
        return f.hasProperty(QTextFormat::FontWeight) ? f.intProperty(QTextFormat::FontWeight) : 400;
    case Prop::Italic: return f.boolProperty(QTextFormat::FontItalic);
    case Prop::Underline:
        if (f.hasProperty(QTextFormat::TextUnderlineStyle))
            return f.intProperty(QTextFormat::TextUnderlineStyle) != QTextCharFormat::NoUnderline;
        return f.boolProperty(QTextFormat::FontUnderline);
    case Prop::Strike: return f.boolProperty(QTextFormat::FontStrikeOut);
    case Prop::Color: {
        if (!f.hasProperty(QTextFormat::ForegroundBrush))
            return QVariant();
        const QBrush b = f.brushProperty(QTextFormat::ForegroundBrush);
        return b.style() == Qt::NoBrush ? QVariant() : QVariant(b.color().name(QColor::HexRgb));
    }
    case Prop::LetterSpacing:
        if (f.hasProperty(QTextFormat::FontLetterSpacing)
            && f.intProperty(QTextFormat::FontLetterSpacingType) == QFont::AbsoluteSpacing)
            return f.doubleProperty(QTextFormat::FontLetterSpacing);
        return 0.0;
    case Prop::MarginTop: case Prop::MarginBottom: {
        const qreal v = f.doubleProperty(p == Prop::MarginTop ? QTextFormat::BlockTopMargin : QTextFormat::BlockBottomMargin);
        return qAbs(v - kMinParagraphSpacing) < 1e-6 ? 0.0 : v;
    }
    case Prop::MarginLeft: return f.doubleProperty(QTextFormat::BlockLeftMargin);
    case Prop::MarginRight: return f.doubleProperty(QTextFormat::BlockRightMargin);
    case Prop::TextIndent: return f.doubleProperty(QTextFormat::TextIndent);
    case Prop::LineHeight:
        switch (f.intProperty(QTextFormat::LineHeightType)) {
        case QTextBlockFormat::ProportionalHeight: return f.doubleProperty(QTextFormat::LineHeight) / 100.0;
        case QTextBlockFormat::FixedHeight: return pxString(f.doubleProperty(QTextFormat::LineHeight));
        default: return 1.0;
        }
    case Prop::Alignment: {
        const int a = f.intProperty(QTextFormat::BlockAlignment);
        if (a & Qt::AlignHCenter) return QStringLiteral("center");
        if (a & Qt::AlignJustify) return QStringLiteral("justify");
        if (a & (Qt::AlignRight | Qt::AlignTrailing)) return QStringLiteral("right");
        return QStringLiteral("left");
    }
    case Prop::OutlineLevel: return f.intProperty(QTextFormat::HeadingLevel);
    case Prop::KeepWithNext: return f.boolProperty(kKeepWithNextProperty);
    }
    return QVariant();
}

QVariantMap directProps(const QTextFormat &actual, const QTextFormat &base, bool blockProps)
{
    QVariantMap out;
    for (int i = 0; i < kPropCount; ++i) {
        const Prop p = Prop(i);
        if (isCharProp(p) == blockProps)
            continue;
        const QVariant a = readProp(actual, p);
        if (!sameValue(a, readProp(base, p)))
            out.insert(QLatin1String(propName(p)), a.isValid() ? a : QVariant::fromValue(nullptr));
    }
    return out;
}

void applyProps(QTextFormat &f, const QVariantMap &props, bool blockProps)
{
    for (auto it = props.cbegin(); it != props.cend(); ++it) {
        Prop p;
        if (!propFromName(it.key(), &p) || isCharProp(p) == blockProps)
            continue;
        writeValue(f, p, it.value());
    }
}

void rebase(QTextFormat &f, const QTextFormat &oldBase, const QTextFormat &newBase, bool blockProps, bool force)
{
    for (int i = 0; i < kPropCount; ++i) {
        const Prop p = Prop(i);
        if (isCharProp(p) == blockProps)
            continue;
        if (force || sameValue(readProp(f, p), readProp(oldBase, p)))
            copyRaw(f, newBase, p);
    }
    if (blockProps && f.doubleProperty(QTextFormat::BlockTopMargin) <= 0
        && f.doubleProperty(QTextFormat::BlockBottomMargin) <= 0)
        f.setProperty(QTextFormat::BlockBottomMargin, kMinParagraphSpacing);
}

void ensureMinSpacing(QTextBlockFormat &f)
{
    if (f.topMargin() <= 0 && f.bottomMargin() <= 0)
        f.setBottomMargin(kMinParagraphSpacing);
}

QTextBlockFormat scaledLengths(const QTextBlockFormat &f, qreal ratio)
{
    QTextBlockFormat out = f;
    if (qFuzzyCompare(ratio, 1.0))
        return out;
    for (int id : { int(QTextFormat::BlockTopMargin), int(QTextFormat::BlockBottomMargin),
                    int(QTextFormat::BlockLeftMargin), int(QTextFormat::BlockRightMargin),
                    int(QTextFormat::TextIndent) })
        if (out.hasProperty(id))
            out.setProperty(id, out.property(id).toDouble() * ratio);
    if (out.hasProperty(QTextFormat::LineHeight)
        && out.intProperty(QTextFormat::LineHeightType) == QTextBlockFormat::FixedHeight)
        out.setProperty(QTextFormat::LineHeight, out.doubleProperty(QTextFormat::LineHeight) * ratio);
    return out;
}

QTextCharFormat scaledLengths(const QTextCharFormat &f, qreal ratio)
{
    QTextCharFormat out = f;
    if (qFuzzyCompare(ratio, 1.0))
        return out;
    if (out.hasProperty(QTextFormat::FontPointSize))
        out.setProperty(QTextFormat::FontPointSize, out.doubleProperty(QTextFormat::FontPointSize) * ratio);
    if (out.hasProperty(QTextFormat::FontLetterSpacing)
        && out.intProperty(QTextFormat::FontLetterSpacingType) == QFont::AbsoluteSpacing)
        out.setProperty(QTextFormat::FontLetterSpacing, out.doubleProperty(QTextFormat::FontLetterSpacing) * ratio);
    // Inline images (QTextImageFormat is a QTextCharFormat): the explicit display size we set in
    // insertImage, scaled the same way as everything else baked into the live document.
    if (out.hasProperty(QTextFormat::ImageWidth))
        out.setProperty(QTextFormat::ImageWidth, out.doubleProperty(QTextFormat::ImageWidth) * ratio);
    if (out.hasProperty(QTextFormat::ImageHeight))
        out.setProperty(QTextFormat::ImageHeight, out.doubleProperty(QTextFormat::ImageHeight) * ratio);
    return out;
}

QString odfEncodeName(const QString &name)
{
    QString out;
    out.reserve(name.size() + 8);
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (isNameChar(c, i == 0))
            out += c;
        else
            out += QLatin1Char('_') + QString::number(c.unicode(), 16) + QLatin1Char('_');
    }
    return out;
}

QString odfDecodeName(const QString &odfName)
{
    QString out;
    out.reserve(odfName.size());
    for (int i = 0; i < odfName.size(); ++i) {
        if (odfName.at(i) == QLatin1Char('_')) {
            const int end = odfName.indexOf(QLatin1Char('_'), i + 1);
            if (end > i + 1 && end - i - 1 <= 4) {
                bool ok = false;
                const uint code = QStringView(odfName).mid(i + 1, end - i - 1).toUInt(&ok, 16);
                if (ok && code >= 0x20) {
                    out += QChar(char16_t(code));
                    i = end;
                    continue;
                }
            }
        }
        out += odfName.at(i);
    }
    return out;
}

QTextTableFormat wordLikeTableFormat()
{
    QTextTableFormat f;
    f.setBorder(1);
    f.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    f.setBorderCollapse(true);
    f.setCellPadding(4);
    f.setCellSpacing(0);
    f.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    return f;
}

} // namespace rt
