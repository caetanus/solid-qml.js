#include "systemtheme.h"

#include <QEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QProcess>
#include <QStyleHints>

// Obey the LOCAL desktop theme WITHOUT a Qt platform-theme plugin. On this box QT_QPA_PLATFORMTHEME
// is qt5ct (a Qt5 tool — it doesn't theme Qt6), so QApplication::palette() is the untrustworthy
// default light Fusion palette. The freedesktop portal is the source of truth Qt DOES read:
// QStyleHints::colorScheme() returns the `prefer-dark` preference. We take dark/light from there,
// the actual accent from GNOME's `accent-color`, and render with the real libadwaita named colours
// (the desktop's gtk-theme is Adwaita) — so solidterm matches the desktop.
namespace {

struct Palette {
    QColor window, view, header, card, popover, text, dim, border, accent, accentText;
};

// Neutral (non-accent) overlay tints for a popover row's hover/pressed state — GNOME's own popover
// menus do NOT tint rows with the accent colour (verified against the real, installed stylesheet:
// `gresource extract /usr/lib/libadwaita-1.so.0 /org/gnome/Adwaita/styles/gtk.css`, rule
// `popover.menu modelbutton:hover, …:selected { background-color: color-mix(in srgb, currentColor
// 10%, transparent); }` / `:active { … 19% …}`, gtk.css:1773/1775). `currentColor` there is the
// popover's own foreground, i.e. our `text` — so these are `text` painted translucent over
// whatever sits under the row, not a fixed grey.
QString rgba(const QColor &c, qreal alpha)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

// GNOME 47 / libadwaita standalone accent name → hex.
QColor accentHex(const QString &name)
{
    static const QHash<QString, QColor> map = {
        { QStringLiteral("blue"), QColor("#3584e4") },   { QStringLiteral("teal"), QColor("#2190a4") },
        { QStringLiteral("green"), QColor("#3a944a") },  { QStringLiteral("yellow"), QColor("#c88800") },
        { QStringLiteral("orange"), QColor("#ed5b00") }, { QStringLiteral("red"), QColor("#e62d42") },
        { QStringLiteral("pink"), QColor("#d56199") },   { QStringLiteral("purple"), QColor("#9141ac") },
        { QStringLiteral("slate"), QColor("#6f8396") },
    };
    return map.value(name, QColor("#3584e4"));
}

// One synchronous gsettings read (best-effort; empty on non-GNOME → callers fall back).
QString gsetting(const QString &schema, const QString &key)
{
    QProcess p;
    p.start(QStringLiteral("gsettings"), { QStringLiteral("get"), schema, key });
    if (!p.waitForFinished(400))
        return QString();
    QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    out.remove(QLatin1Char('\'')); // gsettings quotes strings
    return out;
}

Palette paletteFor(bool dark, const QColor &accent)
{
    Palette p;
    p.accent = accent;
    if (dark) {
        // libadwaita dark named colours.
        p.window = QColor("#242424");
        p.view = QColor("#1e1e1e");   // terminal background
        p.header = QColor("#2e2e2e");
        p.card = QColor("#303030");
        p.popover = QColor("#383838");
        p.text = QColor("#ffffff");   // terminal foreground
        p.dim = QColor("#9a9a9a");
        p.border = QColor("#151515");
        p.accentText = QColor("#ffffff");
    } else {
        p.window = QColor("#fafafb");
        p.view = QColor("#ffffff");
        p.header = QColor("#ffffff");
        p.card = QColor("#ffffff");
        p.popover = QColor("#ffffff");
        p.text = QColor("#2e3436");
        p.dim = QColor("#5e5c64");
        p.border = QColor("#d7d7d9");
        p.accentText = QColor("#ffffff");
    }
    return p;
}

} // namespace

SystemTheme::SystemTheme(QObject *parent)
    : QObject(parent)
{
    refresh();
    // Live re-theme when the desktop toggles dark/light (portal → styleHints).
    if (auto *hints = qApp ? qApp->styleHints() : nullptr)
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] { refresh(); emit changed(); });

    // Icon theme NAME, for <icon> (QmlCss::ThemeIconProvider, vendor/qml-css-engine): same
    // reasoning as the accent/dark-mode reads above — QIcon::themeName()'s own platform-theme
    // detection isn't trustworthy here (qt5ct.conf on this box names a DIFFERENT theme,
    // "Papirus-Adapta-Nokto-Maia", than gsettings' real "Papirus-Dark"; qt5ct doesn't theme Qt6
    // at all per the file comment above), so read the GNOME setting directly and set it
    // explicitly, once (unlike accent/dark-mode, an icon theme change mid-session is rare enough
    // that it doesn't need a live portal/gsettings watch — a fresh launch picks up a change).
    const QString iconTheme = gsetting(QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("icon-theme"));
    if (!iconTheme.isEmpty())
        QIcon::setThemeName(iconTheme);
}

