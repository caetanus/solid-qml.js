#include "csstheme.h"

#include "valueparser.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QJSValue>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace {

QString stripComments(const QString &css)
{
    static const QRegularExpression re(QStringLiteral(R"(/\*.*?\*/)"),
                                       QRegularExpression::DotMatchesEverythingOption);
    QString result = css;
    result.remove(re);
    return result;
}

struct RawMediaBlock {
    QString condition; // the part between `@media` and `{`
    QString body;      // the rules inside
};

// Pull `@media` blocks out of `css` into `media` (condition + inner rules), and DROP the
// at-rules the flat parser can't scope (@supports/@container). Returns the base CSS with all
// of these removed. Brace-balanced. @media rules are applied conditionally by rebuildRules().
QString extractAtRules(const QString &css, QList<RawMediaBlock> &media)
{
    QString out;
    int i = 0;
    while (i < css.length()) {
        if (css.at(i) == QLatin1Char('@')) {
            int j = i + 1;
            while (j < css.length() && css.at(j).isLetter())
                ++j;
            const QString name = css.mid(i + 1, j - i - 1).toLower();
            const bool isMedia = (name == QLatin1String("media"));
            if (isMedia || name == QLatin1String("supports") || name == QLatin1String("container")) {
                const int brace = css.indexOf(QLatin1Char('{'), j);
                if (brace < 0)
                    break; // malformed — drop the rest
                int depth = 0, k = brace;
                for (; k < css.length(); ++k) {
                    if (css.at(k) == QLatin1Char('{'))
                        ++depth;
                    else if (css.at(k) == QLatin1Char('}') && --depth == 0) {
                        ++k;
                        break;
                    }
                }
                if (isMedia) {
                    RawMediaBlock blk;
                    blk.condition = css.mid(j, brace - j).trimmed();
                    blk.body = css.mid(brace + 1, k - brace - 2); // inside the braces
                    media.append(blk);
                }
                i = k; // skip the whole at-rule block
                continue;
            }
        }
        out += css.at(i++);
    }
    return out;
}

// Parse a media condition string ("(max-width: 720px) and (min-width: 400px), print") into
// an OR of AND-groups of feature constraints. Media types (screen/all/print) and unknown
// features are ignored (treated as no constraint). An empty/typeless query always matches.
CssMediaQuery parseMediaCondition(const QString &condition)
{
    CssMediaQuery query;
    const QStringList orParts = condition.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &orPart : orParts) {
        QList<CssMediaFeature> group;
        const QStringList andParts = orPart.split(QLatin1String(" and "), Qt::SkipEmptyParts);
        for (const QString &andPart : andParts) {
            const QString p = andPart.trimmed();
            const int open = p.indexOf(QLatin1Char('('));
            const int colon = p.indexOf(QLatin1Char(':'), open);
            const int close = p.indexOf(QLatin1Char(')'), colon);
            if (open < 0 || colon < 0 || close < 0)
                continue; // a media type (screen/all/print) or `not`/`only` — no constraint
            const QString name = p.mid(open + 1, colon - open - 1).trimmed().toLower();
            double value = 0;
            if (!CssValueParser::parseLengthPx(p.mid(colon + 1, close - colon - 1).trimmed(), &value))
                continue;
            if (name == QLatin1String("min-width") || name == QLatin1String("max-width")
                || name == QLatin1String("min-height") || name == QLatin1String("max-height"))
                group.append({ name, value });
        }
        query.orGroups.append(group);
    }
    return query;
}

bool mediaMatches(const CssMediaQuery &query, qreal width, qreal height)
{
    if (query.orGroups.isEmpty())
        return true;
    for (const QList<CssMediaFeature> &group : query.orGroups) {
        bool all = true;
        for (const CssMediaFeature &f : group) {
            const bool ok = (f.name == QLatin1String("min-width")) ? width >= f.value
                : (f.name == QLatin1String("max-width")) ? width <= f.value
                : (f.name == QLatin1String("min-height")) ? height >= f.value
                : (f.name == QLatin1String("max-height")) ? height <= f.value
                : true;
            if (!ok) { all = false; break; }
        }
        if (all)
            return true;
    }
    return false;
}

// Replace `@name` references with values from `vars`; unknown names (e.g.
// `@media`) are left untouched.
QString substituteVars(const QString &text, const QHash<QString, QString> &vars)
{
    static const QRegularExpression refRe(QStringLiteral(R"(@([A-Za-z0-9_-]+))"));
    QString result;
    qsizetype last = 0;
    auto it = refRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        result += text.mid(last, m.capturedStart() - last);
        const QString name = m.captured(1);
        result += vars.contains(name) ? vars.value(name) : m.captured(0);
        last = m.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

// Inline `@import` statements by reading the referenced file and recursively expanding
// it in place. Resolves a bare/relative path against `baseDir`; `visited` (absolute paths)
// guards against import cycles. http(s) imports are skipped here — there is no base dir or
// synchronous fetch at this layer (use `set-css <url>` for a remote *root* theme instead).
QString expandImports(const QString &cssIn, const QString &baseDir, QStringList &visited)
{
    static const QRegularExpression importRe(
        QStringLiteral(R"(@import\s+(?:url\(\s*)?['"]?([^'")\s]+)['"]?\s*\)?\s*;)"));

    QString out;
    int last = 0;
    auto it = importRe.globalMatch(cssIn);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += cssIn.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();

        QString ref = m.captured(1);
        if (ref.startsWith(QLatin1String("http://")) || ref.startsWith(QLatin1String("https://"))) {
            qWarning() << "CssTheme @import: remote imports are not supported:" << ref;
            continue;
        }
        if (ref.startsWith(QLatin1String("file://")))
            ref = QUrl(ref).toLocalFile();
        const QString path = QFileInfo(ref).isAbsolute() ? ref : QDir(baseDir).filePath(ref);
        const QString abs = QFileInfo(path).absoluteFilePath();
        if (visited.contains(abs))
            continue; // already imported (cycle / duplicate)
        visited.append(abs);

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "CssTheme @import: cannot read" << path;
            continue;
        }
        const QString sub = QString::fromUtf8(file.readAll());
        out += expandImports(sub, QFileInfo(path).absolutePath(), visited);
    }
    out += cssIn.mid(last);
    return out;
}

