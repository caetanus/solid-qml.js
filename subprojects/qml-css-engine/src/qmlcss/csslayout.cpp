#include "csslayout.h"

#include "csstheme.h"
#include "cssrect.h"

#include <QMetaMethod>
#include <QQuickItem>
#include <QRegularExpression>
#include <QTimer>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

// Trimmed string value of a style key, or empty.
QString styleStr(const QVariantMap &style, const QString &key)
{
    const auto it = style.constFind(key);
    return it == style.constEnd() ? QString() : it->toString().trimmed();
}

// JS parseFloat: leading numeric prefix (sign, digits, ., exponent); NaN if none.
double jsParseFloat(const QString &s)
{
    int i = 0;
    const int n = s.size();
    while (i < n && s[i].isSpace())
        ++i;
    const int start = i;
    if (i < n && (s[i] == QLatin1Char('+') || s[i] == QLatin1Char('-')))
        ++i;
    bool digits = false;
    while (i < n && s[i].isDigit()) { ++i; digits = true; }
    if (i < n && s[i] == QLatin1Char('.')) {
        ++i;
        while (i < n && s[i].isDigit()) { ++i; digits = true; }
    }
    if (digits && i < n && (s[i] == QLatin1Char('e') || s[i] == QLatin1Char('E'))) {
        int j = i + 1;
        if (j < n && (s[j] == QLatin1Char('+') || s[j] == QLatin1Char('-')))
            ++j;
        if (j < n && s[j].isDigit()) {
            i = j;
            while (i < n && s[i].isDigit())
                ++i;
        }
    }
    if (!digits)
        return std::nan("");
    bool ok = false;
    const double v = s.mid(start, i - start).toDouble(&ok);
    return ok ? v : std::nan("");
}

bool isLayoutChild(QQuickItem *k)
{
    return k && (k->property("style").isValid() || k->property("cssPrimitive").isValid());
}

bool isTextPrimitive(QQuickItem *k)
{
    return k && k->property("cssPrimitive").toString() == QLatin1String("text");
}

} // namespace

CssLayoutEngine::CssLayoutEngine(CssTheme *theme, QObject *parent)
    : QObject(parent)
    , m_theme(theme)
    , m_flushTimer(new QTimer(this))
{
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(0);
    connect(m_flushTimer, &QTimer::timeout, this, &CssLayoutEngine::flush);
    // The theme brackets its bulk apply passes with our batch gate.
    if (m_theme)
        m_theme->setLayoutEngine(this);
}

void CssLayoutEngine::endBatch()
{
    if (--m_batchDepth == 0 && !m_pending.isEmpty() && !m_flushing)
        flush();
}

// --- scheduling -------------------------------------------------------------------------

void CssLayoutEngine::requestLayout(QQuickItem *root, QQuickItem *content)
{
    if (!root || !content)
        return;
    m_pending.insert(root, content);
    // Hibernating (batch open): only record — endBatch runs the single flush.
    if (m_batchDepth > 0)
        return;
    // Re-entrant request (a layout pass resized a child, which re-queued work): just record it.
    // The synchronous drain loop in flush() picks it up in its next pass — no timer needed.
    if (m_flushing)
        return;
    flush();
}

void CssLayoutEngine::notifyParentLayout(QQuickItem *item)
{
    if (!item)
        return;
    // Duck-typed like the QML original (`if (holder.parent.requestRelayout) ...`): only boxes
    // (CssRect/CssFill) have requestRelayout(). A scrollable box interposes a Flickable and
    // its contentItem between the holder and the box, so climb a few levels to find it;
    // plain-Item ancestors without the method are skipped silently.
    QQuickItem *p = item->parentItem();
    for (int hops = 0; p && hops < 5; ++hops, p = p->parentItem()) {
        if (hops == 0)
            continue; // the holder itself
        const QMetaObject *mo = p->metaObject();
        const int idx = mo->indexOfMethod("requestRelayout()");
        if (idx >= 0) {
            mo->method(idx).invoke(p);
            return;
        }
    }
}

static bool sqLayoutTrace = qEnvironmentVariableIntValue("SQ_LAYOUT_TRACE") != 0;

void CssLayoutEngine::flush()
{
    if (m_flushing)
        return;

    // Drain to a FIXED POINT within this one synchronous call. A layout pass resizes children,
    // whose geometry change re-queues DEEPER boxes (and their parents). Previously each such nested
    // batch was deferred to the next event-loop turn, so an N-deep subtree took N turns — and N
    // rendered frames — to fully relayout after a resize. On a live window resize that left deep
    // boxes (e.g. a button 6 levels down) painting at STALE geometry for several frames, exposing
    // un-laid-out area. Draining here makes one resize relayout the WHOLE affected subtree before
    // the frame renders, so geometry is never stale on screen. The box model converges (implicit
    // sizes stabilise and place()/setImplicit* only re-fire past a 0.5px delta), so the loop
    // terminates; a generous guard bounds any pathological ping-pong, deferring the remainder to
    // the timer rather than spinning.
    m_flushing = true;
    int guard = 0;
    const int maxPasses = 64;
    while (!m_pending.isEmpty() && guard++ < maxPasses) {
        const auto batch = m_pending;
        m_pending.clear();
        for (auto it = batch.constBegin(); it != batch.constEnd(); ++it) {
            if (it.key() && it.value())
                layout(it.key(), it.value());
        }
    }
    m_flushing = false;

    // Only reached if the guard tripped (non-convergent): finish the remainder next turn.
    if (!m_pending.isEmpty() && !m_flushTimer->isActive())
        m_flushTimer->start();
}

// --- length / calc ----------------------------------------------------------------------

qreal CssLayoutEngine::length(const QString &value, qreal avail) const
{
    return evalExpr(value, avail);
}

double CssLayoutEngine::calcUnit(const QString &tokIn, double avail) const
{
    const QString tok = tokIn.trimmed();
    if (tok.isEmpty())
        return std::nan("");
    if (tok.endsWith(QLatin1Char('%'))) {
        const double p = jsParseFloat(tok);
        return std::isnan(p) ? std::nan("") : avail * p / 100.0;
    }
    const QString low = tok.toLower();
    const double vw = m_theme ? m_theme->viewportWidth() : 0;
    const double vh = m_theme ? m_theme->viewportHeight() : 0;
    if (low.endsWith(QLatin1String("vmin")))
        return jsParseFloat(tok) * std::min(vw, vh) / 100.0;
    if (low.endsWith(QLatin1String("vmax")))
        return jsParseFloat(tok) * std::max(vw, vh) / 100.0;
    if (low.endsWith(QLatin1String("vw")))
        return jsParseFloat(tok) * vw / 100.0;
    if (low.endsWith(QLatin1String("vh")))
        return jsParseFloat(tok) * vh / 100.0;
    return jsParseFloat(tok); // px / bare number; NaN for "auto"/"fit-content"/…
}

