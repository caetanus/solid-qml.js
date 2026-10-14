#include "richtext_odf.h"
#include "richtext_blocks.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <private/qzipreader_p.h> // Qt ships no public (un)zip; these ARE how Qt reads/writes .odt
#include <private/qzipwriter_p.h>

namespace rt {

namespace {

const QString kStandard = QStringLiteral("Standard"); // ODF name of Default Paragraph Style
constexpr qreal kDefaultFontPt = 12.0;                // ODF's base font size when a file names none

QString localPath(const QString &nameOrUrl)
{
    const QUrl u(nameOrUrl);
    return u.isLocalFile() ? u.toLocalFile() : nameOrUrl;
}

// ═══ reader ═════════════════════════════════════════════════════════════════════════════════════

struct RawStyle {
    QString family;       // "paragraph" | "text" | other (ignored)
    QString display;      // style:display-name
    QString parent;       // style:parent-style-name (ODF name)
    QString next;         // style:next-style-name (ODF name)
    int cellAlignment = -1; // Absent until a table-cell style specifies it.
    int outline = 0;      // style:default-outline-level
    QVariantMap props;    // canonical; fontSize may still be a "N%" string (relative to the parent)
};

struct OdfStyles {
    QHash<QString, RawStyle> styles; // by style:name
    QStringList order;               // file order
    RawStyle defaultParagraph;       // <style:default-style style:family="paragraph">
    QHash<QString, QString> fonts;   // style:font-face name → font family
    QHash<QString, QTextListFormat::Style> lists; // text:list-style name → its level-1 kind
};

// Resolve the first body paragraph's master through its style chain. Reading both XML parts
// also handles automatic paragraph styles and avoids choosing an unrelated first page layout.
QVariantList documentGeometry(const QByteArray &styles, const QByteArray &content)
{
    QHash<QString, QVariantList> layouts;
    QHash<QString, QString> masters, parents, styleMasters;
    QString firstMaster, firstParagraph;
    bool foundParagraph = false;
    for (const auto &part : {styles, content}) {
        QXmlStreamReader xml(part);
        QString layout;
        bool body = false;
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::EndElement && xml.name() == QLatin1String("page-layout")) layout.clear();
            if (token != QXmlStreamReader::StartElement) continue;
            const auto a = xml.attributes();
            const auto val = [&](const char *name) { return a.value(QLatin1String(name)).toString(); };
            if (xml.name() == QLatin1String("body")) body = true;
            if (body && !foundParagraph && (xml.name() == QLatin1String("p") || xml.name() == QLatin1String("h"))) {
                firstParagraph = val("text:style-name");
                foundParagraph = true;
            }
            if (xml.name() == QLatin1String("style")) {
                const QString name = val("style:name");
                parents.insert(name, val("style:parent-style-name"));
                styleMasters.insert(name, val("style:master-page-name"));
            } else if (xml.name() == QLatin1String("master-page")) {
                const QString name = val("style:name");
                if (firstMaster.isEmpty()) firstMaster = name;
                masters.insert(name, val("style:page-layout-name"));
            } else if (xml.name() == QLatin1String("page-layout")) {
                layout = val("style:name");
            } else if (!layout.isEmpty() && xml.name() == QLatin1String("page-layout-properties")) {
                const qreal w = lengthPx(val("fo:page-width")), h = lengthPx(val("fo:page-height"));
                if (!(w > 0 && h > 0)) continue;
                qreal all = lengthPx(val("fo:margin"));
                if (qIsNaN(all)) all = 0;
                QVariantList geometry{w,h};
                for (const char *side : {"fo:margin-top", "fo:margin-right", "fo:margin-bottom", "fo:margin-left"}) {
                    const qreal margin = lengthPx(val(side));
                    geometry.append(qIsNaN(margin) ? all : margin);
                }
                layouts.insert(layout, geometry);
            }
        }
    }
    QString master;
    QSet<QString> seen;
    for (QString style = firstParagraph; !style.isEmpty() && !seen.contains(style); style = parents.value(style)) {
        seen.insert(style);
        master = styleMasters.value(style);
        if (!master.isEmpty()) break;
    }
    if (master.isEmpty()) master = firstMaster;
    return layouts.value(masters.value(master));
}

QString firstFamily(QStringView v)
{
    const qsizetype comma = v.indexOf(QLatin1Char(','));
    QStringView f = (comma < 0 ? v : v.left(comma)).trimmed();
    if (f.size() >= 2 && (f.front() == QLatin1Char('\'') || f.front() == QLatin1Char('"')) && f.back() == f.front())
        f = f.mid(1, f.size() - 2);
    return f.toString();
}

void putProp(QVariantMap &props, Prop p, const QVariant &raw)
{
    QVariant v;
    if (canonicalValue(p, raw, &v, nullptr))
        props.insert(QLatin1String(propName(p)), v);
}

void readTextProps(const QXmlStreamAttributes &a, const QHash<QString, QString> &fonts, QVariantMap &props)
{
    const auto val = [&](const char *n) { return a.value(QLatin1String(n)); };
    if (const auto fn = val("style:font-name"); !fn.isEmpty())
        props.insert(QStringLiteral("fontFamily"), fonts.value(fn.toString(), fn.toString()));
    else if (const auto ff = val("fo:font-family"); !ff.isEmpty())
        putProp(props, Prop::FontFamily, firstFamily(ff));
    if (const auto size = val("fo:font-size"); !size.isEmpty()) {
        if (size.endsWith(QLatin1Char('%'))) {
            props.insert(QStringLiteral("fontSize"), size.toString()); // resolved against the parent later
        } else {
            const qreal px = lengthPx(size);
            if (!qIsNaN(px) && px > 0)
                props.insert(QStringLiteral("fontSize"), px * 0.75);
        }
    }
    if (const auto w = val("fo:font-weight"); !w.isEmpty())
        putProp(props, Prop::FontWeight, w.toString());
    if (const auto st = val("fo:font-style"); !st.isEmpty())
        props.insert(QStringLiteral("italic"), st == QLatin1String("italic") || st == QLatin1String("oblique"));
    if (const auto u = val("style:text-underline-style"); !u.isEmpty())
        props.insert(QStringLiteral("underline"), u != QLatin1String("none"));
    if (const auto s = val("style:text-line-through-style"); !s.isEmpty())
        props.insert(QStringLiteral("strike"), s != QLatin1String("none"));
    if (const auto c = val("fo:color"); !c.isEmpty())
        putProp(props, Prop::Color, c.toString());
    if (const auto ls = val("fo:letter-spacing"); !ls.isEmpty())
        putProp(props, Prop::LetterSpacing, ls.toString());
    if (const auto language = val("fo:language"); !language.isEmpty()) {
        QString tag = language.toString();
        const auto country = val("fo:country");
        if (!country.isEmpty() && country != QLatin1String("none")) tag += QLatin1Char('-') + country.toString();
        putProp(props, Prop::Language, tag);
    }
}

void readParagraphProps(const QXmlStreamAttributes &a, QVariantMap &props)
{
    const auto val = [&](const char *n) { return a.value(QLatin1String(n)); };
    const auto length = [&](const char *attr, const char *prop) {
        const qreal px = lengthPx(val(attr));
        if (!qIsNaN(px))
            props.insert(QLatin1String(prop), px);
    };
    const qreal all = lengthPx(val("fo:margin")); // the shorthand the four sides start from
    if (!qIsNaN(all))
        for (const char *p : { "marginTop", "marginBottom", "marginLeft", "marginRight" })
            props.insert(QLatin1String(p), all);
    length("fo:margin-top", "marginTop");
    length("fo:margin-bottom", "marginBottom");
    length("fo:margin-left", "marginLeft");
    length("fo:margin-right", "marginRight");
    length("fo:text-indent", "textIndent");
    if (const auto lh = val("fo:line-height"); !lh.isEmpty())
        putProp(props, Prop::LineHeight, lh.toString());
    if (const auto al = val("fo:text-align"); !al.isEmpty())
        putProp(props, Prop::Alignment, al.toString());
    if (const auto k = val("fo:keep-with-next"); !k.isEmpty())
        props.insert(QStringLiteral("keepWithNext"), k == QLatin1String("always"));
    // insertPageBreak() (item 5): not a rt::Prop — see the matching writer side (writeProps()).
    if (val("fo:break-before") == QLatin1String("page"))
        props.insert(QStringLiteral("__pageBreakBefore"), true);
}