// Expand GTK-style `@define-color name value;` declarations: collect them,
// resolve references between them, substitute `@name` throughout, then strip
// the declarations themselves.
QString expandDefineColors(const QString &cssIn)
{
    static const QRegularExpression defRe(
        QStringLiteral(R"(@define-color\s+([A-Za-z0-9_-]+)\s+([^;]+);)"));

    QHash<QString, QString> vars;
    auto it = defRe.globalMatch(cssIn);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        vars.insert(m.captured(1).trimmed(), m.captured(2).trimmed());
    }
    if (vars.isEmpty())
        return cssIn;

    // Resolve references between definitions (e.g. `@define-color a @b;`).
    for (int pass = 0; pass < 5; ++pass) {
        bool changed = false;
        for (auto v = vars.begin(); v != vars.end(); ++v) {
            const QString expanded = substituteVars(v.value(), vars);
            if (expanded != v.value()) {
                v.value() = expanded;
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    QString css = cssIn;
    css.remove(defRe);
    return substituteVars(css, vars);
}

CssSimpleSelector parseSimpleSelector(const QString &raw)
{
    CssSimpleSelector sel;
    const QString s = raw.trimmed();

    if (s.isEmpty() || s == QStringLiteral("*")) {
        sel.universal = true;
        return sel;
    }

    int i = 0;
    while (i < s.length()) {
        const QChar c = s[i];
        if (c == QLatin1Char('*')) {
            sel.universal = true;
            ++i;
        } else if (c == QLatin1Char('#')) {
            int j = i + 1;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('_')))
                ++j;
            sel.id = s.mid(i + 1, j - i - 1);
            sel.specificity += 100;
            i = j;
        } else if (c == QLatin1Char('.')) {
            int j = i + 1;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('_')))
                ++j;
            sel.classes.append(s.mid(i + 1, j - i - 1));
            sel.specificity += 10;
            i = j;
        } else if (c == QLatin1Char(':')) {
            // Pseudo-classes (single colon, e.g. :hover) become state classes the
            // widget must supply at resolve time; pseudo-elements (::x) are ignored.
            int j = i + 1;
            bool pseudoElement = false;
            if (j < s.length() && s[j] == QLatin1Char(':')) {
                pseudoElement = true;
                ++j;
            }
            const int nameStart = j;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-')))
                ++j;
            if (!pseudoElement) {
                sel.classes.append(s.mid(nameStart, j - nameStart));
                sel.specificity += 10;
            } else {
                // Pseudo-elements (::before/::after) name a decorative overlay; keep
                // the name so resolve*() can target it instead of dropping it.
                sel.pseudoElement = s.mid(nameStart, j - nameStart);
                sel.specificity += 1;
            }
            i = j;
        } else if (c.isLetter() || c == QLatin1Char('_')) {
            int j = i;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('_')))
                ++j;
            sel.element = s.mid(i, j - i);
            sel.specificity += 1;
            i = j;
        } else {
            ++i;
        }
    }

    return sel;
}

struct FullSelectorParse {
    QString ancestorId;
    CssSimpleSelector subject;
};

// Split a full selector on combinators and extract the subject (last token)
// plus the ancestor id from the leading part.
FullSelectorParse parseFullSelector(const QString &fullSelector)
{
    static const QRegularExpression combinatorRe(QStringLiteral(R"(\s*[>+~]\s*|\s+)"));
    const QStringList parts = fullSelector.trimmed().split(combinatorRe, Qt::SkipEmptyParts);

    FullSelectorParse result;
    if (parts.isEmpty()) {
        result.subject.universal = true;
        return result;
    }

    result.subject = parseSimpleSelector(parts.last());

    for (int i = 0; i + 1 < parts.size(); ++i) {
        const CssSimpleSelector ancestor = parseSimpleSelector(parts.at(i));
        result.subject.specificity += ancestor.specificity;
        // Take the first #id found in the ancestor chain as context key
        if (!ancestor.id.isEmpty() && result.ancestorId.isEmpty())
            result.ancestorId = ancestor.id;
    }

    return result;
}

QVariantMap parseDeclarationBlock(const QString &block, QVariantMap *importantOut = nullptr)
{
    QVariantMap props;
    int i = 0;
    while (i < block.length()) {
        int end = block.indexOf(QLatin1Char(';'), i);
        if (end < 0)
            end = block.length();
        const QString decl = block.mid(i, end - i).trimmed();
        const int colon = decl.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            const QString prop = decl.left(colon).trimmed().toLower();
            QString val = decl.mid(colon + 1).trimmed();
            // CSS `!important`: strip a trailing `!important` (optional space) and flag it,
            // so the cascade can let it win over higher-specificity non-important rules.
            bool important = false;
            {
                const int bang = val.toLower().lastIndexOf(QLatin1String("!important"));
                if (bang >= 0 && val.mid(bang + 10).trimmed().isEmpty()) {
                    val = val.left(bang).trimmed();
                    important = true;
                }
            }
            // Normalize CSS color keywords that Qt cannot parse to an explicit
            // fully-transparent hex, in the engine, so every consumer gets a usable
            // value — a raw QML `color:` binding or a Canvas fillStyle, not just
            // parseColor(). Qt's QColor rejects `transparent`/`none`/`inherit` on those
            // raw paths and falls back to black; `#00000000` reads as transparent black
            // under both Qt's #AARRGGBB and CSS's #RRGGBBAA. Whole-value only: a gradient
            // keeps its inner `transparent` stop (parseGradient routes it through
            // parseColor) and substrings inside url()/font-family are never touched.
            // `transparent` is always a colour keyword so it normalizes on any property;
            // `none`/`inherit` mean "absent" on non-colour shorthands (border/box-shadow),
            // so they only fold on colour properties.
            const QString low = val.toLower();
            const bool colorProp = prop.endsWith(QLatin1String("color"))
                || prop == QLatin1String("fill")
                || prop == QLatin1String("background");
            if (low == QLatin1String("transparent")
                || (colorProp && (low == QLatin1String("none") || low == QLatin1String("inherit"))))
                val = QStringLiteral("#00000000");
            if (!prop.isEmpty()) {
                props.insert(prop, val);
                if (important && importantOut != nullptr)
                    importantOut->insert(prop, val);
            }
        }
        i = end + 1;
    }
    return props;
}

// Parse a `@keyframes` body — `0% { ... } 50% { ... } 100% { ... }` (also `from`/`to`,
// and comma-separated offsets) — into a list of { offset (0..1), properties }, sorted by
// offset. Reuses parseDeclarationBlock for each frame's declarations.
QVariantList parseKeyframeBlock(const QString &block)
{
    QVariantList frames;
    int i = 0;
    while (i < block.length()) {
        const int brace = block.indexOf(QLatin1Char('{'), i);
        if (brace < 0)
            break;
        const QString selector = block.mid(i, brace - i).trimmed();
        const int close = block.indexOf(QLatin1Char('}'), brace + 1);
        if (close < 0)
            break;
        const QVariantMap props = parseDeclarationBlock(block.mid(brace + 1, close - brace - 1));
        const QStringList sels = selector.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &s : sels) {
            const QString t = s.trimmed().toLower();
            double offset = -1.0;
            if (t == QLatin1String("from")) {
                offset = 0.0;
            } else if (t == QLatin1String("to")) {
                offset = 1.0;
            } else if (t.endsWith(QLatin1Char('%'))) {
                bool ok = false;
                const double pct = t.left(t.length() - 1).trimmed().toDouble(&ok);
                if (ok)
                    offset = pct / 100.0;
            }
            if (offset >= 0.0) {
                QVariantMap frame;
                frame.insert(QStringLiteral("offset"), offset);
                frame.insert(QStringLiteral("properties"), props);
                frames.append(frame);
            }
        }
        i = close + 1;
    }
    std::sort(frames.begin(), frames.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("offset")).toDouble()
            < b.toMap().value(QStringLiteral("offset")).toDouble();
    });
    return frames;
}

// Extract `@keyframes <name> { ... }` blocks (brace-matched, since they nest — which the
// flat parseCss cannot handle) into `out`, and return the CSS with them removed so the
// ordinary rule parser never sees them.
QString extractKeyframes(const QString &css, QHash<QString, QVariantList> &out)
{
    QString result;
    int i = 0;
    while (i < css.length()) {
        const int at = css.indexOf(QStringLiteral("@keyframes"), i, Qt::CaseInsensitive);
        if (at < 0) {
            result += css.mid(i);
            break;
        }
        result += css.mid(i, at - i);
        int j = at + 10; // past "@keyframes"
        while (j < css.length() && css[j].isSpace())
            ++j;
        const int nameStart = j;
        while (j < css.length() && css[j] != QLatin1Char('{') && !css[j].isSpace())
            ++j;
        const QString name = css.mid(nameStart, j - nameStart).trimmed();
        while (j < css.length() && css[j] != QLatin1Char('{'))
            ++j;
        if (j >= css.length())
            break;
        int depth = 0;
        int k = j;
        for (; k < css.length(); ++k) {
            if (css[k] == QLatin1Char('{')) {
                ++depth;
            } else if (css[k] == QLatin1Char('}')) {
                --depth;
                if (depth == 0) {
                    ++k;
                    break;
                }
            }
        }
        const QString inner = css.mid(j + 1, k - j - 2);
        if (!name.isEmpty())
            out.insert(name, parseKeyframeBlock(inner));
        i = k;
    }
    return result;
}