namespace {
struct Tok {
    enum Type { Op, Id, Num } type;
    QString text;
};

QVector<Tok> calcTokenize(const QString &s)
{
    QVector<Tok> t;
    int i = 0;
    const int n = s.size();
    while (i < n) {
        const QChar c = s[i];
        if (c.isSpace()) { ++i; continue; }
        if (QStringLiteral("()+*/,-").contains(c)) { t.push_back({ Tok::Op, QString(c) }); ++i; continue; }
        if (c.isLetter()) {
            int j = i;
            while (j < n && s[j].isLetter())
                ++j;
            t.push_back({ Tok::Id, s.mid(i, j - i).toLower() });
            i = j;
            continue;
        }
        int num = i;
        while (num < n && (s[num].isDigit() || s[num] == QLatin1Char('.')))
            ++num;
        int u = num;
        while (u < n && (s[u].isLetter() || s[u] == QLatin1Char('%')))
            ++u;
        t.push_back({ Tok::Num, s.mid(i, u - i) });
        i = u;
    }
    return t;
}
} // namespace

double CssLayoutEngine::evalExpr(const QString &value, double avail) const
{
    const QString s = value.trimmed();
    if (s.isEmpty())
        return std::nan("");
    const QString low = s.toLower();
    if (!low.contains(QLatin1String("calc(")) && !low.contains(QLatin1String("min("))
        && !low.contains(QLatin1String("max(")) && !low.contains(QLatin1String("clamp(")))
        return calcUnit(s, avail);

    const QVector<Tok> t = calcTokenize(s);
    int i = 0;
    // Recursive descent: add/sub over mul/div over primary (with parens, fns, unary sign).
    std::function<double()> primary, mul, add;
    primary = [&]() -> double {
        if (i >= t.size())
            return std::nan("");
        const Tok &tk = t[i];
        if (tk.type == Tok::Op && tk.text == QLatin1String("-")) { ++i; return -primary(); }
        if (tk.type == Tok::Op && tk.text == QLatin1String("+")) { ++i; return primary(); }
        if (tk.type == Tok::Op && tk.text == QLatin1String("(")) {
            ++i;
            const double v = add();
            if (i < t.size() && t[i].text == QLatin1String(")")) ++i;
            return v;
        }
        if (tk.type == Tok::Id) {
            const QString name = tk.text;
            ++i;
            if (i < t.size() && t[i].text == QLatin1String("(")) {
                ++i;
                QVector<double> args;
                args.push_back(add());
                while (i < t.size() && t[i].text == QLatin1String(",")) { ++i; args.push_back(add()); }
                if (i < t.size() && t[i].text == QLatin1String(")")) ++i;
                if (name == QLatin1String("min")) return *std::min_element(args.begin(), args.end());
                if (name == QLatin1String("max")) return *std::max_element(args.begin(), args.end());
                if (name == QLatin1String("clamp") && args.size() >= 3)
                    return std::min(std::max(args[1], args[0]), args[2]);
                return args.isEmpty() ? std::nan("") : args[0]; // calc()/unknown
            }
            return std::nan(""); // bare keyword
        }
        if (tk.type == Tok::Num) { ++i; return calcUnit(tk.text, avail); }
        ++i;
        return std::nan("");
    };
    mul = [&]() -> double {
        double v = primary();
        while (i < t.size() && t[i].type == Tok::Op
               && (t[i].text == QLatin1String("*") || t[i].text == QLatin1String("/"))) {
            const QString op = t[i].text;
            ++i;
            const double r = primary();
            v = (op == QLatin1String("*")) ? v * r : v / r;
        }
        return v;
    };
    add = [&]() -> double {
        double v = mul();
        while (i < t.size() && t[i].type == Tok::Op
               && (t[i].text == QLatin1String("+") || t[i].text == QLatin1String("-"))) {
            const QString op = t[i].text;
            ++i;
            const double r = mul();
            v = (op == QLatin1String("+")) ? v + r : v - r;
        }
        return v;
    };
    return add();
}

// --- box-model helpers ------------------------------------------------------------------

double CssLayoutEngine::sizeOf(const QVariantMap &style, const QString &key, double avail) const
{
    const QString v = styleStr(style, key);
    return v.isEmpty() ? std::nan("") : evalExpr(v, avail);
}

QVector<double> CssLayoutEngine::box(const QString &value) const
{
    if (value.isEmpty())
        return { 0, 0, 0, 0 };
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList parts = value.trimmed().split(ws, Qt::SkipEmptyParts);
    QVector<double> n;
    for (const QString &p : parts) {
        const double x = evalExpr(p, 0);
        n.push_back(std::isnan(x) ? 0 : x);
    }
    if (n.size() == 1) return { n[0], n[0], n[0], n[0] };
    if (n.size() == 2) return { n[0], n[1], n[0], n[1] };
    if (n.size() == 3) return { n[0], n[1], n[2], n[1] };
    if (n.size() >= 4) return { n[0], n[1], n[2], n[3] };
    return { 0, 0, 0, 0 };
}

QVector<double> CssLayoutEngine::paddingOf(const QVariantMap &style) const
{
    QVector<double> b = box(styleStr(style, "padding"));
    const QString sides[4] = { QStringLiteral("padding-top"), QStringLiteral("padding-right"),
                               QStringLiteral("padding-bottom"), QStringLiteral("padding-left") };
    for (int i = 0; i < 4; ++i) {
        const QString s = styleStr(style, sides[i]);
        if (!s.isEmpty()) {
            const double r = evalExpr(s, 0);
            if (!std::isnan(r)) b[i] = r;
        }
    }
    return b;
}

// Border widths [top,right,bottom,left] — the analogue of paddingOf, needed by box-sizing so a
// content-box item's border box accounts for its borders. Reads the `border-width` box shorthand,
// the leading length of the `border` shorthand, and per-side `border-<side>-width`/`border-<side>`.
QVector<double> CssLayoutEngine::borderOf(const QVariantMap &style) const
{
    QVector<double> b = { 0, 0, 0, 0 };
    const QString bw = styleStr(style, "border-width");
    if (!bw.isEmpty())
        b = box(bw);
    const QString bshort = styleStr(style, "border");
    if (!bshort.isEmpty()) {
        const double w = evalExpr(bshort.section(QLatin1Char(' '), 0, 0), 0);
        if (!std::isnan(w)) b = { w, w, w, w };
    }
    const QString sides[4] = { QStringLiteral("top"), QStringLiteral("right"),
                               QStringLiteral("bottom"), QStringLiteral("left") };
    for (int i = 0; i < 4; ++i) {
        const QString sw = styleStr(style, QStringLiteral("border-") + sides[i] + QStringLiteral("-width"));
        if (!sw.isEmpty()) {
            const double r = evalExpr(sw, 0);
            if (!std::isnan(r)) b[i] = r;
            continue;
        }
        const QString sh = styleStr(style, QStringLiteral("border-") + sides[i]);
        if (!sh.isEmpty()) {
            const double r = evalExpr(sh.section(QLatin1Char(' '), 0, 0), 0);
            if (!std::isnan(r)) b[i] = r;
        }
    }
    return b;
}

QVector<double> CssLayoutEngine::marginOf(const QVariantMap &style) const
{
    QVector<double> b = box(styleStr(style, "margin"));
    const QString sides[4] = { QStringLiteral("margin-top"), QStringLiteral("margin-right"),
                               QStringLiteral("margin-bottom"), QStringLiteral("margin-left") };
    for (int i = 0; i < 4; ++i) {
        const QString s = styleStr(style, sides[i]);
        if (!s.isEmpty()) {
            const double r = evalExpr(s, 0);
            if (!std::isnan(r)) b[i] = r;
        }
    }
    return b;
}