QTextListFormat::Style listKind(const QXmlStreamReader &xml)
{
    if (xml.name() == QLatin1String("list-level-style-number")) {
        const auto f = xml.attributes().value(QLatin1String("style:num-format"));
        if (f == QLatin1String("a")) return QTextListFormat::ListLowerAlpha;
        if (f == QLatin1String("A")) return QTextListFormat::ListUpperAlpha;
        if (f == QLatin1String("i")) return QTextListFormat::ListLowerRoman;
        if (f == QLatin1String("I")) return QTextListFormat::ListUpperRoman;
        return QTextListFormat::ListDecimal;
    }
    const auto ch = xml.attributes().value(QLatin1String("text:bullet-char"));
    if (ch == QStringLiteral("◦") || ch == QStringLiteral("○")) return QTextListFormat::ListCircle;
    if (ch == QStringLiteral("▪") || ch == QStringLiteral("■")) return QTextListFormat::ListSquare;
    return QTextListFormat::ListDisc;
}

// style:style / style:default-style / text:list-style / style:font-face elements up to the end of
// `section` (office:styles, office:automatic-styles, office:font-face-decls).
void readStyles(QXmlStreamReader &xml, QLatin1String section, OdfStyles &out)
{
    QString current;          // style:name being read
    bool inDefault = false;   // inside the paragraph default-style
    QString currentList;
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::EndElement) {
            if (xml.name() == section)
                break;
            if (xml.name() == QLatin1String("style") || xml.name() == QLatin1String("default-style")) {
                current.clear();
                inDefault = false;
            } else if (xml.name() == QLatin1String("list-style")) {
                currentList.clear();
            }
            continue;
        }
        if (tok != QXmlStreamReader::StartElement)
            continue;
        const auto a = xml.attributes();
        const auto name = xml.name();
        if (name == QLatin1String("font-face")) {
            out.fonts.insert(a.value(QLatin1String("style:name")).toString(),
                             firstFamily(a.value(QLatin1String("svg:font-family"))));
        } else if (name == QLatin1String("style")) {
            current = a.value(QLatin1String("style:name")).toString();
            inDefault = false;
            if (current.isEmpty())
                continue;
            if (!out.styles.contains(current))
                out.order << current;
            RawStyle &s = out.styles[current];
            s.family = a.value(QLatin1String("style:family")).toString();
            s.display = a.value(QLatin1String("style:display-name")).toString();
            s.parent = a.value(QLatin1String("style:parent-style-name")).toString();
            s.next = a.value(QLatin1String("style:next-style-name")).toString();
            s.outline = a.value(QLatin1String("style:default-outline-level")).toInt();
        } else if (name == QLatin1String("default-style")) {
            current.clear();
            inDefault = a.value(QLatin1String("style:family")) == QLatin1String("paragraph");
        } else if (name == QLatin1String("list-style")) {
            current.clear();
            inDefault = false;
            currentList = a.value(QLatin1String("style:name")).toString();
        } else if (!currentList.isEmpty()
                   && (name == QLatin1String("list-level-style-number") || name == QLatin1String("list-level-style-bullet"))) {
            const auto level = a.value(QLatin1String("text:level"));
            if ((level.isEmpty() || level == QLatin1String("1")) && !out.lists.contains(currentList))
                out.lists.insert(currentList, listKind(xml));
        } else if (name == QLatin1String("table-cell-properties") && !current.isEmpty()) {
            const auto alignment=a.value(QLatin1String("style:vertical-align"));
            if (alignment==QLatin1String("middle")) out.styles[current].cellAlignment=QTextCharFormat::AlignMiddle;
            else if (alignment==QLatin1String("bottom")) out.styles[current].cellAlignment=QTextCharFormat::AlignBottom;
            else if (alignment==QLatin1String("top")) out.styles[current].cellAlignment=QTextCharFormat::AlignTop;
        } else if (name == QLatin1String("text-properties") && (inDefault || !current.isEmpty())) {
            readTextProps(a, out.fonts, inDefault ? out.defaultParagraph.props : out.styles[current].props);
        } else if (name == QLatin1String("paragraph-properties") && (inDefault || !current.isEmpty())) {
            readParagraphProps(a, inDefault ? out.defaultParagraph.props : out.styles[current].props);
        }
    }
}

// A "N%" font size → points, against `base`.
void resolveRelativeSize(QVariantMap &props, qreal base)
{
    const auto it = props.find(QStringLiteral("fontSize"));
    if (it == props.end() || it->typeId() != QMetaType::QString)
        return;
    const qreal pct = it->toString().chopped(1).toDouble();
    if (pct > 0) *it = base * pct / 100.0;
    else props.erase(it);
}

// The file's named styles (office:styles) on top of the built-in ones. A built-in the file
// defines (matched by ODF name: Standard, Text_20_body, Heading_20_1, …) takes the file's
// definition; the others stay as built in.
class SheetBuilder {
public:
    SheetBuilder(OdfStyles &named, StyleSheet &sheet) : m_named(named), m_sheet(sheet) {}

    void build()
    {
        // The paragraph default-style IS Default Paragraph Style's base: merge it into Standard.
        if (!m_named.defaultParagraph.props.isEmpty() || m_named.styles.contains(kStandard)) {
            if (!m_named.styles.contains(kStandard)) {
                m_named.order.prepend(kStandard);
                m_named.styles[kStandard].family = QStringLiteral("paragraph");
            }
            RawStyle &st = m_named.styles[kStandard];
            for (auto it = m_named.defaultParagraph.props.cbegin(); it != m_named.defaultParagraph.props.cend(); ++it)
                if (!st.props.contains(it.key()))
                    st.props.insert(it.key(), it.value());
        }
        // API names first (parents and next styles can come later in the file).
        QSet<QString> taken;
        for (const QString &n : m_sheet.order())
            taken.insert(n);
        for (const QString &odf : std::as_const(m_named.order)) {
            const RawStyle &r = m_named.styles[odf];
            const Family fam = r.family == QLatin1String("text") ? Family::Character : Family::Paragraph;
            if (r.family != QLatin1String("paragraph") && r.family != QLatin1String("text"))
                continue;
            QString api;
            if (const NamedStyle *b = m_sheet.findByOdfName(odf); b && b->family == fam) {
                api = b->name;
            } else {
                api = r.display.isEmpty() ? odfDecodeName(odf) : r.display;
                if (taken.contains(api))
                    api = odf;
                if (taken.contains(api))
                    continue; // a duplicate ODF name: keep the first
                taken.insert(api);
            }
            m_api.insert(odf, api);
        }
        for (const QString &odf : std::as_const(m_named.order)) {
            if (!m_api.contains(odf))
                continue;
            const RawStyle &r = m_named.styles[odf];
            NamedStyle s;
            s.name = m_api.value(odf);
            s.family = r.family == QLatin1String("text") ? Family::Character : Family::Paragraph;
            const QString parent = r.parent == odf ? QString() : apiFor(r.parent);
            const NamedStyle *ps = parent.isEmpty() ? nullptr : m_sheet.find(parent);
            const bool parentOk = !parent.isEmpty() && (m_api.contains(r.parent) || (ps && ps->family == s.family));
            s.parent = s.name == defaultParagraphStyle() || !parentOk ? QString() : parent;
            if (s.family == Family::Paragraph)
                s.next = apiFor(r.next);
            s.props = r.props;
            if (s.props.contains(QStringLiteral("fontSize")))
                s.props.insert(QStringLiteral("fontSize"), absoluteSize(odf, 0));
            if (r.outline > 0)
                s.props.insert(QStringLiteral("outlineLevel"), r.outline);
            s.odfName = odf;
            const NamedStyle *b = m_sheet.findByOdfName(odf);
            s.odfDisplayName = b && b->name == s.name ? b->odfDisplayName : r.display;
            m_sheet.put(s);
        }
    }