// Parse a single `@font-face { ... }` body into { family, url }. Reuses parseDeclarationBlock for
// the declarations; pulls the family name (quotes stripped) and the FIRST url() listed in `src`.
CssFontFace parseFontFace(const QString &body)
{
    CssFontFace face;
    const QVariantMap props = parseDeclarationBlock(body);

    QString family = props.value(QStringLiteral("font-family")).toString().trimmed();
    if ((family.startsWith(QLatin1Char('"')) && family.endsWith(QLatin1Char('"')))
        || (family.startsWith(QLatin1Char('\'')) && family.endsWith(QLatin1Char('\''))))
        family = family.mid(1, family.size() - 2).trimmed();
    face.family = family;

    const QString src = props.value(QStringLiteral("src")).toString();
    static const QRegularExpression urlRe(QStringLiteral(R"(url\(\s*['"]?([^'")]+)['"]?\s*\))"));
    const QRegularExpressionMatch m = urlRe.match(src);
    if (m.hasMatch())
        face.url = m.captured(1).trimmed();
    return face;
}

// Extract `@font-face { ... }` blocks (brace-matched) into `out` and return the CSS with them
// removed, so the ordinary rule parser never sees them (their `@font-face` "selector" would
// otherwise be glued onto the following rule). Mirrors extractKeyframes.
QString extractFontFaces(const QString &css, QList<CssFontFace> &out)
{
    QString result;
    int i = 0;
    while (i < css.length()) {
        const int at = css.indexOf(QStringLiteral("@font-face"), i, Qt::CaseInsensitive);
        if (at < 0) {
            result += css.mid(i);
            break;
        }
        result += css.mid(i, at - i);
        int j = at + 10; // past "@font-face"
        while (j < css.length() && css[j] != QLatin1Char('{'))
            ++j;
        if (j >= css.length())
            break;
        int depth = 0, k = j;
        for (; k < css.length(); ++k) {
            if (css[k] == QLatin1Char('{')) {
                ++depth;
            } else if (css[k] == QLatin1Char('}')) {
                --depth;
                if (depth == 0) {
                    ++k;
                    break;
                }
            }
        }
        const CssFontFace face = parseFontFace(css.mid(j + 1, k - j - 2));
        if (!face.family.isEmpty() && !face.url.isEmpty())
            out.append(face);
        i = k;
    }
    return result;
}

QList<CssRule> parseCss(const QString &css)
{
    QList<CssRule> rules;
    const QString cleaned = expandDefineColors(stripComments(css));

    int i = 0;
    while (i < cleaned.length()) {
        const int blockStart = cleaned.indexOf(QLatin1Char('{'), i);
        if (blockStart < 0)
            break;

        const int blockEnd = cleaned.indexOf(QLatin1Char('}'), blockStart + 1);
        if (blockEnd < 0)
            break;

        QString selectorPart = cleaned.mid(i, blockStart - i);
        // Discard any preceding at-rule statements (e.g. "@define-color name #hex;"),
        // which have no {} block of their own and would otherwise be glued onto the
        // next rule's selector text. Selectors never contain ';', so only keep what
        // follows the last one.
        const int lastSemicolon = selectorPart.lastIndexOf(QLatin1Char(';'));
        if (lastSemicolon >= 0)
            selectorPart = selectorPart.mid(lastSemicolon + 1);
        selectorPart = selectorPart.trimmed();
        const QString blockContent = cleaned.mid(blockStart + 1, blockEnd - blockStart - 1);

        if (!selectorPart.isEmpty()) {
            QVariantMap important;
            const QVariantMap props = parseDeclarationBlock(blockContent, &important);
            if (!props.isEmpty()) {
                const QStringList selectors = selectorPart.split(QLatin1Char(','), Qt::SkipEmptyParts);
                for (const QString &sel : selectors) {
                    const FullSelectorParse parsed = parseFullSelector(sel.trimmed());
                    rules.append({parsed.ancestorId, parsed.subject, props, important});
                }
            }
        }

        i = blockEnd + 1;
    }

    return rules;
}

bool selectorMatches(const CssSimpleSelector &sel, const QString &contextId, const QString &id,
                     const QStringList &classes, const QString &pseudoElement, const QString &primitive)
{
    // A pseudo-element rule (`::before`) only matches when that overlay is requested,
    // and an ordinary rule only matches when no overlay is requested.
    if (sel.pseudoElement != pseudoElement)
        return false;
    // A type selector (`button`, `input`) matches the element's primitive — the component
    // exposes `cssPrimitive`, so the engine matches against it directly (no class needed).
    if (!sel.element.isEmpty() && sel.element != primitive)
        return false;
    // A selector that constrains nothing at the top level (no id, class, or type) is too broad
    // for component styling; allow it only with an ancestor context (i.e. via resolveWith).
    if (!sel.universal && sel.id.isEmpty() && sel.classes.isEmpty() && sel.element.isEmpty()
        && contextId.isEmpty())
        return false;
    if (!sel.id.isEmpty() && sel.id != id)
        return false;
    for (const QString &cls : sel.classes) {
        if (!classes.contains(cls))
            return false;
    }
    return true;
}

// CSS gradient side keywords → angle (0deg points up, increasing clockwise).
double cssSideToAngle(const QString &sideRaw)
{
    const QString side = sideRaw.toLower().simplified();
    if (side == QLatin1String("top")) return 0.0;
    if (side == QLatin1String("right")) return 90.0;
    if (side == QLatin1String("bottom")) return 180.0;
    if (side == QLatin1String("left")) return 270.0;
    if (side == QLatin1String("top right") || side == QLatin1String("right top")) return 45.0;
    if (side == QLatin1String("bottom right") || side == QLatin1String("right bottom")) return 135.0;
    if (side == QLatin1String("bottom left") || side == QLatin1String("left bottom")) return 225.0;
    if (side == QLatin1String("top left") || side == QLatin1String("left top")) return 315.0;
    return 180.0;
}

} // namespace

CssTheme::CssTheme(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &CssTheme::onCssFileChanged);
}

void CssTheme::load(const QString &path)
{
    loadLayered(QStringList{path});
}

void CssTheme::setStylePrelude(const QString &css)
{
    m_stylePrelude = css; // applied on the next loadLayered() the caller triggers
}