void SystemTheme::refresh()
{
    m_accent = accentHex(gsetting(QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("accent-color")));
    if (auto *hints = qApp ? qApp->styleHints() : nullptr) {
        const Qt::ColorScheme cs = hints->colorScheme();
        if (cs != Qt::ColorScheme::Unknown) {
            m_dark = cs == Qt::ColorScheme::Dark;
            return;
        }
    }
    // Fallbacks: the GNOME preference directly, then the palette lightness.
    const QString pref = gsetting(QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("color-scheme"));
    m_dark = pref.contains(QLatin1String("dark"))
        || (qApp && qApp->palette().color(QPalette::Window).lightness() < 128);
}

bool SystemTheme::dark() const { return m_dark; }
QColor SystemTheme::accent() const { return m_accent; }

QColor SystemTheme::base() const { return paletteFor(m_dark, m_accent).view; }
QColor SystemTheme::text() const { return paletteFor(m_dark, m_accent).text; }
QColor SystemTheme::window() const { return paletteFor(m_dark, m_accent).window; }

bool SystemTheme::eventFilter(QObject *watched, QEvent *event)
{
    return QObject::eventFilter(watched, event);
}

void SystemTheme::setUiOpacity(qreal v)
{
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(m_uiOpacity, v))
        return;
    m_uiOpacity = v;
    emit changed();
}