    // ODF name → API name (the file's styles, then the built-ins); "" when unknown.
    QString apiFor(const QString &odf) const
    {
        if (odf.isEmpty())
            return QString();
        if (auto it = m_api.constFind(odf); it != m_api.cend())
            return *it;
        const NamedStyle *b = m_sheet.findByOdfName(odf);
        return b ? b->name : QString();
    }

private:
    // A style's font size in points along its chain (relative sizes resolved), for "N%" values.
    qreal absoluteSize(const QString &odf, int depth)
    {
        if (auto it = m_sizes.constFind(odf); it != m_sizes.cend())
            return *it;
        const auto it = m_named.styles.constFind(odf);
        qreal base = kDefaultFontPt;
        if (it == m_named.styles.cend() || depth > 32) {
            const QVariant d = m_named.defaultParagraph.props.value(QStringLiteral("fontSize"));
            return isAbsolute(d) ? d.toDouble() : kDefaultFontPt;
        }
        const QString parent = it->parent.isEmpty() && odf != kStandard && it->family == QLatin1String("paragraph")
            ? kStandard : it->parent;
        if (!parent.isEmpty() && parent != odf)
            base = absoluteSize(parent, depth + 1);
        else if (const QVariant d = m_named.defaultParagraph.props.value(QStringLiteral("fontSize")); isAbsolute(d))
            base = d.toDouble();
        const QVariant v = it->props.value(QStringLiteral("fontSize"));
        qreal size = base;
        if (v.typeId() == QMetaType::QString) {
            const qreal pct = v.toString().chopped(1).toDouble();
            if (pct > 0) size = base * pct / 100.0;
        } else if (v.isValid()) {
            size = v.toDouble();
        }
        m_sizes.insert(odf, size);
        return size;
    }
    static bool isAbsolute(const QVariant &v) { return v.isValid() && v.typeId() != QMetaType::QString; }

    OdfStyles &m_named;
    StyleSheet &m_sheet;
    QHash<QString, QString> m_api;
    QHash<QString, qreal> m_sizes;
};

} // namespace