void CssTheme::loadLayered(const QStringList &paths)
{
    m_requestedLayer = paths; // remembered so a file-change reload re-reads the whole layer

    QString combined;
    QStringList present;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "CssTheme: cannot open CSS file" << path;
            continue;
        }
        present.append(path);
        // Inline each file's @imports relative to ITS OWN directory, then concatenate so
        // later sheets (the theme) cascade over earlier ones (the base).
        QStringList visited{QFileInfo(path).absoluteFilePath()};
        combined += expandImports(QString::fromUtf8(file.readAll()),
                                  QFileInfo(path).absolutePath(), visited);
        combined += QLatin1Char('\n');
    }

    // Prepend the config-derived prelude (lowest priority) so the theme CSS — concatenated
    // after it — overrides it on equal specificity. The CSS is thus the single source of truth.
    if (!m_stylePrelude.isEmpty())
        combined = m_stylePrelude + QLatin1Char('\n') + combined;

    // Append the generated layer (highest priority) so synthesised inline-style rules —
    // e.g. `style="..."` props turned into `#id { ... }` — win the cascade over the files.
    if (!m_generatedCss.isEmpty())
        combined += m_generatedCss + QLatin1Char('\n');

    // Watch every present file; drop watches for files no longer in the layer. (Editors
    // atomically replace files, dropping them from the watch list — re-add on each load.)
    const QStringList watched = m_watcher->files();
    for (const QString &w : watched)
        if (!present.contains(w))
            m_watcher->removePath(w);
    for (const QString &p : present)
        if (!m_watcher->files().contains(p))
            m_watcher->addPath(p);

    const QByteArray hash = QCryptographicHash::hash(combined.toUtf8(), QCryptographicHash::Md5);
    if (hash == m_contentHash)
        return; // content unchanged — touch or spurious notification
    m_contentHash = hash;
    loadFromString(combined);
}

void CssTheme::loadLayeredString(const QString &generatedCss)
{
    // The generated layer is content the build produced (not a file): inline-style rules
    // the transpiler synthesised. Store it and re-run the layered load so it's appended last
    // (highest priority) and survives file-change reloads.
    if (generatedCss == m_generatedCss && m_loaded)
        return;
    m_generatedCss = generatedCss;
    m_contentHash.clear(); // force a rebuild — only the generated layer changed
    loadLayered(m_requestedLayer);
}

void CssTheme::onCssFileChanged(const QString &)
{
    // Any file in the layer changed — re-read the whole layer (base + theme).
    loadLayered(m_requestedLayer);
}

void CssTheme::loadFromString(const QString &css)
{
    // @keyframes blocks nest, which the flat parseCss can't handle — extract them first
    // (on the comment-stripped, @define-color-expanded source so frame values resolve).
    const QString cleaned = expandDefineColors(stripComments(css));
    m_keyframes.clear();
    const QString afterKeyframes = extractKeyframes(cleaned, m_keyframes);

    // Pull @font-face blocks aside (they nest declarations, like @keyframes) so the flat parser
    // never sees them; each declares a web font to download + register with QFontDatabase.
    QList<CssFontFace> fontFaces;
    const QString afterFonts = extractFontFaces(afterKeyframes, fontFaces);

    // Pull @media blocks aside (and drop @supports/@container); parse the base rules plus each
    // media block's rules. rebuildRules() then assembles the active cascade for the viewport.
    QList<RawMediaBlock> rawMedia;
    const QString body = extractAtRules(afterFonts, rawMedia);
    m_baseRules = parseCss(body);
    m_mediaGroups.clear();
    for (const RawMediaBlock &blk : rawMedia)
        m_mediaGroups.append({ parseMediaCondition(blk.condition), parseCss(blk.body) });

    rebuildRules();
    m_loaded = true;
    emit loadedChanged();
    // Reverse slot: push freshly-resolved styles to every registered object so a theme
    // reload re-styles the live UI without any QML binding.
    reapplyAll();

    // Load/download each @font-face (deduped by URL). Cached fonts register synchronously here;
    // uncached ones arrive later and bump fontRevision, re-resolving the text that uses them.
    for (const CssFontFace &face : fontFaces)
        registerFontFace(face);
}

void CssTheme::rebuildRules()
{
    m_rules = m_baseRules;
    // Append matching @media rules AFTER the base so they override on equal specificity
    // (CSS source order: @media blocks follow and refine the base).
    for (const CssMediaGroup &group : m_mediaGroups) {
        if (mediaMatches(group.query, m_viewportWidth, m_viewportHeight))
            m_rules += group.rules;
    }
}

void CssTheme::registerFontData(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    const int id = QFontDatabase::addApplicationFontFromData(bytes);
    if (id < 0) {
        qWarning() << "CssTheme @font-face: QFontDatabase rejected the font data";
        return;
    }
    // The family is now installed in QFontDatabase, so resolveFontFamily() will find it. Bump the
    // revision (text bindings observe it and re-resolve) and re-push styles to registered objects.
    ++m_fontRevision;
    emit fontRevisionChanged();
    reapplyAll();
}

void CssTheme::registerFontFace(const CssFontFace &face)
{
    const QString url = face.url;
    // Dedupe: a font is fetched/registered once, even though loadFromString runs on every reload.
    if (url.isEmpty() || m_fontFacesSeen.contains(url))
        return;
    m_fontFacesSeen.insert(url);

    const bool remote = url.startsWith(QLatin1String("http://")) || url.startsWith(QLatin1String("https://"));
    if (!remote) {
        // A local file:// or filesystem path — register straight from disk.
        const QString path = url.startsWith(QLatin1String("file://")) ? QUrl(url).toLocalFile() : url;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            registerFontData(file.readAll());
        else
            qWarning() << "CssTheme @font-face: cannot read local font" << path;
        return;
    }

    // Disk cache keyed by a hash of the URL, so a launch never re-downloads (fast + offline).
    const QByteArray key = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/webfonts");
    QDir().mkpath(cacheDir);
    const QString cachePath = cacheDir + QLatin1Char('/') + QString::fromLatin1(key);

    QFile cached(cachePath);
    if (cached.exists() && cached.open(QIODevice::ReadOnly)) {
        registerFontData(cached.readAll());
        return;
    }

    // Cache miss: download asynchronously (never block the UI thread). HTTP/2 is disabled to
    // dodge the QNAM connection-reuse bug (see webfetch.cpp).
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    QNetworkRequest request{ QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cachePath] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "CssTheme @font-face: download failed" << reply->url().toString()
                       << reply->errorString();
            return;
        }
        const QByteArray fontBytes = reply->readAll();
        if (fontBytes.isEmpty())
            return;
        // Persist to the cache for next launch, then register.
        QFile out(cachePath);
        if (out.open(QIODevice::WriteOnly))
            out.write(fontBytes);
        registerFontData(fontBytes);
    });
}

void CssTheme::setViewport(qreal width, qreal height)
{
    if (qFuzzyCompare(width, m_viewportWidth) && qFuzzyCompare(height, m_viewportHeight))
        return;
    m_viewportWidth = width;
    m_viewportHeight = height;
    emit viewportChanged();
    if (!m_loaded)
        return;
    rebuildRules();
    reapplyAll();
}

QVariantMap CssTheme::parseAnimation(const QString &cssValue) const
{
    return CssValueParser::parseAnimation(cssValue);
}

QVariantList CssTheme::keyframes(const QString &name) const
{
    return m_keyframes.value(name);
}