double CssLayoutEngine::aspectOf(const QVariantMap &style) const
{
    const QString v = styleStr(style, "aspect-ratio");
    if (v.isEmpty())
        return std::nan("");
    if (v.contains(QLatin1Char('/'))) {
        const QStringList p = v.split(QLatin1Char('/'));
        const double a = jsParseFloat(p.value(0)), b = jsParseFloat(p.value(1));
        return (a > 0 && b > 0) ? a / b : std::nan("");
    }
    const double r = jsParseFloat(v);
    return r > 0 ? r : std::nan("");
}

// --- geometry assignment ----------------------------------------------------------------

double CssLayoutEngine::clampSize(const QVariantMap &style, const QString &axis, double v, double avail) const
{
    if (std::isnan(v))
        return v;
    const double mx = sizeOf(style, QStringLiteral("max-") + axis, avail);
    const double mn = sizeOf(style, QStringLiteral("min-") + axis, avail);
    if (!std::isnan(mx) && v > mx) v = mx;
    if (!std::isnan(mn) && v < mn) v = mn;
    return v;
}

void CssLayoutEngine::place(QQuickItem *k, double x, double y, double w, double h) const
{
    // Final clamp so stretched / grown items still honour min-/max-width/height (a flex row that
    // stretches children to the container's cross box must not blow past their max-width — this is
    // what makes a `max-width` paragraph wrap instead of overflowing).
    const QVariantMap ks = k->property("style").toMap();
    w = clampSize(ks, QStringLiteral("width"), w, std::nan(""));
    h = clampSize(ks, QStringLiteral("height"), h, std::nan(""));
    // A declared `transition: width/height` animates the write instead of snapping; the
    // cast + covers-check only run when the geometry actually changes.
    // Record the imposition (the CSS "definite size" signal) even when the value is
    // unchanged — children of an externally-sized box get the spec flex-shrink default.
    CssRect *marked = qobject_cast<CssRect *>(k);
    if (marked)
        marked->markImposed(!std::isnan(w) && w >= 0, !std::isnan(h) && h >= 0);
    CssRect *box = nullptr;
    if (!std::isnan(w) && w >= 0 && std::abs(k->width() - w) > 0.5) {
        box = marked;
        if (!box || !box->animateGeometry(QLatin1String("width"), w))
            k->setWidth(w);
    }
    if (!std::isnan(h) && h >= 0 && std::abs(k->height() - h) > 0.5) {
        if (!box)
            box = qobject_cast<CssRect *>(k);
        if (!box || !box->animateGeometry(QLatin1String("height"), h))
            k->setHeight(h);
    }
    if (!std::isnan(x) && std::abs(k->x() - x) > 0.5) k->setX(x);
    if (!std::isnan(y) && std::abs(k->y() - y) > 0.5) k->setY(y);
}

void CssLayoutEngine::placeAbsolute(QQuickItem *k, double ox, double oy, double cw, double ch) const
{
    const QVariantMap ks = k->property("style").toMap();
    double t = std::nan(""), r = std::nan(""), b = std::nan(""), l = std::nan("");
    const QString inset = styleStr(ks, "inset");
    if (!inset.isEmpty()) {
        const QVector<double> ib = box(inset);
        t = ib[0]; r = ib[1]; b = ib[2]; l = ib[3];
    } else {
        const QString ts = styleStr(ks, "top"); if (!ts.isEmpty()) t = evalExpr(ts, ch);
        const QString rs = styleStr(ks, "right"); if (!rs.isEmpty()) r = evalExpr(rs, cw);
        const QString bs = styleStr(ks, "bottom"); if (!bs.isEmpty()) b = evalExpr(bs, ch);
        const QString ls = styleStr(ks, "left"); if (!ls.isEmpty()) l = evalExpr(ls, cw);
    }
    double w = sizeOf(ks, "width", cw);
    double h = sizeOf(ks, "height", ch);
    const double ar = aspectOf(ks);
    if (!std::isnan(ar) && ar > 0) {
        if (!std::isnan(w) && std::isnan(h)) h = w / ar;
        else if (!std::isnan(h) && std::isnan(w)) w = h * ar;
    }
    double x;
    if (!std::isnan(l) && !std::isnan(r)) { x = ox + l; if (std::isnan(w)) w = cw - l - r; }
    else if (!std::isnan(r)) { if (std::isnan(w)) w = k->implicitWidth(); x = ox + cw - r - w; }
    else { if (std::isnan(w)) w = k->implicitWidth(); x = ox + (std::isnan(l) ? 0 : l); }
    double y;
    if (!std::isnan(t) && !std::isnan(b)) { y = oy + t; if (std::isnan(h)) h = ch - t - b; }
    else if (!std::isnan(b)) { if (std::isnan(h)) h = k->implicitHeight(); y = oy + ch - b - h; }
    else { if (std::isnan(h)) h = k->implicitHeight(); y = oy + (std::isnan(t) ? 0 : t); }
    place(k, x, y, w, h);
}

// --- main entry -------------------------------------------------------------------------