bool readOdt(const QString &path, QTextDocument *doc, StyleSheet *sheetOut, QString *error)
{
    if (!QFile::exists(path)) {
        *error = QStringLiteral("file not found: ") + path;
        return false;
    }
    QZipReader zip(path);
    const QByteArray content = zip.fileData(QStringLiteral("content.xml"));
    if (content.isEmpty()) {
        *error = QStringLiteral("not an OpenDocument file: ") + path;
        return false;
    }

    // Named styles (styles.xml office:styles) → the style sheet.
    OdfStyles named;
    {
        QXmlStreamReader sx(zip.fileData(QStringLiteral("styles.xml")));
        while (!sx.atEnd()) {
            if (sx.readNext() != QXmlStreamReader::StartElement)
                continue;
            if (sx.name() == QLatin1String("font-face-decls"))
                readStyles(sx, QLatin1String("font-face-decls"), named);
            else if (sx.name() == QLatin1String("styles"))
                readStyles(sx, QLatin1String("styles"), named);
        }
    }
    StyleSheet sheet = StyleSheet::builtIns();
    SheetBuilder builder(named, sheet);
    builder.build();
    // Keep the imported base font in the style model, including restyling and saving.
    sheet.useDocumentFont();

    // content.xml's automatic styles: direct formatting over a named style.
    OdfStyles autos;
    autos.fonts = named.fonts;
    autos.lists = named.lists;

    struct Prepared {
        QString style;             // the named paragraph style
        QTextBlockFormat block;    // materialized + direct paragraph properties
        QTextCharFormat chars;     // the paragraph's text: style + direct character properties
        QVariantMap charDirect;
        int outline = 0;
    };
    QHash<QString, Prepared> prepared;
    const auto paragraph = [&](const QString &odf) -> const Prepared & {
        if (auto it = prepared.constFind(odf); it != prepared.cend())
            return *it;
        Prepared p;
        QVariantMap direct;
        const auto isParagraphStyle = [&](const QString &n) {
            const NamedStyle *s = n.isEmpty() ? nullptr : sheet.find(n);
            return s && s->family == Family::Paragraph;
        };
        p.style = builder.apiFor(odf);
        if (!isParagraphStyle(p.style)) {
            p.style.clear();
            if (auto a = autos.styles.constFind(odf); a != autos.styles.cend() && a->family == QLatin1String("paragraph")) {
                p.style = builder.apiFor(a->parent);
                direct = a->props;
                p.outline = a->outline;
            }
            if (!isParagraphStyle(p.style))
                p.style = defaultParagraphStyle();
            const QVariant base = sheet.resolved(p.style).value(QStringLiteral("fontSize"));
            resolveRelativeSize(direct, base.isValid() ? base.toDouble() : kDefaultFontPt);
        }
        p.block = sheet.blockFormat(p.style);
        if (!direct.isEmpty()) {
            applyProps(p.block, direct, true); // ignores __pageBreakBefore: not a rt::Prop
            ensureMinSpacing(p.block);
            if (direct.value(QStringLiteral("__pageBreakBefore")).toBool())
                p.block.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);
            for (auto it = direct.cbegin(); it != direct.cend(); ++it) {
                Prop pr;
                if (propFromName(it.key(), &pr) && isCharProp(pr))
                    p.charDirect.insert(it.key(), it.value());
            }
        }
        p.chars = sheet.charFormat(p.style, QString());
        applyProps(p.chars, p.charDirect, false);
        return *prepared.insert(odf, p);
    };

    struct SpanStyle { QString charStyle; bool named = false; QVariantMap direct; };
    QHash<QString, SpanStyle> spanStyles;
    const auto spanStyle = [&](const QString &odf) -> const SpanStyle & {
        if (auto it = spanStyles.constFind(odf); it != spanStyles.cend())
            return *it;
        SpanStyle s;
        const auto isCharStyle = [&](const QString &n) {
            const NamedStyle *st = n.isEmpty() ? nullptr : sheet.find(n);
            return st && st->family == Family::Character;
        };
        const QString api = builder.apiFor(odf);
        if (isCharStyle(api)) {
            s.charStyle = api;
            s.named = true;
        } else if (auto a = autos.styles.constFind(odf); a != autos.styles.cend()) {
            const QString parent = builder.apiFor(a->parent);
            if (isCharStyle(parent)) {
                s.charStyle = parent;
                s.named = true;
            }
            s.direct = a->props;
            resolveRelativeSize(s.direct, kDefaultFontPt);
        }
        return *spanStyles.insert(odf, s);
    };

    // What text is written in: a paragraph's base, then one entry per open span.
    struct Run {
        QString paraStyle;
        QString charStyle;
        QVariantMap direct;
        QTextCharFormat format;
        bool paragraph = false;
    };
    QList<Run> runs;

    QXmlStreamReader xml(content);

    // Opening a file is not an undoable edit: with undo off, clear() + the build leave no history
    // (re-enabling it starts an empty stack), so Ctrl+Z can't empty the freshly opened document.
    doc->setUndoRedoEnabled(false);
    doc->clear();
    auto rootFormat = doc->rootFrame()->frameFormat();
    rootFormat.setProperty(kDocumentGeometryProperty, documentGeometry(zip.fileData(QStringLiteral("styles.xml")), content));
    doc->rootFrame()->setFrameFormat(rootFormat);
    QTextCursor c(doc);
    c.beginEditBlock();
    bool firstBlock = true;
    int paragraphDepth = 0; // text:p inside text:note bodies nest

    // Lists: one QTextList per text:list (created on its first item), nested ones indented.
    struct ListLevel { QTextList *list = nullptr; QTextListFormat::Style style = QTextListFormat::ListDisc; };
    QList<ListLevel> lists;

    // Table state (word-like subset: flat tables, spans/images inside cells). Column count comes
    // from the table:table-column declarations that precede the rows; the table is created on the
    // first row and grown with appendRows so the streaming parse never needs a second pass.
    QTextTable *curTable = nullptr;
    int declaredCols = 0;
    int tRow = -1;
    int tCol = -1;
    QTextCursor cellCursor;
    bool inCell = false;
    int nestedTables = 0;
    bool cellFirstBlock = false;
    int cellAlignment = -1;
    struct Merge { int row, col, rows, cols; };
    QList<Merge> merges;

    // Pending inline image (draw:frame carries the size, the nested draw:image the href; the
    // insert happens when the frame closes so both are known).
    QString imgHref;
    qreal imgW = -1, imgH = -1;

    // The cursor content lands on: the current cell inside a table, the document elsewhere.
    const auto act = [&]() -> QTextCursor & { return inCell ? cellCursor : c; };

    const auto beginBlock = [&](const QTextBlockFormat &bf, const QTextCharFormat &cf) {
        if (inCell) {
            // Every cell is born with one empty block — reuse it for the first paragraph.
            if (cellFirstBlock) {
                cellCursor.setBlockFormat(bf);
                cellCursor.setBlockCharFormat(cf);
                cellFirstBlock = false;
            } else {
                cellCursor.insertBlock(bf, cf);
            }
            return;
        }
        if (firstBlock) {
            c.setBlockFormat(bf);
            c.setBlockCharFormat(cf);
            firstBlock = false;
        } else {
            c.insertBlock(bf, cf);
        }
        if (!lists.isEmpty()) {
            ListLevel &l = lists.last();
            if (!l.list) {
                QTextListFormat lf;
                lf.setStyle(l.style);
                lf.setIndent(int(lists.size()));
                l.list = c.createList(lf);
            } else {
                l.list->add(c.block());
            }
        }
    };

    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const auto name = xml.name();
            const auto attrs = xml.attributes();
            if (name == QLatin1String("font-face-decls")) {
                readStyles(xml, QLatin1String("font-face-decls"), autos);
            } else if (name == QLatin1String("automatic-styles")) {
                readStyles(xml, QLatin1String("automatic-styles"), autos);
                prepared.clear();
                spanStyles.clear();
            } else if (name == QLatin1String("list")) {
                ListLevel l;
                const QString ls = attrs.value(QLatin1String("text:style-name")).toString();
                l.style = autos.lists.value(ls, lists.isEmpty() ? QTextListFormat::ListDisc : lists.last().style);
                lists << l;
            } else if (name == QLatin1String("p") || name == QLatin1String("h")) {
                ++paragraphDepth;
                const Prepared &p = paragraph(attrs.value(QLatin1String("text:style-name")).toString());
                QTextBlockFormat bf = p.block;
                if (name == QLatin1String("h")) {
                    int level = attrs.value(QLatin1String("text:outline-level")).toInt();
                    if (level <= 0) level = bf.headingLevel() > 0 ? bf.headingLevel() : (p.outline > 0 ? p.outline : 1);
                    if (bf.headingLevel() != level)
                        bf.setHeadingLevel(qBound(1, level, 10));
                }
                beginBlock(bf, p.chars);
                // Office item 3: the block's stable id (adopted by the handler after the load;
                // blocks without one — or repeating one — get a generated id there).
                if (const auto xmlId = attrs.value(QLatin1String("xml:id")); !xmlId.isEmpty())
                    act().block().setUserData(new BlockInfo(xmlId.toString()));
                act().setCharFormat(p.chars);
                runs << Run { p.style, QString(), p.charDirect, p.chars, true };
            } else if (name == QLatin1String("span")) {
                if (!runs.isEmpty()) {
                    const Run &outer = runs.last();
                    const SpanStyle &s = spanStyle(attrs.value(QLatin1String("text:style-name")).toString());
                    Run r { outer.paraStyle, s.named ? s.charStyle : outer.charStyle, outer.direct, {}, false };
                    for (auto it = s.direct.cbegin(); it != s.direct.cend(); ++it)
                        r.direct.insert(it.key(), it.value());
                    r.format = sheet.charFormat(r.paraStyle, r.charStyle);
                    applyProps(r.format, r.direct, false);
                    act().setCharFormat(r.format);
                    runs << r;
                }
            } else if (name == QLatin1String("tab")) {
                act().insertText(QStringLiteral("\t"));
            } else if (name == QLatin1String("line-break")) {
                act().insertText(QString(QChar::LineSeparator));
            } else if (name == QLatin1String("s")) {
                const int n = attrs.value(QLatin1String("text:c")).toInt();
                act().insertText(QString(qMax(1, n), QLatin1Char(' ')));
            } else if (name == QLatin1String("table")) {
                // A table inside a cell is not supported: its cells' text flows into the outer cell.
                if (inCell || nestedTables > 0) {
                    ++nestedTables;
                } else {
                    curTable = nullptr;
                    declaredCols = 0;
                    tRow = -1;
                    merges.clear();
                }
            } else if (nestedTables > 0 && name.startsWith(QLatin1String("table-"))) {
                // a nested table's columns/rows/cells: its paragraphs land in the outer cell
            } else if (name == QLatin1String("table-column")) {
                const int rep = attrs.value(QLatin1String("table:number-columns-repeated")).toInt();
                declaredCols += qMax(1, rep);
            } else if (name == QLatin1String("table-row")) {
                if (!curTable) {
                    curTable = c.insertTable(1, qMax(1, declaredCols), wordLikeTableFormat());
                    tRow = 0;
                } else {
                    curTable->appendRows(1);
                    ++tRow;
                }
                tCol = -1;
            } else if (name == QLatin1String("covered-table-cell")) {
                if (curTable)
                    ++tCol; // the slot a spanning cell covers
            } else if (name == QLatin1String("table-cell")) {
                // Only a cell of the current row: a mis-nested file (a cell outside any row) is
                // read as plain paragraphs rather than trusted.
                const QTextTableCell cell = curTable && tRow >= 0 && tRow < curTable->rows()
                    ? curTable->cellAt(tRow, qMin(++tCol, curTable->columns() - 1)) : QTextTableCell();
                if (cell.isValid()) {
                    cellAlignment=-1;
                    QString styleName=attrs.value(QLatin1String("table:style-name")).toString();
                    for (int depth=0;!styleName.isEmpty() && depth<32;++depth) {
                        auto it=autos.styles.constFind(styleName);
                        const RawStyle *style=it==autos.styles.cend() ? nullptr : &*it;
                        if (!style) { auto namedStyle=named.styles.constFind(styleName); if(namedStyle!=named.styles.cend()) style=&*namedStyle; }
                        if (!style || style->family!=QLatin1String("table-cell")) break;
                        if (style->cellAlignment>=0) {
                            cellAlignment=style->cellAlignment; break;
                        }
                        styleName=style->parent;
                    }
                    cellCursor = cell.firstCursorPosition();
                    inCell = true;
                    cellFirstBlock = true;
                    const int cs = attrs.value(QLatin1String("table:number-columns-spanned")).toInt();
                    const int rs = attrs.value(QLatin1String("table:number-rows-spanned")).toInt();
                    if (cs > 1 || rs > 1)
                        merges << Merge { tRow, tCol, qMax(1, rs), qMax(1, cs) };
                }
            } else if (name == QLatin1String("frame")) {
                imgW = lengthPx(attrs.value(QLatin1String("svg:width")));
                imgH = lengthPx(attrs.value(QLatin1String("svg:height")));
                imgHref.clear();
            } else if (name == QLatin1String("image")) {
                imgHref = attrs.value(QLatin1String("xlink:href")).toString();
            }
        } else if (tok == QXmlStreamReader::EndElement) {
            const auto name = xml.name();
            if (name == QLatin1String("list")) {
                if (!lists.isEmpty())
                    lists.removeLast();
            } else if (name == QLatin1String("p") || name == QLatin1String("h")) {
                --paragraphDepth;
                while (!runs.isEmpty() && !runs.takeLast().paragraph) {}
                if (!runs.isEmpty())
                    act().setCharFormat(runs.last().format); // back in the paragraph a note sat in
            } else if (name == QLatin1String("span")) {
                if (runs.size() > 1 && !runs.last().paragraph) {
                    runs.removeLast();
                    act().setCharFormat(runs.last().format);
                }
            } else if (name == QLatin1String("table") && nestedTables > 0) {
                --nestedTables;
            } else if (nestedTables > 0 && name.startsWith(QLatin1String("table-"))) {
                // a nested table's row/cell end: the outer cell stays open
            } else if (name == QLatin1String("table-cell")) {
                if (inCell && curTable && cellAlignment>=0) {
                    auto cell=curTable->cellAt(tRow,tCol);
                    auto format=cell.format().toTableCellFormat();
                    format.setVerticalAlignment(static_cast<QTextCharFormat::VerticalAlignment>(cellAlignment));
                    cell.setFormat(format);
                }
                inCell = false;
            } else if (name == QLatin1String("table")) {
                if (curTable) {
                    for (const Merge &m : std::as_const(merges))
                        if (m.row + m.rows <= curTable->rows() && m.col + m.cols <= curTable->columns())
                            curTable->mergeCells(m.row, m.col, m.rows, m.cols);
                    // Continue after the table frame, reusing the trailing block Qt keeps there.
                    c.setPosition(curTable->lastPosition() + 1);
                    firstBlock = true;
                    curTable = nullptr;
                }
            } else if (name == QLatin1String("frame")) {
                if (!imgHref.isEmpty()) {
                    // Pull the embedded picture out of the archive into a document resource. The
                    // name must be an ABSOLUTE opaque URL: resource lookup goes through
                    // baseUrl.resolved(name), and TextEdit sets baseUrl to the QML dir — a
                    // relative "Pictures/x" key would never match again.
                    const QImage img = QImage::fromData(zip.fileData(imgHref));
                    if (!img.isNull()) {
                        const QUrl resUrl(QStringLiteral("odf:/") + imgHref);
                        doc->addResource(QTextDocument::ImageResource, resUrl, img);
                        QTextImageFormat f;
                        f.setName(resUrl.toString());
                        if (imgW > 0) f.setWidth(imgW);
                        if (imgH > 0) f.setHeight(imgH);
                        act().insertImage(f);
                        if (!runs.isEmpty())
                            act().setCharFormat(runs.last().format);
                    }
                    imgHref.clear();
                }
            }
        } else if (tok == QXmlStreamReader::Characters) {
            // Whitespace between spans IS content inside a paragraph (a single significant
            // space); outside paragraphs it is XML formatting noise.
            if (paragraphDepth > 0)
                act().insertText(xml.text().toString());
        }
    }
    c.endEditBlock();
    doc->setUndoRedoEnabled(true);
    doc->setModified(false);
    *sheetOut = sheet;

    if (xml.hasError()) {
        *error = QStringLiteral("ODF parse error: ") + xml.errorString();
        return false;
    }
    error->clear();
    return true;
}