// Coerce a CssQmlItem identity property (cssAlternateId / cssClass) to a string list:
// it may be authored as a single string ("waybar", "focused"), a space-separated string,
// or a QML list (["network", "nm-applet"]).
static QStringList cssVariantToStringList(const QVariant &v)
{
    if (!v.isValid())
        return {};
    // A QML `var` array (e.g. `cssClass: active ? ["active"] : []`) arrives here wrapped as a
    // QJSValue, NOT a QVariantList — unwrap it to its variant form before coercing, otherwise
    // it falls through to toString() (empty) and every state class is silently dropped.
    if (v.metaType().id() == qMetaTypeId<QJSValue>())
        return cssVariantToStringList(v.value<QJSValue>().toVariant());
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

QVariantMap CssTheme::resolveMerged(const QString &cssId, const QStringList &alternateIds,
                                    const QStringList &classes, const QString &primitive) const
{
    QVariantMap merged = resolve(QString(), classes, QString(), primitive);
    // Waybar-compat aliases first (lowest priority), then the primary id wins on conflict.
    for (const QString &alt : alternateIds) {
        if (alt.isEmpty())
            continue;
        const QVariantMap m = resolve(alt, classes, QString(), primitive);
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            merged.insert(it.key(), it.value());
    }
    if (!cssId.isEmpty()) {
        const QVariantMap primary = resolve(cssId, classes, QString(), primitive);
        for (auto it = primary.constBegin(); it != primary.constEnd(); ++it)
            merged.insert(it.key(), it.value());
    }
    return merged;
}

void CssTheme::applyCssTo(QObject *target) const
{
    if (!target)
        return;

    // Identity is read straight off the CssQmlItem target (re-read every apply, so a
    // later cssClass change is reflected on the next apply / reload).
    const QString cssId = target->property("cssId").toString();
    // Authored classes + runtime state classes (`cssState`: hover/active/focus/…) so
    // pseudo-class rules like `.counter:hover` resolve when the widget reports the state.
    QStringList classes = cssVariantToStringList(target->property("cssClass"));
    classes += cssVariantToStringList(target->property("cssState"));
    const QString cssPart = target->property("cssPart").toString();
    // Type selectors (`button {}`) match this element's primitive.
    const QString cssPrimitive = target->property("cssPrimitive").toString();

    // A part target (`#tray.item`, `#cpu.graph`) resolves ONLY that part — excluding the
    // bare `#id` base — so a sub-element doesn't inherit the container's own background.
    // Otherwise merge the waybar alias(es) under the primary id.
    QVariantMap style;
    if (!cssPart.isEmpty()) {
        style = resolvePart(cssId, cssPart, classes);
    } else {
        const QStringList alternateIds = cssVariantToStringList(target->property("cssAlternateId"));
        style = resolveMerged(cssId, alternateIds, classes, cssPrimitive);
    }

    // Push the resolved map into the target's `style` sink; its renderer keys off it
    // (gradient/box-shadow/bevel with the alpha-fix a plain Rectangle cannot do).
    target->setProperty("style", style);
}

void CssTheme::loadCss(QObject *target)
{
    if (!target)
        return;

    // The engine's one guarantee: the target must carry the CssQmlItem signature
    // (identity `cssId` + a `style` sink). Without it loadCss would resolve nothing and
    // style nothing — i.e. "quebra o brinquedo silenciosamente". So fail LOUDLY instead.
    const QMetaObject *mo = target->metaObject();
    if (mo->indexOfProperty("cssId") < 0 || mo->indexOfProperty("style") < 0) {
        qWarning().noquote() << "CssTheme::loadCss: target" << mo->className()
                             << "lacks the CssQmlItem signature (needs cssId + style) — refusing to style it.";
        return;
    }

    // Register once; the reverse slot re-reads the object's identity on every (re)load.
    if (!m_bindings.contains(QPointer<QObject>(target))) {
        m_bindings.append(QPointer<QObject>(target));
        // The reverse slot must never dangle: drop the registration when the object dies.
        connect(target, &QObject::destroyed, this, [this](QObject *obj) {
            m_bindings.removeIf([obj](const QPointer<QObject> &p) { return p.isNull() || p == obj; });
        });
        // Observe the registered cssClass AND cssState: a state change (focused/urgent/
        // hover/active) must re-style automatically, so the applet registers once and never
        // re-calls loadCss.
        const QMetaMethod reapplySlot = staticMetaObject.method(
            staticMetaObject.indexOfSlot("reapplyForSender()"));
        for (const char *stateProp : { "cssClass", "cssState" }) {
            const int idx = mo->indexOfProperty(stateProp);
            if (idx < 0)
                continue;
            const QMetaProperty prop = mo->property(idx);
            if (prop.hasNotifySignal())
                connect(target, prop.notifySignal(), this, reapplySlot);
        }
    }

    applyCssTo(target);
}

void CssTheme::reapplyForSender()
{
    if (QObject *target = sender())
        applyCssTo(target);
}

void CssTheme::reapplyAll()
{
    m_bindings.removeIf([](const QPointer<QObject> &p) { return p.isNull(); });
    for (const QPointer<QObject> &p : m_bindings) {
        if (p)
            applyCssTo(p);
    }
}

QVariantMap CssTheme::resolve(const QString &id, const QStringList &classes, const QString &pseudoElement,
                              const QString &primitive) const
{
    return resolveImpl(QString(), id, classes, pseudoElement, primitive);
}

QVariantMap CssTheme::resolveWith(const QString &contextId, const QString &id, const QStringList &classes,
                                  const QString &pseudoElement) const
{
    return resolveImpl(contextId, id, classes, pseudoElement);
}

QVariantMap CssTheme::resolveImpl(const QString &contextId, const QString &id, const QStringList &classes,
                                  const QString &pseudoElement, const QString &primitive) const
{
    struct Match {
        int specificity;
        int sourceOrder;
        QVariantMap props;
        QVariantMap important;
    };

    QList<Match> matches;
    for (int i = 0; i < m_rules.size(); ++i) {
        const CssRule &rule = m_rules.at(i);
        // A rule with a required ancestor only matches when called with that ancestor as context.
        // Rules with no ancestor requirement match in all contexts.
        if (!rule.requiredAncestorId.isEmpty() && rule.requiredAncestorId != contextId)
            continue;
        if (selectorMatches(rule.selector, contextId, id, classes, pseudoElement, primitive))
            matches.append({rule.selector.specificity, i, rule.properties, rule.importantProperties});
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        return a.specificity != b.specificity ? a.specificity < b.specificity : a.sourceOrder < b.sourceOrder;
    });

    QVariantMap result;
    for (const Match &m : matches) {
        for (auto it = m.props.constBegin(); it != m.props.constEnd(); ++it)
            result.insert(it.key(), it.value());
    }
    // `!important` overlay: re-apply important declarations (in specificity order) on top,
    // so they win over any non-important declaration regardless of its specificity.
    for (const Match &m : matches) {
        for (auto it = m.important.constBegin(); it != m.important.constEnd(); ++it)
            result.insert(it.key(), it.value());
    }
    return result;
}

QVariantMap CssTheme::resolveExact(const QString &id, const QStringList &classes, const QString &pseudoElement,
                                   const QString &requiredClass) const
{
    struct Match {
        int specificity;
        int sourceOrder;
        QVariantMap props;
        QVariantMap important;
    };

    QList<Match> matches;
    for (int i = 0; i < m_rules.size(); ++i) {
        const CssRule &rule = m_rules.at(i);
        if (!rule.requiredAncestorId.isEmpty())
            continue;
        const CssSimpleSelector &sel = rule.selector;
        if (sel.id != id || sel.pseudoElement != pseudoElement)
            continue;
        // `requiredClass` (used by resolvePart) restricts to rules carrying that class,
        // e.g. `#cpu.graph`, so the bare `#cpu` base is excluded.
        if (!requiredClass.isEmpty() && !sel.classes.contains(requiredClass))
            continue;
        // Every other class the selector demands must be a supplied state (the required
        // class is implicit), so `#cpu.graph:active` matches only when "active" is asked.
        bool ok = true;
        for (const QString &cls : sel.classes)
            if (cls != requiredClass && !classes.contains(cls)) { ok = false; break; }
        if (!ok)
            continue;
        matches.append({sel.specificity, i, rule.properties, rule.importantProperties});
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        return a.specificity != b.specificity ? a.specificity < b.specificity : a.sourceOrder < b.sourceOrder;
    });

    QVariantMap result;
    for (const Match &m : matches)
        for (auto it = m.props.constBegin(); it != m.props.constEnd(); ++it)
            result.insert(it.key(), it.value());
    for (const Match &m : matches) // !important overlay (see resolveImpl)
        for (auto it = m.important.constBegin(); it != m.important.constEnd(); ++it)
            result.insert(it.key(), it.value());
    return result;
}