void CssLayoutEngine::layout(QQuickItem *root, QQuickItem *content)
{
    if (sqLayoutTrace) {
        const QString cls = root->property("cssClass").toStringList().join(QLatin1Char('.'));
        fprintf(stderr, "[layout] %s w=%.0f h=%.0f\n", qPrintable(cls.isEmpty() ? QString::fromLatin1(root->metaObject()->className()) : cls), root->width(), root->height());
    }
    if (!root || !content)
        return;
    const QVariantMap rootStyle = root->property("style").toMap();
    const QVector<double> pad = paddingOf(rootStyle);
    const double originX = pad[3];
    const double originY = pad[0];
    const double cw = std::max(0.0, root->width() - pad[1] - pad[3]);
    const double ch = std::max(0.0, root->height() - pad[0] - pad[2]);

    QString disp = styleStr(rootStyle, "display");
    if (disp.isEmpty())
        disp = QStringLiteral("block");
    const bool isGrid = (disp == QLatin1String("grid"));
    const bool isFlex = disp.contains(QLatin1String("flex"));

    QList<QQuickItem *> flow, abs;
    const auto kids = content->childItems();
    for (QQuickItem *k : kids) {
        if (!isLayoutChild(k))
            continue;
        // An invisible child is out of flow (conditional rendering: <Show>/<Switch> branches, an
        // empty <For>), like `display: none` — it must not occupy flex/grid space. When it becomes
        // visible, CssRect's onVisibleChanged notifies the parent, which relayouts and sizes it.
        if (!k->isVisible())
            continue;
        const QVariantMap ks = k->property("style").toMap();
        if (styleStr(ks, "display") == QLatin1String("none"))
            continue;
        if (styleStr(ks, "position") == QLatin1String("absolute"))
            abs.push_back(k);
        else
            flow.push_back(k);
    }

    // CSS `order`: flex/grid items are laid out by ascending `order` (default 0), ties keeping
    // document order. Read `order` ONCE per item (0-overhead: the comparator must not re-copy each
    // item's style map — that would be O(n log n) QVariantMap copies), then stable_sort the pairs.
    if ((isFlex || isGrid) && flow.size() > 1) {
        QVarLengthArray<QPair<int, QQuickItem *>> byOrder(flow.size());
        for (int i = 0; i < flow.size(); ++i) {
            bool ok = false;
            const int o = styleStr(flow[i]->property("style").toMap(), QStringLiteral("order")).toInt(&ok);
            byOrder[i] = { ok ? o : 0, flow[i] };
        }
        std::stable_sort(byOrder.begin(), byOrder.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        for (int i = 0; i < flow.size(); ++i)
            flow[i] = byOrder[i].second;
    }

    // CSS "definite size" per axis: declared in the container's own style, or imposed by
    // the parent's layout (place() marks the box). Gates the spec flex-shrink default.
    const auto axisDefinite = [&](const char *key) {
        const QString v = styleStr(rootStyle, QLatin1String(key));
        return !v.isEmpty() && v != QLatin1String("auto");
    };
    auto *rootBox = qobject_cast<CssRect *>(root);
    const bool widthDefinite = axisDefinite("width") || (rootBox && rootBox->widthImposed());
    const bool heightDefinite = axisDefinite("height") || (rootBox && rootBox->heightImposed());

    const QPair<double, double> contentSize = isGrid
        ? layoutGrid(flow, rootStyle, cw, ch, originX, originY)
        : layoutFlex(flow, rootStyle, cw, ch, originX, originY, isFlex, widthDefinite, heightDefinite);

    for (QQuickItem *k : abs)
        placeAbsolute(k, originX, originY, cw, ch);

    const double iw = contentSize.first + pad[1] + pad[3];
    const double ih = contentSize.second + pad[0] + pad[2];
    if (std::abs(root->implicitWidth() - iw) > 0.5) root->setImplicitWidth(iw);
    if (std::abs(root->implicitHeight() - ih) > 0.5) root->setImplicitHeight(ih);
}

// --- flexbox ----------------------------------------------------------------------------

QPair<double, double> CssLayoutEngine::layoutFlex(const QList<QQuickItem *> &flow,
                                                  const QVariantMap &rootStyle, double cw, double ch,
                                                  double originX, double originY, bool isFlex,
                                                  bool widthDefinite, bool heightDefinite) const
{
    QString flexDir = styleStr(rootStyle, "flex-direction");
    if (flexDir.isEmpty()) flexDir = QStringLiteral("row");
    const bool horizontal = isFlex ? flexDir.startsWith(QLatin1String("row")) : false;
    const double mainAvail = horizontal ? cw : ch;
    const double crossAvail = horizontal ? ch : cw;
    // CSS "definite main size": only then does the spec default flex-shrink:1 apply. An
    // auto-sized main (content-driven) must let children overflow, never crush them against
    // a stale in-convergence size (the historical auto-column collapse).
    const bool mainDefinite = horizontal ? widthDefinite : heightDefinite;

    QString gapStr = styleStr(rootStyle, "gap");
    if (gapStr.isEmpty()) gapStr = styleStr(rootStyle, "row-gap");
    if (gapStr.isEmpty()) gapStr = styleStr(rootStyle, "column-gap");
    const double gap = gapStr.isEmpty() ? 0 : evalExpr(gapStr, 0);

    QString justify = styleStr(rootStyle, "justify-content");
    if (justify.isEmpty()) justify = QStringLiteral("flex-start");
    QString alignItems = styleStr(rootStyle, "align-items");
    if (alignItems.isEmpty()) alignItems = QStringLiteral("stretch");

    const int n = flow.size();
    QVector<double> mains(n), crosses(n), grows(n), shrinks(n);
    QVector<bool> cfixed(n);
    QVector<QPair<double, double>> mMar(n), cMar(n);
    double usedMain = 0, totalGrow = 0;

    for (int i = 0; i < n; ++i) {
        const QVariantMap ks = flow[i]->property("style").toMap();
        const QVector<double> mar = marginOf(ks);
        mMar[i] = horizontal ? qMakePair(mar[3], mar[1]) : qMakePair(mar[0], mar[2]);
        cMar[i] = horizontal ? qMakePair(mar[0], mar[2]) : qMakePair(mar[3], mar[1]);

        const bool wExplicit = !std::isnan(sizeOf(ks, "width", cw));
        const bool hExplicit = !std::isnan(sizeOf(ks, "height", ch));
        double w = sizeOf(ks, "width", cw);
        double h = sizeOf(ks, "height", ch);
        // box-sizing: content-box (CSS default) means the specified width/height is the CONTENT box,
        // so the laid-out border box = width + padding + border. border-box (what the project's
        // reset.css applies to *) means width already IS the border box, so nothing is added. Only
        // pay for the padding/border reads when a content-box item actually has an explicit size.
        if ((wExplicit || hExplicit)
            && styleStr(ks, "box-sizing") != QLatin1String("border-box")) {
            const QVector<double> ip = paddingOf(ks);
            const QVector<double> ib = borderOf(ks);
            if (wExplicit) w += ip[1] + ip[3] + ib[1] + ib[3];
            if (hExplicit) h += ip[0] + ip[2] + ib[0] + ib[2];
        }
        const double ar = aspectOf(ks);
        if (!std::isnan(ar) && ar > 0) {
            if (!std::isnan(w) && std::isnan(h)) h = w / ar;
            else if (!std::isnan(h) && std::isnan(w)) w = h * ar;
        }
        if (std::isnan(w)) w = flow[i]->implicitWidth();
        if (std::isnan(h)) h = flow[i]->implicitHeight();
        // Clamp the natural size by min-/max-<axis> so content-sizing (and the container's implicit
        // size) respects a max-width — a paragraph with max-width contributes its clamped width.
        w = clampSize(ks, QStringLiteral("width"), w, cw);
        h = clampSize(ks, QStringLiteral("height"), h, ch);

        mains[i] = horizontal ? w : h;
        crosses[i] = horizontal ? h : w;
        // flex-basis sets the base main-axis size; flex-grow/shrink distribute free space from here.
        const double basis = sizeOf(ks, QStringLiteral("flex-basis"), mainAvail);
        if (!std::isnan(basis)) mains[i] = basis;
        cfixed[i] = horizontal ? (hExplicit || (!std::isnan(ar) && wExplicit))
                               : (wExplicit || (!std::isnan(ar) && hExplicit));

        double g = 0;
        const QString fg = styleStr(ks, "flex-grow");
        if (!fg.isEmpty()) g = jsParseFloat(fg);
        else { const QString fx = styleStr(ks, "flex"); if (!fx.isEmpty()) g = jsParseFloat(fx); }
        if (std::isnan(g)) g = 0;
        grows[i] = g;

        // flex-shrink: weights how much an item gives back when a line OVERFLOWS. The spec
        // default 1 applies ONLY when the container's main size is DEFINITE (declared in its
        // style, or imposed by the parent's layout — see markImposed); an auto-sized main
        // keeps 0 so content stacks/overflows instead of crushing against a stale
        // in-convergence size (the historical auto-column collapse).
        double sh = mainDefinite ? 1 : 0;
        const QString fsh = styleStr(ks, "flex-shrink");
        if (!fsh.isEmpty())
            sh = jsParseFloat(fsh);
        else {
            const QString fx = styleStr(ks, "flex");
            if (!fx.isEmpty()) {
                static const QRegularExpression fws(QStringLiteral("\\s+"));
                const QStringList fp = fx.trimmed().split(fws, Qt::SkipEmptyParts);
                if (fp.size() >= 2) sh = jsParseFloat(fp[1]);
            }
        }
        if (std::isnan(sh)) sh = 0;
        shrinks[i] = sh;

        usedMain += mains[i] + mMar[i].first + mMar[i].second;
        totalGrow += g;
    }

    Q_UNUSED(usedMain);
    Q_UNUSED(totalGrow);

    // Partition items into flex LINES. Without `flex-wrap: wrap` it is one line (the common case);
    // with wrap, a new line starts whenever the next item would overflow the main axis. Lines stack
    // along the cross axis (each line as tall as its tallest item), so a long button row reflows.
    const QString wrapStr = styleStr(rootStyle, "flex-wrap");
    const bool wrap = wrapStr.startsWith(QLatin1String("wrap"));
    QVector<QVector<int>> linesIdx;
    if (!wrap) {
        QVector<int> all;
        for (int i = 0; i < n; ++i) all.push_back(i);
        linesIdx.push_back(all);
    } else {
        QVector<int> cur;
        double run = 0;
        for (int i = 0; i < n; ++i) {
            const double itemMain = mains[i] + mMar[i].first + mMar[i].second;
            const double add = (cur.isEmpty() ? 0.0 : gap) + itemMain;
            if (!cur.isEmpty() && run + add > mainAvail + 0.5) {
                linesIdx.push_back(cur);
                cur.clear();
                run = 0;
            }
            run += (cur.isEmpty() ? 0.0 : gap) + itemMain;
            cur.push_back(i);
        }
        if (!cur.isEmpty()) linesIdx.push_back(cur);
    }

    // Per-line NATURAL cross sizes (tallest item incl. cross margins), computed up front so
    // align-content can distribute the leftover cross space across MULTIPLE wrapped lines.
    const int nLines = linesIdx.size();
    QVector<double> lineCrosses(nLines, 0);
    for (int li = 0; li < nLines; ++li) {
        for (int k = 0; k < linesIdx[li].size(); ++k) {
            const int i = linesIdx[li][k];
            const double tc = crosses[i] + cMar[i].first + cMar[i].second;
            if (tc > lineCrosses[li]) lineCrosses[li] = tc;
        }
    }

    // align-content positions the LINES within the cross box — multi-line only; a single line keeps
    // the existing stretch-to-container behaviour. Default `stretch` grows each line to fill.
    double lineStart = 0;    // cross offset of the first line
    double lineBetween = 0;  // extra spacing inserted between consecutive lines
    double lineStretch = 0;  // extra cross size added to every line (align-content: stretch)
    if (nLines > 1 && !std::isnan(crossAvail)) {
        double linesCross = gap * (nLines - 1);
        for (int li = 0; li < nLines; ++li) linesCross += lineCrosses[li];
        const double crossFree = crossAvail - linesCross;
        if (crossFree > 0) {
            QString alignContent = styleStr(rootStyle, "align-content");
            if (alignContent.isEmpty()) alignContent = QStringLiteral("stretch");
            if (alignContent == QLatin1String("center")) lineStart = crossFree / 2;
            else if (alignContent == QLatin1String("flex-end") || alignContent == QLatin1String("end")) lineStart = crossFree;
            else if (alignContent == QLatin1String("space-between")) lineBetween = crossFree / (nLines - 1);
            else if (alignContent == QLatin1String("space-around")) { lineBetween = crossFree / nLines; lineStart = lineBetween / 2; }
            else if (alignContent == QLatin1String("stretch")) lineStretch = crossFree / nLines;
            // flex-start (and any unknown): lines pack at the start, no distribution.
        }
    }

    double crossCursor = lineStart;   // running offset down the cross axis as lines are placed
    double maxMainUsed = 0;   // widest line → main-axis content size
    double totalCross = 0;    // summed natural line crosses (+ gaps) → cross-axis content size

    for (int li = 0; li < nLines; ++li) {
        const QVector<int> &line = linesIdx[li];
        const int ln = line.size();

        // Main-axis free space for THIS line, then grow / justify distribution within the line.
        double lineUsed = 0, lineGrow = 0;
        for (int k = 0; k < ln; ++k) {
            const int i = line[k];
            lineUsed += mains[i] + mMar[i].first + mMar[i].second;
            lineGrow += grows[i];
        }
        if (ln > 1) lineUsed += gap * (ln - 1);
        double freeSpace = mainAvail - lineUsed;

        QVector<double> lm = mains; // per-line main sizes (flex-grow inflates / flex-shrink deflates)
        if (freeSpace > 0 && lineGrow > 0) {
            for (int k = 0; k < ln; ++k) lm[line[k]] += freeSpace * grows[line[k]] / lineGrow;
            freeSpace = 0;
        } else if (freeSpace < 0) {
            // Overflow: give the (negative) free space back by flex-shrink × base-size (CSS's scaled
            // shrink factor), so the items deflate proportionally and the line stops overflowing.
            double scaled = 0;
            for (int k = 0; k < ln; ++k) scaled += shrinks[line[k]] * mains[line[k]];
            if (scaled > 0) {
                for (int k = 0; k < ln; ++k) {
                    const int i = line[k];
                    lm[i] = std::max(0.0, mains[i] + freeSpace * (shrinks[i] * mains[i]) / scaled);
                }
                freeSpace = 0;
            }
        }

        double pos = 0;
        double between = gap;
        if (lineGrow == 0 && freeSpace > 0) {
            if (justify == QLatin1String("center")) pos = freeSpace / 2;
            else if (justify == QLatin1String("flex-end") || justify == QLatin1String("end")) pos = freeSpace;
            else if (justify == QLatin1String("space-between") && ln > 1) between = gap + freeSpace / (ln - 1);
            else if (justify == QLatin1String("space-around") && ln > 0) { between = gap + freeSpace / ln; pos = freeSpace / (2 * ln); }
            else if (justify == QLatin1String("space-evenly") && ln > 0) { between = gap + freeSpace / (ln + 1); pos = freeSpace / (ln + 1); }
        }

        // The line's NATURAL cross size (precomputed). A single, non-wrapped line stretches its
        // items to the container's cross box (so a row constrains to the parent's width even when
        // its natural content is wider — it then wraps rather than overflowing). Wrapped lines take
        // their own natural height plus any align-content: stretch growth.
        const double lineCross = lineCrosses[li];
        const double lineBox = (nLines == 1 && !std::isnan(crossAvail)) ? crossAvail : lineCross + lineStretch;

        for (int k = 0; k < ln; ++k) {
            const int i = line[k];
            const QVariantMap ks = flow[i]->property("style").toMap();
            const double msz = lm[i];
            // NATURAL cross feeds content-sizing; the stretched/centred value only PLACES the item,
            // so the container's implicit cross can't circularly echo its own size.
            const double naturalCross = crosses[i];
            double csz = crosses[i];
            QString al = styleStr(ks, "align-self");
            if (al.isEmpty()) al = alignItems;
            const double crossInner = lineBox - cMar[i].first - cMar[i].second;
            if ((al == QLatin1String("stretch") || al.isEmpty()) && !cfixed[i] && !std::isnan(crossInner)) csz = crossInner;
            // QQuickText's implicit height is a font metrics box. When centered inside a fixed
            // flex row (button labels, TodoMVC's "○"), use the available cross-axis box so
            // Text.AlignVCenter can center the glyph visually instead of centering metrics.
            if (al == QLatin1String("center") && isTextPrimitive(flow[i]) && !std::isnan(crossInner) && crossInner > csz)
                csz = crossInner;
            double crossPos = cMar[i].first;
            if (al == QLatin1String("center")) crossPos = cMar[i].first + (crossInner - csz) / 2;
            else if (al == QLatin1String("flex-end") || al == QLatin1String("end")) crossPos = lineBox - cMar[i].second - csz;
            crossPos += crossCursor;

            const double mainPos = pos + mMar[i].first;
            if (horizontal) place(flow[i], originX + mainPos, originY + crossPos, msz, csz);
            else place(flow[i], originX + crossPos, originY + mainPos, csz, msz);

            pos = mainPos + msz + mMar[i].second + between;
            Q_UNUSED(naturalCross);
        }
        const double lineMainUsed = ln > 0 ? (pos - between) : 0;
        if (lineMainUsed > maxMainUsed) maxMainUsed = lineMainUsed;
        totalCross += lineCross + (li + 1 < nLines ? gap : 0);
        // Stack lines by their box (natural + align-content stretch) plus the gap and any
        // align-content spacing between lines.
        crossCursor += lineCross + lineStretch + gap + lineBetween;
    }

    return horizontal ? qMakePair(maxMainUsed, totalCross) : qMakePair(totalCross, maxMainUsed);
}

// --- grid -------------------------------------------------------------------------------

QStringList CssLayoutEngine::gridSplit(const QString &s) const
{
    QStringList out;
    int depth = 0;
    QString cur;
    for (const QChar c : s) {
        if (c == QLatin1Char('(')) ++depth;
        else if (c == QLatin1Char(')')) --depth;
        if ((c == QLatin1Char(' ') || c == QLatin1Char('\t')) && depth == 0) {
            if (!cur.isEmpty()) { out << cur; cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

QString CssLayoutEngine::expandRepeat(const QString &specIn) const
{
    QString out = specIn;
    int guard = 0;
    while (guard++ < 64) {
        const int idx = out.toLower().indexOf(QLatin1String("repeat("));
        if (idx < 0)
            break;
        const int open = idx + 6; // the "("
        int depth = 0, end = -1;
        for (int i = open; i < out.size(); ++i) {
            if (out[i] == QLatin1Char('(')) ++depth;
            else if (out[i] == QLatin1Char(')')) { if (--depth == 0) { end = i; break; } }
        }
        if (end < 0)
            break;
        const QString inner = out.mid(open + 1, end - open - 1);
        const int comma = inner.indexOf(QLatin1Char(','));
        if (comma < 0)
            break;
        const int nrep = inner.left(comma).trimmed().toInt();
        const QString pattern = inner.mid(comma + 1).trimmed();
        QStringList rep;
        for (int j = 0; j < std::max(1, nrep); ++j)
            rep << pattern;
        out = out.left(idx) + rep.join(QLatin1Char(' ')) + out.mid(end + 1);
    }
    return out;
}

CssLayoutEngine::Track CssLayoutEngine::parseTrack(const QString &tokenIn, double avail) const
{
    const QString t = tokenIn.trimmed().toLower();
    if (t == QLatin1String("auto") || t == QLatin1String("min-content") || t == QLatin1String("max-content"))
        return { Track::Auto, 0 };
    if (t.startsWith(QLatin1String("minmax("))) {
        const QString inner = t.mid(7, t.lastIndexOf(QLatin1Char(')')) - 7);
        const QStringList parts = inner.split(QLatin1Char(','));
        return parseTrack(parts.size() > 1 ? parts[1] : parts[0], avail); // grow toward the max
    }
    if (t.endsWith(QLatin1String("fr"))) {
        const double f = jsParseFloat(t);
        return { Track::Fr, std::isnan(f) ? 1.0 : f };
    }
    if (t.endsWith(QLatin1Char('%'))) {
        const double p = jsParseFloat(t);
        return { Track::Px, std::isnan(p) ? 0.0 : avail * p / 100.0 };
    }
    const double n = calcUnit(t, avail);
    return std::isnan(n) ? Track{ Track::Auto, 0 } : Track{ Track::Px, n };
}

QVector<CssLayoutEngine::Track> CssLayoutEngine::parseTracks(const QString &spec, double avail) const
{
    QVector<Track> tracks;
    if (spec.isEmpty())
        return tracks;
    const QStringList toks = gridSplit(expandRepeat(spec));
    for (const QString &tk : toks)
        tracks.push_back(parseTrack(tk, avail));
    return tracks;
}

QVector<double> CssLayoutEngine::resolveTracks(const QVector<Track> &tracks, double avail, double gap,
                                               const QVector<double> &contentSizes) const
{
    double used = 0, frSum = 0;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks[i].kind == Track::Px) used += tracks[i].value;
        else if (tracks[i].kind == Track::Auto) used += contentSizes.value(i, 0);
        else if (tracks[i].kind == Track::Fr) frSum += tracks[i].value;
    }
    used += gap * std::max(0, static_cast<int>(tracks.size()) - 1);
    const double freeSpace = std::max(0.0, avail - used);
    QVector<double> sizes;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks[i].kind == Track::Px) sizes.push_back(tracks[i].value);
        else if (tracks[i].kind == Track::Auto) sizes.push_back(contentSizes.value(i, 0));
        else sizes.push_back(frSum > 0 ? freeSpace * tracks[i].value / frSum : 0);
    }
    return sizes;
}

QPair<double, double> CssLayoutEngine::layoutGrid(const QList<QQuickItem *> &flow,
                                                  const QVariantMap &rootStyle, double cw, double ch,
                                                  double ox, double oy) const
{
    QString colGapS = styleStr(rootStyle, "column-gap");
    if (colGapS.isEmpty()) colGapS = styleStr(rootStyle, "gap");
    QString rowGapS = styleStr(rootStyle, "row-gap");
    if (rowGapS.isEmpty()) rowGapS = styleStr(rootStyle, "gap");
    const double colGap = colGapS.isEmpty() ? 0 : evalExpr(colGapS, 0);
    const double rowGap = rowGapS.isEmpty() ? 0 : evalExpr(rowGapS, 0);

    // --- grid-template-areas -----------------------------------------------------------
    // A named-area grid: `grid-template-areas: "head head" "side main"` defines a row×col
    // matrix of area names ("." = empty). Each name occupies the contiguous rectangle of
    // cells it spans. An item with `grid-area: head` is placed over that rectangle (spanning
    // its columns/rows), sized by the grid-template-columns/rows tracks. Items without a
    // `grid-area` fall back to row-major auto-placement into single cells.
    const QString areasSpec = styleStr(rootStyle, "grid-template-areas");
    if (!areasSpec.isEmpty()) {
        // Parse the quoted rows into a name matrix.
        QVector<QStringList> grid;
        {
            static const QRegularExpression ws(QStringLiteral("\\s+"));
            int i = 0;
            const int len = areasSpec.size();
            while (i < len) {
                const QChar c = areasSpec[i];
                if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                    const QChar q = c;
                    int j = i + 1;
                    while (j < len && areasSpec[j] != q)
                        ++j;
                    const QString rowStr = areasSpec.mid(i + 1, j - i - 1).trimmed();
                    grid.push_back(rowStr.isEmpty() ? QStringList()
                                                    : rowStr.split(ws, Qt::SkipEmptyParts));
                    i = j + 1;
                } else {
                    ++i;
                }
            }
        }
        if (!grid.isEmpty()) {
            struct Rect { int r0, c0, r1, c1; };
            const int nrows = grid.size();
            int ncols = 0;
            for (const QStringList &r : grid)
                ncols = std::max(ncols, static_cast<int>(r.size()));
            if (ncols < 1)
                ncols = 1;

            // area name -> bounding rectangle (inclusive) of the cells it covers.
            QHash<QString, Rect> areas;
            for (int r = 0; r < nrows; ++r) {
                for (int c = 0; c < grid[r].size(); ++c) {
                    const QString name = grid[r][c];
                    if (name == QLatin1String("."))
                        continue;
                    auto it = areas.find(name);
                    if (it == areas.end()) {
                        areas.insert(name, Rect{ r, c, r, c });
                    } else {
                        it->r0 = std::min(it->r0, r);
                        it->c0 = std::min(it->c0, c);
                        it->r1 = std::max(it->r1, r);
                        it->c1 = std::max(it->c1, c);
                    }
                }
            }

            QVector<Track> areaColTracks = parseTracks(styleStr(rootStyle, "grid-template-columns"), cw);
            while (areaColTracks.size() < ncols) areaColTracks.push_back({ Track::Fr, 1 });
            areaColTracks.resize(ncols);
            QVector<Track> areaRowTracks = parseTracks(styleStr(rootStyle, "grid-template-rows"), ch);
            while (areaRowTracks.size() < nrows) areaRowTracks.push_back({ Track::Auto, 0 });
            areaRowTracks.resize(nrows);

            const int n = flow.size();
            QVector<double> dW(n), dH(n);
            QVector<Rect> itemRect(n);
            int autoIdx = 0;
            for (int k = 0; k < n; ++k) {
                const QVariantMap ks = flow[k]->property("style").toMap();
                double w = sizeOf(ks, "width", cw);
                double h = sizeOf(ks, "height", ch);
                const double ar = aspectOf(ks);
                if (!std::isnan(ar) && ar > 0) {
                    if (!std::isnan(w) && std::isnan(h)) h = w / ar;
                    else if (!std::isnan(h) && std::isnan(w)) w = h * ar;
                }
                dW[k] = std::isnan(w) ? flow[k]->implicitWidth() : w;
                dH[k] = std::isnan(h) ? flow[k]->implicitHeight() : h;
                const QString an = styleStr(ks, "grid-area");
                const auto it = areas.constFind(an);
                if (!an.isEmpty() && it != areas.constEnd()) {
                    itemRect[k] = *it;
                } else {
                    const int c = autoIdx % ncols;
                    const int r = std::min(nrows - 1, autoIdx / ncols);
                    itemRect[k] = Rect{ r, c, r, c };
                    ++autoIdx;
                }
            }

            // Content sizes for auto tracks: only single-cell items contribute to a track.
            QVector<double> colContent(ncols, 0), rowContent(nrows, 0);
            for (int k = 0; k < n; ++k) {
                const Rect &rc = itemRect[k];
                if (rc.c0 == rc.c1) colContent[rc.c0] = std::max(colContent[rc.c0], dW[k]);
                if (rc.r0 == rc.r1) rowContent[rc.r0] = std::max(rowContent[rc.r0], dH[k]);
            }
            const QVector<double> colSizes = resolveTracks(areaColTracks, cw, colGap, colContent);
            const QVector<double> rowSizes = resolveTracks(areaRowTracks, ch, rowGap, rowContent);

            QVector<double> colX(ncols);
            double xa = ox;
            for (int c = 0; c < ncols; ++c) { colX[c] = xa; xa += colSizes[c] + colGap; }
            QVector<double> rowY(nrows);
            double ya = oy;
            for (int r = 0; r < nrows; ++r) { rowY[r] = ya; ya += rowSizes[r] + rowGap; }

            QString justifyItems = styleStr(rootStyle, "justify-items");
            if (justifyItems.isEmpty()) justifyItems = QStringLiteral("stretch");
            QString alignItems = styleStr(rootStyle, "align-items");
            if (alignItems.isEmpty()) alignItems = QStringLiteral("stretch");

            for (int k = 0; k < n; ++k) {
                const QVariantMap ks = flow[k]->property("style").toMap();
                Rect rc = itemRect[k];
                rc.c0 = std::clamp(rc.c0, 0, ncols - 1);
                rc.c1 = std::clamp(rc.c1, 0, ncols - 1);
                rc.r0 = std::clamp(rc.r0, 0, nrows - 1);
                rc.r1 = std::clamp(rc.r1, 0, nrows - 1);
                const double cellX = colX[rc.c0];
                const double cellY = rowY[rc.r0];
                const double areaW = colX[rc.c1] + colSizes[rc.c1] - colX[rc.c0];
                const double areaH = rowY[rc.r1] + rowSizes[rc.r1] - rowY[rc.r0];
                bool wExplicit = !std::isnan(sizeOf(ks, "width", cw));
                bool hExplicit = !std::isnan(sizeOf(ks, "height", ch));
                if (!std::isnan(aspectOf(ks))) { wExplicit = wExplicit || dW[k] > 0; hExplicit = hExplicit || dH[k] > 0; }
                const double iw2 = (justifyItems == QLatin1String("stretch") && !wExplicit) ? areaW : dW[k];
                const double ih2 = (alignItems == QLatin1String("stretch") && !hExplicit) ? areaH : dH[k];
                double px = cellX, py = cellY;
                if (justifyItems == QLatin1String("center")) px = cellX + (areaW - iw2) / 2;
                else if (justifyItems == QLatin1String("end") || justifyItems == QLatin1String("flex-end")) px = cellX + areaW - iw2;
                if (alignItems == QLatin1String("center")) py = cellY + (areaH - ih2) / 2;
                else if (alignItems == QLatin1String("end") || alignItems == QLatin1String("flex-end")) py = cellY + areaH - ih2;
                place(flow[k], px, py, iw2, ih2);
            }

            double totalW = 0;
            for (int c = 0; c < ncols; ++c) totalW += colSizes[c];
            totalW += colGap * std::max(0, ncols - 1);
            double totalH = 0;
            for (int r = 0; r < nrows; ++r) totalH += rowSizes[r];
            totalH += rowGap * std::max(0, nrows - 1);
            return qMakePair(totalW, totalH);
        }
    }

    QVector<Track> colTracks = parseTracks(styleStr(rootStyle, "grid-template-columns"), cw);
    if (colTracks.isEmpty()) colTracks.push_back({ Track::Fr, 1 });
    const int ncols = colTracks.size();
    const int n = flow.size();
    const int nrows = std::max(1, (n + ncols - 1) / ncols);

    QVector<double> dW(n), dH(n);
    for (int k = 0; k < n; ++k) {
        const QVariantMap ks = flow[k]->property("style").toMap();
        double w = sizeOf(ks, "width", cw);
        double h = sizeOf(ks, "height", ch);
        const double ar = aspectOf(ks);
        if (!std::isnan(ar) && ar > 0) {
            if (!std::isnan(w) && std::isnan(h)) h = w / ar;
            else if (!std::isnan(h) && std::isnan(w)) w = h * ar;
        }
        dW[k] = std::isnan(w) ? flow[k]->implicitWidth() : w;
        dH[k] = std::isnan(h) ? flow[k]->implicitHeight() : h;
    }

    QVector<double> colContent(ncols, 0);
    for (int k = 0; k < n; ++k) { const int c = k % ncols; if (dW[k] > colContent[c]) colContent[c] = dW[k]; }
    const QVector<double> colSizes = resolveTracks(colTracks, cw, colGap, colContent);

    QVector<Track> rowTracks = parseTracks(styleStr(rootStyle, "grid-template-rows"), ch);
    QVector<double> rowContent(nrows, 0);
    for (int k = 0; k < n; ++k) { const int r = k / ncols; if (dH[k] > rowContent[r]) rowContent[r] = dH[k]; }
    QVector<double> rowSizes;
    if (!rowTracks.isEmpty()) {
        while (rowTracks.size() < nrows) rowTracks.push_back({ Track::Auto, 0 });
        rowTracks.resize(nrows);
        rowSizes = resolveTracks(rowTracks, ch, rowGap, rowContent);
    } else {
        rowSizes = rowContent;
    }

    QVector<double> colX(ncols);
    double x = ox;
    for (int c = 0; c < ncols; ++c) { colX[c] = x; x += colSizes[c] + colGap; }
    QVector<double> rowY(nrows);
    double y = oy;
    for (int r = 0; r < nrows; ++r) { rowY[r] = y; y += rowSizes[r] + rowGap; }

    QString justifyItems = styleStr(rootStyle, "justify-items");
    if (justifyItems.isEmpty()) justifyItems = QStringLiteral("stretch");
    QString alignItems = styleStr(rootStyle, "align-items");
    if (alignItems.isEmpty()) alignItems = QStringLiteral("stretch");

    for (int k = 0; k < n; ++k) {
        const QVariantMap ks = flow[k]->property("style").toMap();
        const int col = k % ncols, row = k / ncols;
        const double cellX = colX[col], cellY = rowY[row];
        const double cellW = colSizes[col], cellH = rowSizes[row];
        bool wExplicit = !std::isnan(sizeOf(ks, "width", cw));
        bool hExplicit = !std::isnan(sizeOf(ks, "height", ch));
        if (!std::isnan(aspectOf(ks))) { wExplicit = wExplicit || dW[k] > 0; hExplicit = hExplicit || dH[k] > 0; }
        const double iw2 = (justifyItems == QLatin1String("stretch") && !wExplicit) ? cellW : dW[k];
        const double ih2 = (alignItems == QLatin1String("stretch") && !hExplicit) ? cellH : dH[k];
        double px = cellX, py = cellY;
        if (justifyItems == QLatin1String("center")) px = cellX + (cellW - iw2) / 2;
        else if (justifyItems == QLatin1String("end") || justifyItems == QLatin1String("flex-end")) px = cellX + cellW - iw2;
        if (alignItems == QLatin1String("center")) py = cellY + (cellH - ih2) / 2;
        else if (alignItems == QLatin1String("end") || alignItems == QLatin1String("flex-end")) py = cellY + cellH - ih2;
        place(flow[k], px, py, iw2, ih2);
    }

    double totalW = 0;
    for (int c = 0; c < ncols; ++c) totalW += colSizes[c];
    totalW += colGap * std::max(0, ncols - 1);
    double totalH = 0;
    for (int r = 0; r < nrows; ++r) totalH += rowSizes[r];
    totalH += rowGap * std::max(0, nrows - 1);
    return qMakePair(totalW, totalH);
}