QString SystemTheme::styleSheet() const
{
    const Palette p = paletteFor(m_dark, m_accent);
    const auto h = [](const QColor &c) { return c.name(QColor::HexRgb); };
    // When translucent, the app root goes transparent so the (alpha) window reveals the desktop
    // behind the terminal + gaps; the header/status chrome stay solid (tilix-style glass). At full
    // opacity the root is the solid window colour, so nothing shows through.
    const QString rootBg = m_uiOpacity < 1.0 ? QStringLiteral("transparent") : h(p.window);

    // Menu/MenuItem (solidterm's right-click menu + pane-header dropdown): a neutral row tint, NOT
    // accent — `currentColor` (our `text`) painted translucent over the card, per the real Adwaita
    // values above. The SAME `color-mix(in srgb, currentColor N%, transparent)` formula is also how
    // real Adwaita paints an `entry`'s own background (gtk.css:988) and a flat button's hover/active
    // state — reused below for the search bar's field fill and icon-button hover/press.
    const QString menuHover = rgba(p.text, 0.10);
    const QString menuActive = rgba(p.text, 0.19);
    // Focus ring: `entry:focus-within { outline-color: color-mix(in srgb, var(--accent-color) 50%,
    // transparent); outline-width: 2px; outline-offset: -2px }` (gtk.css:994) — an OUTLINE (hugging,
    // slightly overlapping the edge), not a border colour swap.
    const QString focusRing = rgba(p.accent, 0.5);
    // The popover's own drop shadow is scheme-INDEPENDENT in real Adwaita too (gtk.css:2107 —
    // `popover > arrow, popover > contents { border: 1px solid RGB(0 0 0/14%); box-shadow: 0 1px
    // 5px 1px RGB(0 0 0/9%), 0 2px 14px 3px RGB(0 0 0/5%); }`, same rule for light and dark — no
    // `@media (prefers-color-scheme: dark)` override exists for it). Two problems stop us using
    // those two values verbatim: (1) our CssRect only paints the FIRST non-inset shadow in a
    // `box-shadow` list (cssrect.cpp:1053-1060 picks one and breaks — no layered shadows), and
    // (2) each one ALONE is barely perceptible (measured: the 5%-alpha layer renders at ~1-3/255
    // alpha over ~5px before the hard card edge) — together they read as "soft card shadow" to the
    // eye, but a single 5%(or 9%)-alpha layer alone reads as "no shadow". So this is a deliberate
    // single-shadow approximation of the pair's combined WEIGHT (not a literal gtk.css value),
    // verified by pixel-sampling an actual grab (docs/evidence/solidterm/menu-*): soft blur, a
    // little spread for body, offset down (elevated-card read), opacity strong enough to survive
    // one layer instead of two stacked ones.
    const QString menuShadow = QStringLiteral("0 3px 18px 2px rgba(0, 0, 0, 0.22)");

    // Semantic app classes → the desktop palette. Loaded OVER the structural term.css.
    return QStringLiteral(R"(
.term-root { background: %11; color: %6; }
.term-header { background: %3; border-bottom: 1px solid %8; }
.term-title { color: %6; }
.term-gear { color: %7; }
.term-gear:hover { background: %4; }
.term-gear:active { background: %5; }
.tabbar { background: %3; }
.tab { background: %1; }
.tab-label { color: %7; }
.tab-active { background: %2; border-top: 2px solid %9; }
.tab-active .tab-label { color: %6; }
.tab-x { color: %7; }
.tab-x:hover { background: %5; }
.tab-new { color: %7; }
.tab-new:hover { background: %4; }
.searchbar { background: %3; border-bottom: 1px solid %8; }
.search-icon { color: %7; }
/* The tint lives on the WRAPPER now (`.search-in-wrap`) — the <input> inside it goes transparent/
   borderless, so icon + typed text read as one field. The focus ring stays on the input itself:
   the engine has no `:focus-within` to put it on the wrapper instead, so it hugs the input's own
   box (very slightly short of the icon's edge) rather than nothing. */
.search-in-wrap { background: %13; }
.search-in { background: transparent; color: %6; border: none; }
.search-in:focus { outline: 2px solid %15; outline-offset: -2px; }
.search-count { color: %7; }
.search-btn { color: %6; }
.search-btn:hover { background: %13; }
.search-btn:active { background: %14; }
.term-status { background: %3; border-top: 1px solid %8; }
.term-status-t { color: %9; }
.term-hint { color: %7; }
.popup { background: %5; border: 1px solid %8; box-shadow: %12; }
.option:hover { background: %9; }
.option-label { color: %6; }
.option-label:hover { color: %10; }
.sep { background: %8; }
/* solidterm's own menus (<Menu class="tmenu">/<ContextMenu class="tmenu">, src/term.tsx) — more
   specific than the bare `.option`/`.option-label` rules above, so they win for THIS component only
   and leave Select's accent-highlighted dropdown (which also carries the generic `.popup`/`.option`
   classes, src/widgets/select.cpp) untouched. Real values: gtk.css:1773/1775 (hover/active tint).
   COMPOUND (`.tmenu.option`, no space), not descendant — see term.css's `.tmenu.popup` comment for
   why: "tmenu" rides on the row's OWN cssClass (MenuItem::menuClass), not a real ancestor. */
.tmenu.option:hover { background: %13; }
.tmenu.option:active { background: %14; }
.tmenu.option-label:hover { color: %6; }
.cfg { background: %1; color: %6; }
.cfg-scroll { background: %1; }
.cfg-group { color: %7; }
.cfg-path { color: %7; }
.cfg-card { background: %4; border: 1px solid %8; }
.cfg-l { color: %6; }
/* The controls' INNER parts: a TextField paints with the colour it INHERITS (falling back to a
   dark default), and a Select's value/chevron are their own text nodes — so the rows must carry an
   inheritable colour and the parts need explicit rules, or both render near-black on a dark theme. */
.cfg-card, .cfg-row, .cfg-krow, .cfg-slide { color: %6; }
.cfg-sel .value, .cfg-sel .chevron { color: %6; }
.cfg-num .value, .cfg-in .value { color: %6; }
.cfg-div { background: %8; }
/* text inputs = inset (view shade); dropdowns/browse = raised button (popover shade) */
.cfg-in, .cfg-num, .cfg-in-img { background: %2; color: %6; border: 1px solid %8; }
.cfg-sel, .cfg-browse { background: %5; color: %6; border: 1px solid %8; }
/* A button's label is a text NODE inside it: colour on the button alone does not reach it, so the
   label needs its own scoped rule (the same reason the gallery writes `.nav button.active text`). */
.cfg-browse text { color: %6; }
.cfg-close text { color: %10; }
.cfg-sel:hover, .cfg-browse:hover { background: %3; }
.cfg-in:focus, .cfg-num:focus, .cfg-sel:focus, .cfg-in-img:focus { border: 1px solid %9; }
.cfg-slideval { color: %6; }
.cfg-range .track { background: %8; }
.cfg-range .track-fill { background: %9; }
.cfg-range .handle { background: %9; }
.cfg-actions { background: %1; border-top: 1px solid %8; }
.cfg-close { background: %9; color: %10; }
.cfg-close:hover { background: %9; }
.paste-hint { color: %7; }
.paste-cancel { background: %4; color: %6; }
.ov-tab { color: %7; }
.ov-title { color: %6; }
.ov-row:hover { background: %4; }
.ov-active { background: %9; }
.ov-active .ov-title { color: %10; }
.ov-active .ov-tab { color: %10; }
)")
        .arg(h(p.window), h(p.view), h(p.header), h(p.card), h(p.popover),
             h(p.text), h(p.dim), h(p.border), h(p.accent), h(p.accentText))
        .arg(rootBg)
        .arg(menuShadow, menuHover, menuActive)
        .arg(focusRing);
}