QVariantMap CssTheme::resolvePart(const QString &id, const QString &part, const QStringList &classes,
                                  const QString &pseudoElement) const
{
    // A "part" is an exact-match query that REQUIRES the part class — see resolveExact.
    return resolveExact(id, classes, pseudoElement, part);
}

QColor CssTheme::parseColor(const QString &cssColor) const
{
    return CssValueParser::parseColor(cssColor);
}

qreal CssTheme::parseLength(const QString &value, qreal fallback) const
{
    double out = 0.0;
    if (CssValueParser::parseLengthPx(value, &out))
        return out;
    return fallback;
}

qreal CssTheme::parseFontSize(const QString &value, qreal fallbackPt) const
{
    // Resolve a CSS font-size to POINTS (QML Text.pointSize), centrally, so no component
    // hand-converts units. CSS px are device-independent pixels → points ×72/96; pt is
    // points; em/rem scale `fallbackPt` (the element's base size); a bare number is points.
    const QString s = value.trimmed().toLower();
    // Forgiving number+unit parse so "13px" / "10pt" / "1.5rem" / "11" all work.
    static const QRegularExpression numRe(QStringLiteral("^\\s*(-?\\d*\\.?\\d+)\\s*([a-z%]*)\\s*$"));
    const QRegularExpressionMatch m = numRe.match(s);
    if (!m.hasMatch())
        return fallbackPt;
    const double num = m.captured(1).toDouble();
    const QString unit = m.captured(2);
    if (unit == QLatin1String("px"))
        return num * 72.0 / 96.0;
    if (unit == QLatin1String("em") || unit == QLatin1String("rem"))
        return num * fallbackPt;
    return num; // pt or unitless → points
}

namespace {
QVector<qreal> cssBox(const QString &value)
{
    if (value.trimmed().isEmpty())
        return { 0, 0, 0, 0 };
    const QStringList parts = CssValueParser::splitTopLevelWhitespace(value.trimmed());
    QVector<qreal> n;
    for (int i = 0; i < parts.size() && i < 4; ++i) {
        double v = 0;
        n.push_back(CssValueParser::parseLengthPx(parts.at(i), &v) ? v : 0);
    }
    if (n.size() == 1) return { n[0], n[0], n[0], n[0] };
    if (n.size() == 2) return { n[0], n[1], n[0], n[1] };
    if (n.size() == 3) return { n[0], n[1], n[2], n[1] };
    if (n.size() >= 4) return { n[0], n[1], n[2], n[3] };
    return { 0, 0, 0, 0 };
}

QString sideName(int side)
{
    static const QString names[4] = {
        QStringLiteral("top"), QStringLiteral("right"),
        QStringLiteral("bottom"), QStringLiteral("left"),
    };
    return names[std::clamp(side, 0, 3)];
}
} // namespace

int CssTheme::fontWeight(const QString &value) const
{
    const QString s = value.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("normal") || s == QLatin1String("400"))
        return QFont::Normal;
    if (s == QLatin1String("bold") || s == QLatin1String("700"))
        return QFont::Bold;
    if (s == QLatin1String("bolder"))
        return QFont::ExtraBold;
    if (s == QLatin1String("lighter"))
        return QFont::Light;

    bool ok = false;
    const int n = s.toInt(&ok);
    if (!ok) return QFont::Normal;
    if (n <= 100) return QFont::Thin;
    if (n <= 300) return QFont::Light;
    if (n <= 400) return QFont::Normal;
    if (n <= 500) return QFont::Medium;
    if (n <= 600) return QFont::DemiBold;
    if (n <= 700) return QFont::Bold;
    if (n <= 800) return QFont::ExtraBold;
    return QFont::Black;
}

bool CssTheme::isProportionalLineHeight(const QString &value) const
{
    const QString s = value.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("normal"))
        return true;
    static const QRegularExpression unitRe(QStringLiteral("[a-z%]"));
    return !unitRe.match(s).hasMatch();
}

qreal CssTheme::parseLineHeight(const QString &value, qreal fallbackPx) const
{
    const QString s = value.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("normal"))
        return 1.0;
    if (isProportionalLineHeight(s)) {
        bool ok = false;
        const qreal n = s.toDouble(&ok);
        return ok ? n : 1.0;
    }
    return parseLength(s, fallbackPx);
}

qreal CssTheme::boxSideLength(const QVariantMap &style, const QString &prefix, int side) const
{
    const QVector<qreal> box = cssBox(style.value(prefix).toString());
    const QString key = prefix + QLatin1Char('-') + sideName(side);
    const auto it = style.constFind(key);
    if (it == style.constEnd())
        return box.value(std::clamp(side, 0, 3), 0);
    return parseLength(it->toString(), box.value(std::clamp(side, 0, 3), 0));
}

bool CssTheme::hasSideBorder(const QVariantMap &style) const
{
    static const QStringList sides = { QStringLiteral("top"), QStringLiteral("right"),
                                      QStringLiteral("bottom"), QStringLiteral("left") };
    for (const QString &side : sides) {
        if (style.contains(QStringLiteral("border-") + side)
            || style.contains(QStringLiteral("border-") + side + QStringLiteral("-width"))
            || style.contains(QStringLiteral("border-") + side + QStringLiteral("-color"))
            || style.contains(QStringLiteral("border-") + side + QStringLiteral("-style")))
            return true;
    }
    return false;
}

QVariantMap CssTheme::borderSide(const QVariantMap &style, const QString &side,
                                 qreal defaultWidth, const QColor &defaultColor) const
{
    const QString prefix = QStringLiteral("border-") + side;
    const QVariantMap shorthand = style.contains(prefix) ? parseBorder(style.value(prefix).toString()) : QVariantMap();

    const qreal width = style.contains(prefix + QStringLiteral("-width"))
        ? parseLength(style.value(prefix + QStringLiteral("-width")).toString(), 0)
        : shorthand.value(QStringLiteral("width"), defaultWidth).toReal();
    const QColor color = style.contains(prefix + QStringLiteral("-color"))
        ? parseColor(style.value(prefix + QStringLiteral("-color")).toString())
        : shorthand.value(QStringLiteral("color"), defaultColor).value<QColor>();
    const QString borderStyle = style.contains(prefix + QStringLiteral("-style"))
        ? style.value(prefix + QStringLiteral("-style")).toString()
        : shorthand.value(QStringLiteral("style"), QStringLiteral("solid")).toString();

    QVariantMap out;
    out.insert(QStringLiteral("width"), width);
    out.insert(QStringLiteral("color"), color);
    out.insert(QStringLiteral("style"), borderStyle);
    out.insert(QStringLiteral("visible"), width > 0 && color.alphaF() > 0
               && borderStyle != QLatin1String("none") && borderStyle != QLatin1String("hidden"));
    return out;
}