// --- transform-keyframe animation -------------------------------------------------------

QVariantList CssLayoutEngine::buildAnimStops(const QVariantList &frames) const
{
    if (frames.isEmpty())
        return {};
    QVariantList out;
    for (const QVariant &fv : frames) {
        const QVariantMap f = fv.toMap();
        const QVariantMap props = f.value(QStringLiteral("properties")).toMap();
        QVariantMap tr;
        if (props.contains(QStringLiteral("transform")) && m_theme)
            tr = m_theme->parseTransform(props.value(QStringLiteral("transform")).toString());
        QVariantMap stop;
        stop.insert(QStringLiteral("offset"), f.value(QStringLiteral("offset")));
        stop.insert(QStringLiteral("rotate"), tr.value(QStringLiteral("rotate"), 0.0));
        stop.insert(QStringLiteral("scale"), tr.value(QStringLiteral("scale"), 1.0));
        stop.insert(QStringLiteral("tx"), tr.value(QStringLiteral("translateX"), 0.0));
        stop.insert(QStringLiteral("ty"), tr.value(QStringLiteral("translateY"), 0.0));
        out.push_back(stop);
    }
    if (out.first().toMap().value(QStringLiteral("offset")).toDouble() > 0) {
        QVariantMap z;
        z.insert(QStringLiteral("offset"), 0.0);
        z.insert(QStringLiteral("rotate"), 0.0);
        z.insert(QStringLiteral("scale"), 1.0);
        z.insert(QStringLiteral("tx"), 0.0);
        z.insert(QStringLiteral("ty"), 0.0);
        out.prepend(z);
    }
    QVariantMap last = out.last().toMap();
    if (last.value(QStringLiteral("offset")).toDouble() < 1) {
        last.insert(QStringLiteral("offset"), 1.0);
        out.push_back(last);
    }
    return out;
}