// ═══ writer ═════════════════════════════════════════════════════════════════════════════════════

namespace {

#define ODF_NAMESPACES \
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\"" \
    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\"" \
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\"" \
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\"" \
    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\"" \
    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\"" \
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\"" \
    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\"" \
    " xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\"" \
    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"" \
    " office:version=\"1.3\""

QString inches(qreal px) { return QString::number(px / 96.0, 'g', 8) + QStringLiteral("in"); }
QString points(qreal pt) { return QString::number(pt, 'g', 6) + QStringLiteral("pt"); }

// One property map → the attributes of style:paragraph-properties / style:text-properties.
void writeProps(QXmlStreamWriter &x, const QVariantMap &props, QSet<QString> *fonts)
{
    const auto has = [&](const char *k) { const QVariant v = props.value(QLatin1String(k)); return v.isValid() && !v.isNull(); };
    const auto get = [&](const char *k) { return props.value(QLatin1String(k)); };

    // __pageBreakBefore: office item 2b item 5's insertPageBreak() — NOT a rt::Prop (page breaks
    // are a direct, document-level flag, never part of a named style), set straight from
    // QTextBlockFormat::pageBreakPolicy() by writeBlock(), read back the same way by readOdt().
    const bool pageBreak = props.value(QStringLiteral("__pageBreakBefore")).toBool();
    bool anyPara = pageBreak;
    for (const char *k : { "marginTop", "marginBottom", "marginLeft", "marginRight", "textIndent", "lineHeight",
                           "alignment", "keepWithNext" })
        anyPara = anyPara || has(k);
    if (anyPara) {
        x.writeStartElement(QStringLiteral("style:paragraph-properties"));
        const auto len = [&](const char *k, const char *attr) {
            if (has(k)) x.writeAttribute(QLatin1String(attr), inches(get(k).toDouble()));
        };
        len("marginTop", "fo:margin-top");
        len("marginBottom", "fo:margin-bottom");
        len("marginLeft", "fo:margin-left");
        len("marginRight", "fo:margin-right");
        len("textIndent", "fo:text-indent");
        if (has("lineHeight")) {
            const QVariant v = get("lineHeight");
            x.writeAttribute(QStringLiteral("fo:line-height"), v.typeId() == QMetaType::QString
                ? inches(lengthPx(v.toString())) : QString::number(v.toDouble() * 100.0, 'g', 6) + QLatin1Char('%'));
        }
        if (has("alignment")) {
            const QString a = get("alignment").toString();
            x.writeAttribute(QStringLiteral("fo:text-align"), a == QLatin1String("left") ? QStringLiteral("start")
                             : a == QLatin1String("right") ? QStringLiteral("end") : a);
        }
        if (has("keepWithNext"))
            x.writeAttribute(QStringLiteral("fo:keep-with-next"), get("keepWithNext").toBool() ? QStringLiteral("always") : QStringLiteral("auto"));
        if (pageBreak)
            x.writeAttribute(QStringLiteral("fo:break-before"), QStringLiteral("page"));
        x.writeEndElement();
    }

    bool anyText = false;
    for (const char *k : { "fontFamily", "fontSize", "fontWeight", "italic", "underline", "strike", "color", "letterSpacing", "language" })
        anyText = anyText || has(k);
    if (anyText) {
        x.writeStartElement(QStringLiteral("style:text-properties"));
        if (has("language")) {
            const QString tag = get("language").toString();
            x.writeAttribute(QStringLiteral("fo:language"), tag.section(QLatin1Char('-'), 0, 0));
            const QString country = tag.section(QLatin1Char('-'), 1);
            if (!country.isEmpty()) x.writeAttribute(QStringLiteral("fo:country"), country);
        }
        if (has("fontFamily")) {
            const QString f = get("fontFamily").toString();
            x.writeAttribute(QStringLiteral("style:font-name"), f);
            if (fonts) fonts->insert(f);
        }
        if (has("fontSize")) {
            const QString s = points(get("fontSize").toDouble());
            x.writeAttribute(QStringLiteral("fo:font-size"), s);
            x.writeAttribute(QStringLiteral("style:font-size-asian"), s);
            x.writeAttribute(QStringLiteral("style:font-size-complex"), s);
        }
        if (has("fontWeight")) {
            const int w = get("fontWeight").toInt();
            const QString s = w == 700 ? QStringLiteral("bold") : w == 400 ? QStringLiteral("normal")
                                                                           : QString::number(qBound(100, (w + 50) / 100 * 100, 900));
            x.writeAttribute(QStringLiteral("fo:font-weight"), s);
            x.writeAttribute(QStringLiteral("style:font-weight-asian"), s);
            x.writeAttribute(QStringLiteral("style:font-weight-complex"), s);
        }
        if (has("italic")) {
            const QString s = get("italic").toBool() ? QStringLiteral("italic") : QStringLiteral("normal");
            x.writeAttribute(QStringLiteral("fo:font-style"), s);
            x.writeAttribute(QStringLiteral("style:font-style-asian"), s);
            x.writeAttribute(QStringLiteral("style:font-style-complex"), s);
        }
        if (has("underline")) {
            const bool on = get("underline").toBool();
            x.writeAttribute(QStringLiteral("style:text-underline-style"), on ? QStringLiteral("solid") : QStringLiteral("none"));
            if (on) {
                x.writeAttribute(QStringLiteral("style:text-underline-width"), QStringLiteral("auto"));
                x.writeAttribute(QStringLiteral("style:text-underline-color"), QStringLiteral("font-color"));
            }
        }
        if (has("strike")) {
            const bool on = get("strike").toBool();
            x.writeAttribute(QStringLiteral("style:text-line-through-style"), on ? QStringLiteral("solid") : QStringLiteral("none"));
            if (on)
                x.writeAttribute(QStringLiteral("style:text-line-through-type"), QStringLiteral("single"));
        }
        if (has("color"))
            x.writeAttribute(QStringLiteral("fo:color"), get("color").toString());
        if (has("letterSpacing")) {
            const qreal ls = get("letterSpacing").toDouble();
            x.writeAttribute(QStringLiteral("fo:letter-spacing"), qFuzzyIsNull(ls) ? QStringLiteral("normal") : inches(ls));
        }
        x.writeEndElement();
    }
}

QByteArray propsKey(const QVariantMap &props)
{
    QByteArray k;
    for (auto it = props.cbegin(); it != props.cend(); ++it)
        k += it.key().toUtf8() + '=' + it.value().toString().toUtf8() + ';';
    return k;
}

class OdtWriter {
public:
    // `zoom`: divides every length read off `doc`'s actual (possibly zoomed) formats before it is
    // compared against `sheet` or written out, so the file does not depend on the zoom in effect
    // (office item 2b, ★2) — `sheet` itself is always the canonical, unscaled one.
    OdtWriter(const QTextDocument *doc, const StyleSheet &sheet, qreal zoom)
        : m_doc(doc), m_sheet(sheet), m_x(&m_body), m_unzoom(1.0 / zoom) {}