QString CssTheme::resolveFontFamily(const QString &value, const QString &fallback) const
{
    const QStringList families = QFontDatabase::families();
    const auto hasFamily = [&](const QString &candidate) {
        return std::any_of(families.cbegin(), families.cend(), [&](const QString &family) {
            return family.compare(candidate, Qt::CaseInsensitive) == 0;
        });
    };
    const auto clean = [](QString candidate) {
        candidate = candidate.trimmed();
        if ((candidate.startsWith(QLatin1Char('"')) && candidate.endsWith(QLatin1Char('"')))
            || (candidate.startsWith(QLatin1Char('\'')) && candidate.endsWith(QLatin1Char('\''))))
            candidate = candidate.mid(1, candidate.size() - 2).trimmed();
        return candidate;
    };
    const auto alias = [&](const QString &candidate) -> QString {
        const QString low = candidate.toLower();
        if (low == QLatin1String("sans-serif"))
            return hasFamily(QStringLiteral("Noto Sans")) ? QStringLiteral("Noto Sans") : QStringLiteral("Sans Serif");
        if (low == QLatin1String("serif"))
            return hasFamily(QStringLiteral("Noto Serif")) ? QStringLiteral("Noto Serif") : QStringLiteral("Serif");
        if (low == QLatin1String("monospace"))
            return hasFamily(QStringLiteral("Noto Sans Mono")) ? QStringLiteral("Noto Sans Mono") : QStringLiteral("Monospace");
        if (low == QLatin1String("helvetica") || low == QLatin1String("helvetica neue"))
            return hasFamily(QStringLiteral("Nimbus Sans")) ? QStringLiteral("Nimbus Sans") : QString();
        if (low == QLatin1String("arial"))
            return hasFamily(QStringLiteral("Liberation Sans")) ? QStringLiteral("Liberation Sans") : QString();
        return QString();
    };

    for (const QString &part : CssValueParser::splitTopLevel(value, QLatin1Char(','))) {
        const QString candidate = clean(part);
        if (candidate.isEmpty())
            continue;
        if (hasFamily(candidate))
            return candidate;
        const QString mapped = alias(candidate);
        if (!mapped.isEmpty())
            return mapped;
    }
    if (!fallback.isEmpty())
        return resolveFontFamily(fallback, QString());
    return hasFamily(QStringLiteral("Noto Sans")) ? QStringLiteral("Noto Sans") : QStringLiteral("Sans Serif");
}

// Parse the colour stops of a gradient (the parts after any head token), distributing
// positions evenly where omitted. Returns an empty list if there are fewer than 2 stops.
static QVariantList parseGradientStops(const QStringList &parts, int firstStop)
{
    QList<QColor> colors;
    QList<double> positions;
    for (int i = firstStop; i < parts.size(); ++i) {
        const QStringList toks = CssValueParser::splitTopLevelWhitespace(parts.at(i));
        if (toks.isEmpty())
            continue;
        colors.append(CssValueParser::parseColor(toks.first()));
        double pos = -1.0;
        if (toks.size() >= 2 && toks.at(1).endsWith(QLatin1Char('%')))
            pos = toks.at(1).left(toks.at(1).length() - 1).toDouble() / 100.0;
        positions.append(pos);
    }
    if (colors.size() < 2)
        return {};

    QVariantList stops;
    for (int i = 0; i < colors.size(); ++i) {
        double pos = positions.at(i);
        if (pos < 0.0)
            pos = static_cast<double>(i) / static_cast<double>(colors.size() - 1);
        QVariantMap stop;
        stop.insert(QStringLiteral("position"), pos);
        stop.insert(QStringLiteral("color"), colors.at(i));
        stops.append(stop);
    }
    return stops;
}

// A CSS position token to a 0..1 fraction (percentage or keyword); `fallback` otherwise.
static double cssPositionFraction(const QString &token, double fallback)
{
    const QString t = token.trimmed();
    if (t.endsWith(QLatin1Char('%')))
        return t.left(t.length() - 1).toDouble() / 100.0;
    if (t.compare(QLatin1String("left"), Qt::CaseInsensitive) == 0
        || t.compare(QLatin1String("top"), Qt::CaseInsensitive) == 0)
        return 0.0;
    if (t.compare(QLatin1String("center"), Qt::CaseInsensitive) == 0)
        return 0.5;
    if (t.compare(QLatin1String("right"), Qt::CaseInsensitive) == 0
        || t.compare(QLatin1String("bottom"), Qt::CaseInsensitive) == 0)
        return 1.0;
    return fallback;
}

QVariantMap CssTheme::parseGradient(const QString &cssValue) const
{
    const QString s = cssValue.trimmed();
    if (!s.endsWith(QLatin1Char(')')))
        return {};

    const bool radial = s.startsWith(QLatin1String("radial-gradient("), Qt::CaseInsensitive);
    const bool linear = s.startsWith(QLatin1String("linear-gradient("), Qt::CaseInsensitive);
    if (!radial && !linear)
        return {};

    const QString prefix = radial ? QStringLiteral("radial-gradient(") : QStringLiteral("linear-gradient(");
    const QString inner = s.mid(prefix.length(), s.length() - prefix.length() - 1);
    QStringList parts = CssValueParser::splitTopLevel(inner, QLatin1Char(','));
    for (QString &p : parts)
        p = p.trimmed();
    if (parts.isEmpty())
        return {};

    if (radial) {
        // radial-gradient([<shape> | <size>] [at <pos>], <stops>). Only the centre `at`
        // position is honoured (cx/cy as 0..1 fractions); shape/size default to a centred
        // ellipse covering the box.
        double cx = 0.5, cy = 0.5;
        int firstStop = 0;
        const QString head = parts.first();
        const bool headIsConfig = head.contains(QLatin1String(" at "), Qt::CaseInsensitive)
            || head.startsWith(QLatin1String("circle"), Qt::CaseInsensitive)
            || head.startsWith(QLatin1String("ellipse"), Qt::CaseInsensitive);
        if (headIsConfig) {
            firstStop = 1;
            const int atIdx = head.indexOf(QLatin1String("at "), 0, Qt::CaseInsensitive);
            if (atIdx >= 0) {
                const QStringList pos = CssValueParser::splitTopLevelWhitespace(head.mid(atIdx + 3));
                if (pos.size() >= 1) cx = cssPositionFraction(pos.at(0), 0.5);
                cy = pos.size() >= 2 ? cssPositionFraction(pos.at(1), 0.5) : cx;
            }
        }
        const QVariantList stops = parseGradientStops(parts, firstStop);
        if (stops.isEmpty())
            return {};
        QVariantMap result;
        result.insert(QStringLiteral("type"), QStringLiteral("radial"));
        result.insert(QStringLiteral("cx"), cx);
        result.insert(QStringLiteral("cy"), cy);
        result.insert(QStringLiteral("stops"), stops);
        return result;
    }

    double angle = 180.0; // CSS default direction: "to bottom"
    int firstStop = 0;
    const QString head = parts.first();
    if (head.endsWith(QLatin1String("deg"), Qt::CaseInsensitive)) {
        angle = head.left(head.length() - 3).trimmed().toDouble();
        firstStop = 1;
    } else if (head.startsWith(QLatin1String("to "), Qt::CaseInsensitive)) {
        angle = cssSideToAngle(head.mid(3));
        firstStop = 1;
    }

    const QVariantList stops = parseGradientStops(parts, firstStop);
    if (stops.isEmpty())
        return {};

    QVariantMap result;
    result.insert(QStringLiteral("type"), QStringLiteral("linear"));
    result.insert(QStringLiteral("angle"), angle);
    result.insert(QStringLiteral("stops"), stops);
    return result;
}