void CssLayoutEngine::applyAnim(QQuickItem *root, const QVariantList &stops, qreal p) const
{
    if (!root || stops.size() < 2)
        return;
    QVariantMap a = stops.first().toMap(), b = stops.last().toMap();
    for (int i = 0; i < stops.size() - 1; ++i) {
        const double o0 = stops[i].toMap().value(QStringLiteral("offset")).toDouble();
        const double o1 = stops[i + 1].toMap().value(QStringLiteral("offset")).toDouble();
        if (p >= o0 && p <= o1) { a = stops[i].toMap(); b = stops[i + 1].toMap(); break; }
    }
    const double span = b.value(QStringLiteral("offset")).toDouble() - a.value(QStringLiteral("offset")).toDouble();
    const double t = span > 0 ? (p - a.value(QStringLiteral("offset")).toDouble()) / span : 0;
    const auto lerp = [&](const QString &k) {
        const double av = a.value(k).toDouble(), bv = b.value(k).toDouble();
        return av + (bv - av) * t;
    };
    root->setProperty("_animRotate", lerp(QStringLiteral("rotate")));
    root->setProperty("_animScale", lerp(QStringLiteral("scale")));
    root->setProperty("_animTx", lerp(QStringLiteral("tx")));
    root->setProperty("_animTy", lerp(QStringLiteral("ty")));
}