    bool write(QIODevice *dev, QString *error)
    {
        m_x.setAutoFormatting(false); // whitespace inside paragraphs is content
        writeFlow(m_doc->rootFrame()->begin(), m_doc->rootFrame()->end());
        closeLists(0);

        QZipWriter zip(dev);
        // The mimetype entry first and STORED: how a reader recognizes an OpenDocument package.
        zip.setCompressionPolicy(QZipWriter::NeverCompress);
        zip.addFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/vnd.oasis.opendocument.text"));
        zip.setCompressionPolicy(QZipWriter::AlwaysCompress);
        zip.addFile(QStringLiteral("content.xml"), contentXml());
        zip.addFile(QStringLiteral("styles.xml"), stylesXml());
        zip.addFile(QStringLiteral("meta.xml"), metaXml());
        for (const Picture &p : std::as_const(m_pictures)) {
            zip.setCompressionPolicy(QZipWriter::NeverCompress); // already compressed
            zip.addFile(p.path, p.data);
        }
        zip.setCompressionPolicy(QZipWriter::AlwaysCompress);
        zip.addFile(QStringLiteral("META-INF/manifest.xml"), manifestXml());
        zip.close();
        if (zip.status() != QZipWriter::NoError) {
            *error = dev->errorString();
            return false;
        }
        return true;
    }

private:
    struct AutoStyle { QString name; QString family; QString parent; QVariantMap props; };
    struct Picture { QString path; QByteArray data; QString mime; };

    // ── body ────────────────────────────────────────────────────────────────────────────────────
    void writeFlow(QTextFrame::iterator it, const QTextFrame::iterator &end)
    {
        for (; !(it == end); ++it) {
            if (QTextFrame *f = it.currentFrame()) {
                closeLists(0);
                if (auto *t = qobject_cast<QTextTable *>(f))
                    writeTable(t);
                else
                    writeFlow(f->begin(), f->end());
            } else if (const QTextBlock b = it.currentBlock(); b.isValid()) {
                writeBlock(b);
            }
        }
    }

    QString odfName(const QString &style) const
    {
        const NamedStyle *s = m_sheet.find(style);
        return s ? s->odfName : kStandard;
    }

    QString autoStyle(QList<AutoStyle> &list, const char *prefix, const QString &family, const QString &parent,
                      const QVariantMap &props)
    {
        const QByteArray key = family.toUtf8() + '|' + parent.toUtf8() + '|' + propsKey(props);
        if (auto it = m_autoKeys.constFind(key); it != m_autoKeys.cend())
            return *it;
        AutoStyle a { QLatin1String(prefix) + QString::number(list.size() + 1), family, parent, props };
        list << a;
        m_autoKeys.insert(key, a.name);
        return a.name;
    }

    // ── lists: text:list levels open around list items (nested by the list's indent) ─────────
    void syncLists(const QTextBlock &b)
    {
        const QTextList *l = b.textList();
        if (!l) {
            closeLists(0);
            return;
        }
        const int depth = qBound(1, l->format().indent(), 10);
        if (m_lists.size() >= depth)
            closeLists(m_lists[depth - 1] == l ? depth : depth - 1);
        while (m_lists.size() < depth) {
            const int level = int(m_lists.size()) + 1;
            if (level > 1)
                m_x.writeStartElement(QStringLiteral("text:list-item")); // holds the nested list
            m_x.writeStartElement(QStringLiteral("text:list"));
            const QTextListFormat::Style kind = level == depth ? l->format().style() : QTextListFormat::ListDisc;
            m_x.writeAttribute(QStringLiteral("text:style-name"), listStyle(kind));
            m_lists << (level == depth ? l : nullptr);
        }
    }

    void closeLists(int keep)
    {
        while (m_lists.size() > keep) {
            m_lists.removeLast();
            m_x.writeEndElement(); // text:list
            if (!m_lists.isEmpty())
                m_x.writeEndElement(); // the text:list-item holding it
        }
    }

    QString listStyle(QTextListFormat::Style kind)
    {
        if (auto it = m_listStyles.constFind(int(kind)); it != m_listStyles.cend())
            return *it;
        const QString name = QStringLiteral("L%1").arg(m_listStyles.size() + 1);
        m_listStyles.insert(int(kind), name);
        m_listOrder << int(kind);
        return name;
    }

    void writeBlock(const QTextBlock &b)
    {
        syncLists(b);
        const bool listItem = !m_lists.isEmpty();
        if (listItem)
            m_x.writeStartElement(QStringLiteral("text:list-item"));

        // Unzoom first: direct/style comparisons and whatever we write out must be in the
        // document's CANONICAL units, independent of the zoom in effect (office item 2b, ★2).
        const QTextBlockFormat bf = scaledLengths(b.blockFormat(), m_unzoom);
        const QString style = paragraphStyleOf(bf);
        QVariantMap direct = directProps(bf, m_sheet.blockFormat(style), true);
        direct.remove(QStringLiteral("outlineLevel")); // text:h's text:outline-level says it
        // insertPageBreak() (item 5): not a rt::Prop, so directProps() above never sees it —
        // forced into `direct` straight from the live format so writeProps() picks it up.
        if (bf.pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore)
            direct.insert(QStringLiteral("__pageBreakBefore"), true);
        const int level = bf.headingLevel();
        m_x.writeStartElement(level > 0 ? QStringLiteral("text:h") : QStringLiteral("text:p"));
        m_x.writeAttribute(QStringLiteral("text:style-name"), direct.isEmpty()
            ? odfName(style) : autoStyle(m_autoPara, "P", QStringLiteral("paragraph"), odfName(style), direct));
        if (level > 0)
            m_x.writeAttribute(QStringLiteral("text:outline-level"), QString::number(level));
        // Office item 3: the block's stable id round-trips as xml:id (ODF 1.2+ on text:p/text:h).
        if (const BlockInfo *info = blockInfo(b); info && !info->id.isEmpty())
            m_x.writeAttribute(QStringLiteral("xml:id"), info->id);

        m_atStart = true;
        m_prevSpace = false;
        m_pendingSpaces = 0;
        QString openKey; // the span(s) open now: "charStyle|autoStyle"
        int openDepth = 0;
        const auto closeSpans = [&] {
            flushText();
            for (; openDepth > 0; --openDepth)
                m_x.writeEndElement();
            openKey.clear();
        };
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            const QTextCharFormat cf = scaledLengths(frag.charFormat(), m_unzoom);
            if (cf.isImageFormat()) {
                closeSpans();
                const QString text = frag.text();
                for (QChar ch : text)
                    if (ch == QChar::ObjectReplacementCharacter)
                        writeImage(cf.toImageFormat());
                m_atStart = false;
                m_prevSpace = false;
                continue;
            }
            const QString cs = charStyleOf(cf);
            const QVariantMap d = directProps(cf, m_sheet.charFormat(style, cs), false);
            const QString autoName = d.isEmpty() ? QString()
                : autoStyle(m_autoText, "T", QStringLiteral("text"), QString(), d);
            const QString key = cs + QLatin1Char('|') + autoName;
            if (key != openKey) {
                closeSpans();
                if (!cs.isEmpty()) {
                    m_x.writeStartElement(QStringLiteral("text:span"));
                    m_x.writeAttribute(QStringLiteral("text:style-name"), odfName(cs));
                    ++openDepth;
                }
                if (!autoName.isEmpty()) {
                    m_x.writeStartElement(QStringLiteral("text:span"));
                    m_x.writeAttribute(QStringLiteral("text:style-name"), autoName);
                    ++openDepth;
                }
                openKey = key;
            }
            writeText(frag.text());
        }
        closeSpans();
        m_x.writeEndElement(); // text:p / text:h
        if (listItem)
            m_x.writeEndElement();
    }