QVariantMap CssTheme::parseBorder(const QString &cssValue) const
{
    // `border` shorthand: <width> <style> <color> (any order, any subset). Returns
    // { width (px), style, color (QColor) } for the parts present.
    QVariantMap out;
    const QStringList toks = CssValueParser::splitTopLevelWhitespace(cssValue.trimmed());
    static const QStringList styleKeywords = {
        QStringLiteral("none"), QStringLiteral("hidden"), QStringLiteral("solid"),
        QStringLiteral("dashed"), QStringLiteral("dotted"), QStringLiteral("double"),
        QStringLiteral("groove"), QStringLiteral("ridge"), QStringLiteral("inset"),
        QStringLiteral("outset"),
    };
    for (const QString &tok : toks) {
        double w = 0.0;
        if (CssValueParser::parseLengthPx(tok, &w)) {
            out.insert(QStringLiteral("width"), w);
            continue;
        }
        if (styleKeywords.contains(tok, Qt::CaseInsensitive)) {
            out.insert(QStringLiteral("style"), tok.toLower());
            continue;
        }
        const QColor c = CssValueParser::parseColor(tok);
        if (c.isValid())
            out.insert(QStringLiteral("color"), c);
    }
    return out;
}

QVariantList CssTheme::parseGradientLayers(const QString &cssValue) const
{
    // CSS `background` can stack several comma-separated layers (the FIRST listed paints on
    // TOP). Each layer is a gradient or a solid colour. Top-level commas separate layers
    // (commas inside gradient()/rgba() are protected by splitTopLevel).
    QVariantList layers;
    const QStringList segments = CssValueParser::splitTopLevel(cssValue, QLatin1Char(','));
    for (const QString &segment : segments) {
        const QString t = segment.trimmed();
        if (t.isEmpty())
            continue;
        if (t.startsWith(QLatin1String("linear-gradient("), Qt::CaseInsensitive)
            || t.startsWith(QLatin1String("radial-gradient("), Qt::CaseInsensitive)) {
            const QVariantMap g = parseGradient(t);
            if (!g.isEmpty())
                layers.append(g);
        } else {
            const QColor c = CssValueParser::parseColor(t);
            if (c.isValid()) {
                QVariantMap m;
                m.insert(QStringLiteral("type"), QStringLiteral("color"));
                m.insert(QStringLiteral("color"), c);
                layers.append(m);
            }
        }
    }
    return layers;
}

static QVariantMap parseOneBoxShadow(const QString &segment)
{
    const QStringList toks = CssValueParser::splitTopLevelWhitespace(segment.trimmed());

    bool inset = false;
    QList<double> lengths;
    QColor color(0, 0, 0, 128);
    for (const QString &tok : toks) {
        if (tok.compare(QLatin1String("inset"), Qt::CaseInsensitive) == 0) {
            inset = true;
            continue;
        }
        double value = 0.0;
        if (CssValueParser::parseLengthPx(tok, &value)) {
            lengths.append(value);
            continue;
        }
        color = CssValueParser::parseColor(tok);
    }
    if (lengths.size() < 2)
        return {};

    QVariantMap result;
    result.insert(QStringLiteral("x"), lengths.value(0, 0.0));
    result.insert(QStringLiteral("y"), lengths.value(1, 0.0));
    result.insert(QStringLiteral("blur"), lengths.value(2, 0.0));
    result.insert(QStringLiteral("spread"), lengths.value(3, 0.0));
    result.insert(QStringLiteral("color"), color);
    result.insert(QStringLiteral("inset"), inset);
    return result;
}

QVariantMap CssTheme::parseBoxShadow(const QString &cssValue) const
{
    const QString s = cssValue.trimmed();
    if (s.isEmpty() || s.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
        return {};
    // First shadow only — take the first comma-separated segment.
    return parseOneBoxShadow(CssValueParser::splitTopLevel(s, QLatin1Char(',')).first());
}

QVariantList CssTheme::parseBoxShadowList(const QString &cssValue) const
{
    QVariantList shadows;
    const QString s = cssValue.trimmed();
    if (s.isEmpty() || s.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
        return shadows;
    const QStringList segments = CssValueParser::splitTopLevel(s, QLatin1Char(','));
    for (const QString &segment : segments) {
        const QVariantMap shadow = parseOneBoxShadow(segment);
        if (!shadow.isEmpty())
            shadows.append(shadow);
    }
    return shadows;
}

int CssTheme::parseDuration(const QString &cssValue, int fallbackMs) const
{
    return CssValueParser::parseDuration(cssValue, fallbackMs);
}

int CssTheme::parseEasing(const QString &cssValue, int fallbackType) const
{
    return static_cast<int>(CssValueParser::parseEasing(cssValue, static_cast<QEasingCurve::Type>(fallbackType)));
}

QVariantMap CssTheme::parseTransition(const QString &cssValue) const
{
    return CssValueParser::parseTransition(cssValue);
}

QVariantMap CssTheme::parseTransform(const QString &cssValue) const
{
    QVariantMap result{
        {QStringLiteral("rotate"), 0.0},
        {QStringLiteral("scale"), 1.0},
        {QStringLiteral("scaleX"), 1.0},
        {QStringLiteral("scaleY"), 1.0},
        {QStringLiteral("translateX"), 0.0},
        {QStringLiteral("translateY"), 0.0},
    };
    if (cssValue.trimmed().isEmpty()) {
        return result;
    }

    const auto firstNumber = [](const QString &raw, double fallback) -> double {
        static const QRegularExpression re(QStringLiteral("(-?\\d*\\.?\\d+)"));
        const auto m = re.match(raw);
        return m.hasMatch() ? m.captured(1).toDouble() : fallback;
    };
    const auto angleDeg = [&firstNumber](const QString &raw) -> double {
        const double v = firstNumber(raw, 0.0);
        const QString s = raw.toLower();
        if (s.contains(QLatin1String("turn"))) {
            return v * 360.0;
        }
        if (s.contains(QLatin1String("rad"))) { // grad is rare; treat any "rad" as radians
            return v * 180.0 / 3.141592653589793;
        }
        return v; // "deg" or bare number
    };

    static const QRegularExpression fnRe(QStringLiteral("([a-zA-Z]+)\\s*\\(([^)]*)\\)"));
    auto it = fnRe.globalMatch(cssValue);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString fn = m.captured(1).toLower();
        const QStringList args = m.captured(2).split(QLatin1Char(','));
        if (args.isEmpty()) {
            continue;
        }
        if (fn == QLatin1String("rotate")) {
            result.insert(QStringLiteral("rotate"), angleDeg(args.at(0)));
        } else if (fn == QLatin1String("scale")) {
            const double sx = firstNumber(args.at(0), 1.0);
            const double sy = args.size() >= 2 ? firstNumber(args.at(1), sx) : sx;
            result.insert(QStringLiteral("scaleX"), sx);
            result.insert(QStringLiteral("scaleY"), sy);
            result.insert(QStringLiteral("scale"), sx);
        } else if (fn == QLatin1String("scalex")) {
            result.insert(QStringLiteral("scaleX"), firstNumber(args.at(0), 1.0));
        } else if (fn == QLatin1String("scaley")) {
            result.insert(QStringLiteral("scaleY"), firstNumber(args.at(0), 1.0));
        } else if (fn == QLatin1String("translate")) {
            result.insert(QStringLiteral("translateX"), firstNumber(args.at(0), 0.0));
            if (args.size() >= 2) {
                result.insert(QStringLiteral("translateY"), firstNumber(args.at(1), 0.0));
            }
        } else if (fn == QLatin1String("translatex")) {
            result.insert(QStringLiteral("translateX"), firstNumber(args.at(0), 0.0));
        } else if (fn == QLatin1String("translatey")) {
            result.insert(QStringLiteral("translateY"), firstNumber(args.at(0), 0.0));
        }
    }
    return result;
}