    // Text with ODF's white-space rules: runs of spaces (and a leading one) as text:s, tabs as
    // text:tab, line breaks (U+2028) as text:line-break.
    void writeText(QStringView text)
    {
        for (QChar ch : text) {
            if (ch == QLatin1Char(' ')) {
                if (m_atStart || m_prevSpace) {
                    flushChars();
                    ++m_pendingSpaces;
                } else {
                    m_buf += ch;
                }
                m_prevSpace = true;
                m_atStart = false;
                continue;
            }
            m_atStart = false;
            m_prevSpace = false;
            if (ch == QLatin1Char('\t')) {
                flushText();
                m_x.writeEmptyElement(QStringLiteral("text:tab"));
            } else if (ch == QChar::LineSeparator) {
                flushText();
                m_x.writeEmptyElement(QStringLiteral("text:line-break"));
            } else if (ch.unicode() < 0x20 || ch == QChar::ObjectReplacementCharacter || ch == QChar::ParagraphSeparator
                       || ch.unicode() == 0xfdd0 || ch.unicode() == 0xfdd1) {
                // control characters, objects and frame markers carry no text
            } else {
                flushSpaces();
                m_buf += ch;
            }
        }
    }
    void flushChars()
    {
        if (!m_buf.isEmpty()) {
            m_x.writeCharacters(m_buf);
            m_buf.clear();
        }
    }
    void flushSpaces()
    {
        if (m_pendingSpaces > 0) {
            flushChars();
            m_x.writeEmptyElement(QStringLiteral("text:s"));
            if (m_pendingSpaces > 1)
                m_x.writeAttribute(QStringLiteral("text:c"), QString::number(m_pendingSpaces));
            m_pendingSpaces = 0;
        }
    }
    void flushText()
    {
        flushChars();
        flushSpaces();
    }

    void writeImage(const QTextImageFormat &f)
    {
        const QString name = f.name();
        QString path = m_pictureByName.value(name);
        QSize size;
        if (path.isEmpty()) {
            const QVariant res = m_doc->resource(QTextDocument::ImageResource, QUrl(name));
            QImage img;
            QByteArray data;
            if (res.typeId() == QMetaType::QImage) img = res.value<QImage>();
            else if (res.typeId() == QMetaType::QPixmap) img = res.value<QPixmap>().toImage();
            else if (res.typeId() == QMetaType::QByteArray) img = QImage::fromData(res.toByteArray());
            if (img.isNull())
                img = QImage(localPath(name));
            if (img.isNull())
                return; // nothing to embed: the frame would point at nothing
            QBuffer buf(&data);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            path = QStringLiteral("Pictures/image%1.png").arg(m_pictures.size() + 1);
            m_pictures << Picture { path, data, QStringLiteral("image/png") };
            m_pictureByName.insert(name, path);
            m_pictureSize.insert(path, img.size());
        }
        size = m_pictureSize.value(path);
        qreal w = f.width() > 0 ? f.width() : size.width();
        qreal h = f.height() > 0 ? f.height() : (f.width() > 0 && size.width() > 0 ? size.height() * w / size.width() : size.height());
        m_x.writeStartElement(QStringLiteral("draw:frame"));
        m_x.writeAttribute(QStringLiteral("draw:name"), QStringLiteral("Image%1").arg(++m_frameCount));
        m_x.writeAttribute(QStringLiteral("text:anchor-type"), QStringLiteral("as-char"));
        m_x.writeAttribute(QStringLiteral("svg:width"), inches(w));
        m_x.writeAttribute(QStringLiteral("svg:height"), inches(h));
        m_x.writeAttribute(QStringLiteral("draw:z-index"), QStringLiteral("0"));
        m_x.writeEmptyElement(QStringLiteral("draw:image"));
        m_x.writeAttribute(QStringLiteral("xlink:href"), path);
        m_x.writeAttribute(QStringLiteral("xlink:type"), QStringLiteral("simple"));
        m_x.writeAttribute(QStringLiteral("xlink:show"), QStringLiteral("embed"));
        m_x.writeAttribute(QStringLiteral("xlink:actuate"), QStringLiteral("onLoad"));
        m_x.writeEndElement();
    }

    void writeTable(QTextTable *t)
    {
        closeLists(0);
        const QString name = QStringLiteral("Table%1").arg(++m_tableCount);
        const QTextTableFormat tf = t->format();
        m_tables << TableStyle { name, tf.border(), tf.cellPadding() };
        m_x.writeStartElement(QStringLiteral("table:table"));
        m_x.writeAttribute(QStringLiteral("table:name"), name);
        m_x.writeAttribute(QStringLiteral("table:style-name"), name);
        m_x.writeEmptyElement(QStringLiteral("table:table-column"));
        if (t->columns() > 1)
            m_x.writeAttribute(QStringLiteral("table:number-columns-repeated"), QString::number(t->columns()));
        for (int r = 0; r < t->rows(); ++r) {
            m_x.writeStartElement(QStringLiteral("table:table-row"));
            for (int col = 0; col < t->columns(); ++col) {
                const QTextTableCell cell = t->cellAt(r, col);
                if (cell.row() != r || cell.column() != col) {
                    m_x.writeEmptyElement(QStringLiteral("table:covered-table-cell"));
                    continue;
                }
                m_x.writeStartElement(QStringLiteral("table:table-cell"));
                const int alignment=cell.format().verticalAlignment();
                m_tables.last().alignments |= 1u << alignment;
                m_x.writeAttribute(QStringLiteral("table:style-name"), name + QStringLiteral(".A1-%1").arg(alignment));
                if (cell.columnSpan() > 1)
                    m_x.writeAttribute(QStringLiteral("table:number-columns-spanned"), QString::number(cell.columnSpan()));
                if (cell.rowSpan() > 1)
                    m_x.writeAttribute(QStringLiteral("table:number-rows-spanned"), QString::number(cell.rowSpan()));
                m_x.writeAttribute(QStringLiteral("office:value-type"), QStringLiteral("string"));
                writeFlow(cell.begin(), cell.end());
                closeLists(0);
                m_x.writeEndElement();
            }
            m_x.writeEndElement();
        }
        m_x.writeEndElement();
    }

    // ── package parts ───────────────────────────────────────────────────────────────────────────
    void writeFontDecls(QXmlStreamWriter &x, const QSet<QString> &fonts)
    {
        x.writeStartElement(QStringLiteral("office:font-face-decls"));
        QStringList sorted(fonts.cbegin(), fonts.cend());
        sorted.sort();
        for (const QString &f : std::as_const(sorted)) {
            x.writeEmptyElement(QStringLiteral("style:font-face"));
            x.writeAttribute(QStringLiteral("style:name"), f);
            x.writeAttribute(QStringLiteral("svg:font-family"), f.contains(QLatin1Char(' ')) ? QLatin1Char('\'') + f + QLatin1Char('\'') : f);
        }
        x.writeEndElement();
    }

    QByteArray contentXml()
    {
        // Automatic styles are known once the body is written: assemble around it.
        QByteArray styles;
        QSet<QString> fonts;
        {
            QXmlStreamWriter x(&styles);
            const auto writeAuto = [&](const AutoStyle &a) {
                x.writeStartElement(QStringLiteral("style:style"));
                x.writeAttribute(QStringLiteral("style:name"), a.name);
                x.writeAttribute(QStringLiteral("style:family"), a.family);
                if (!a.parent.isEmpty())
                    x.writeAttribute(QStringLiteral("style:parent-style-name"), a.parent);
                writeProps(x, a.props, &fonts);
                x.writeEndElement();
            };
            for (const AutoStyle &a : std::as_const(m_autoPara)) writeAuto(a);
            for (const AutoStyle &a : std::as_const(m_autoText)) writeAuto(a);
            for (const TableStyle &t : std::as_const(m_tables)) {
                x.writeStartElement(QStringLiteral("style:style"));
                x.writeAttribute(QStringLiteral("style:name"), t.name);
                x.writeAttribute(QStringLiteral("style:family"), QStringLiteral("table"));
                x.writeEmptyElement(QStringLiteral("style:table-properties"));
                x.writeAttribute(QStringLiteral("table:align"), QStringLiteral("margins"));
                x.writeEndElement();
                for (int alignment=0;alignment<=QTextCharFormat::AlignBaseline;++alignment) {
                    if (!(t.alignments & (1u<<alignment))) continue;
                x.writeStartElement(QStringLiteral("style:style"));
                x.writeAttribute(QStringLiteral("style:name"), t.name + QStringLiteral(".A1-%1").arg(alignment));
                x.writeAttribute(QStringLiteral("style:family"), QStringLiteral("table-cell"));
                x.writeEmptyElement(QStringLiteral("style:table-cell-properties"));
                if (alignment==QTextCharFormat::AlignMiddle) x.writeAttribute(QStringLiteral("style:vertical-align"),QStringLiteral("middle"));
                else if (alignment==QTextCharFormat::AlignBottom) x.writeAttribute(QStringLiteral("style:vertical-align"),QStringLiteral("bottom"));
                else if (alignment==QTextCharFormat::AlignTop) x.writeAttribute(QStringLiteral("style:vertical-align"),QStringLiteral("top"));
                x.writeAttribute(QStringLiteral("fo:padding"), inches(t.padding));
                x.writeAttribute(QStringLiteral("fo:border"), t.border > 0
                    ? points(t.border * 0.75) + QStringLiteral(" solid #000000") : QStringLiteral("none"));
                x.writeEndElement();
                }
            }
            for (int kind : std::as_const(m_listOrder)) {
                x.writeStartElement(QStringLiteral("text:list-style"));
                x.writeAttribute(QStringLiteral("style:name"), m_listStyles.value(kind));
                for (int level = 1; level <= 10; ++level) {
                    const bool number = kind == QTextListFormat::ListDecimal || kind == QTextListFormat::ListLowerAlpha
                        || kind == QTextListFormat::ListUpperAlpha || kind == QTextListFormat::ListLowerRoman
                        || kind == QTextListFormat::ListUpperRoman;
                    x.writeStartElement(number ? QStringLiteral("text:list-level-style-number")
                                               : QStringLiteral("text:list-level-style-bullet"));
                    x.writeAttribute(QStringLiteral("text:level"), QString::number(level));
                    if (number) {
                        const QString fmt = kind == QTextListFormat::ListLowerAlpha ? QStringLiteral("a")
                            : kind == QTextListFormat::ListUpperAlpha ? QStringLiteral("A")
                            : kind == QTextListFormat::ListLowerRoman ? QStringLiteral("i")
                            : kind == QTextListFormat::ListUpperRoman ? QStringLiteral("I") : QStringLiteral("1");
                        x.writeAttribute(QStringLiteral("style:num-suffix"), QStringLiteral("."));
                        x.writeAttribute(QStringLiteral("style:num-format"), fmt);
                    } else {
                        x.writeAttribute(QStringLiteral("text:bullet-char"), kind == QTextListFormat::ListCircle
                            ? QStringLiteral("◦") : kind == QTextListFormat::ListSquare ? QStringLiteral("▪") : QStringLiteral("•"));
                    }
                    x.writeStartElement(QStringLiteral("style:list-level-properties"));
                    x.writeAttribute(QStringLiteral("text:list-level-position-and-space-mode"), QStringLiteral("label-alignment"));
                    x.writeEmptyElement(QStringLiteral("style:list-level-label-alignment"));
                    x.writeAttribute(QStringLiteral("text:label-followed-by"), QStringLiteral("listtab"));
                    x.writeAttribute(QStringLiteral("text:list-tab-stop-position"), QString::number(0.25 * level, 'f', 4) + QStringLiteral("in"));
                    x.writeAttribute(QStringLiteral("fo:text-indent"), QStringLiteral("-0.25in"));
                    x.writeAttribute(QStringLiteral("fo:margin-left"), QString::number(0.25 * level, 'f', 4) + QStringLiteral("in"));
                    x.writeEndElement();
                    x.writeEndElement();
                }
                x.writeEndElement();
            }
        }
        QByteArray decls;
        {
            QXmlStreamWriter x(&decls);
            writeFontDecls(x, fonts);
        }
        return QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content" ODF_NAMESPACES ">")
            + decls + QByteArrayLiteral("<office:automatic-styles>") + styles
            + QByteArrayLiteral("</office:automatic-styles><office:body><office:text>") + m_body
            + QByteArrayLiteral("</office:text></office:body></office:document-content>");
    }

    QByteArray stylesXml()
    {
        QByteArray styles;
        QSet<QString> fonts;
        {
            QXmlStreamWriter x(&styles);
            for (const QString &n : m_sheet.order()) {
                const NamedStyle *s = m_sheet.find(n);
                x.writeStartElement(QStringLiteral("style:style"));
                x.writeAttribute(QStringLiteral("style:name"), s->odfName);
                if (!s->odfDisplayName.isEmpty() && s->odfDisplayName != s->odfName)
                    x.writeAttribute(QStringLiteral("style:display-name"), s->odfDisplayName);
                x.writeAttribute(QStringLiteral("style:family"), s->family == Family::Paragraph ? QStringLiteral("paragraph") : QStringLiteral("text"));
                if (!s->parent.isEmpty())
                    x.writeAttribute(QStringLiteral("style:parent-style-name"), odfName(s->parent));
                if (!s->next.isEmpty())
                    x.writeAttribute(QStringLiteral("style:next-style-name"), odfName(s->next));
                if (const int level = s->props.value(QStringLiteral("outlineLevel")).toInt(); level > 0)
                    x.writeAttribute(QStringLiteral("style:default-outline-level"), QString::number(level));
                if (s->family == Family::Paragraph)
                    x.writeAttribute(QStringLiteral("style:class"), QStringLiteral("text"));
                writeProps(x, s->props, &fonts);
                x.writeEndElement();
            }
        }
        QByteArray decls;
        {
            QXmlStreamWriter x(&decls);
            writeFontDecls(x, fonts);
        }
        return QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles" ODF_NAMESPACES ">")
            + decls + QByteArrayLiteral("<office:styles>") + styles
            // An office:master-styles element (even empty) is what makes LibreOffice link the
            // styles to their parents (style:parent-style-name is dropped on import without it —
            // verified with LibreOffice 26.8).
            + QByteArrayLiteral("</office:styles><office:master-styles/></office:document-styles>");
    }

    QByteArray metaXml() const
    {
        const QByteArray now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8();
        return QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta" ODF_NAMESPACES ">"
                                 "<office:meta><meta:generator>solid-qml RichText</meta:generator><dc:date>")
            + now + QByteArrayLiteral("</dc:date></office:meta></office:document-meta>");
    }

    QByteArray manifestXml() const
    {
        QByteArray m = QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
            "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
            "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
            "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
            "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>");
        for (const Picture &p : m_pictures)
            m += "<manifest:file-entry manifest:full-path=\"" + p.path.toUtf8() + "\" manifest:media-type=\"" + p.mime.toUtf8() + "\"/>";
        return m + QByteArrayLiteral("</manifest:manifest>");
    }

    struct TableStyle { QString name; qreal border; qreal padding; unsigned alignments = 0; };

    const QTextDocument *m_doc;
    const StyleSheet &m_sheet;
    QByteArray m_body;
    QXmlStreamWriter m_x;
    QList<AutoStyle> m_autoPara, m_autoText;
    QHash<QByteArray, QString> m_autoKeys;
    QList<const QTextList *> m_lists; // open text:list per level (null: a wrapper level)
    QHash<int, QString> m_listStyles;
    QList<int> m_listOrder;
    QList<Picture> m_pictures;
    QHash<QString, QString> m_pictureByName;
    QHash<QString, QSize> m_pictureSize;
    QList<TableStyle> m_tables;
    int m_tableCount = 0;
    int m_frameCount = 0;
    // text state of the paragraph being written
    QString m_buf;
    bool m_atStart = true;
    bool m_prevSpace = false;
    int m_pendingSpaces = 0;
    qreal m_unzoom = 1.0; // 1 / zoom: divides every length read off the (possibly zoomed) document
};

} // namespace

bool writeOdt(const QString &path, const QTextDocument *doc, const StyleSheet &sheet, qreal zoom, QString *error)
{
    // The package is built in memory (QZipWriter closes its device, which QSaveFile forbids),
    // then committed atomically: a failed write leaves the previous file intact.
    QByteArray package;
    QBuffer buffer(&package);
    buffer.open(QIODevice::WriteOnly);
    OdtWriter w(doc, sheet, zoom);
    QString why;
    if (!w.write(&buffer, &why)) {
        *error = QStringLiteral("could not write ") + path + (why.isEmpty() ? QString() : QStringLiteral(": ") + why);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(package) != package.size() || !file.commit()) {
        *error = QStringLiteral("could not write ") + path + QStringLiteral(": ") + file.errorString();
        return false;
    }
    error->clear();
    return true;
}

} // namespace rt
