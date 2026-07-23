#include "systemtheme.h"

#include <QEvent>
#include <QGuiApplication>
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
    // Live re-theme when the desktop toggles dark/light (portal → styleHints).
    if (auto *hints = qApp ? qApp->styleHints() : nullptr)
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] { emit changed(); });
}

bool SystemTheme::dark() const
{
    if (auto *hints = qApp ? qApp->styleHints() : nullptr) {
        const Qt::ColorScheme cs = hints->colorScheme();
        if (cs != Qt::ColorScheme::Unknown)
            return cs == Qt::ColorScheme::Dark;
    }
    // Fallbacks: the GNOME preference directly, then the palette lightness.
    const QString pref = gsetting(QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("color-scheme"));
    if (pref.contains(QLatin1String("dark")))
        return true;
    return qApp && qApp->palette().color(QPalette::Window).lightness() < 128;
}

QColor SystemTheme::accent() const
{
    const QString name = gsetting(QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("accent-color"));
    return accentHex(name);
}

QColor SystemTheme::base() const { return paletteFor(dark(), accent()).view; }
QColor SystemTheme::text() const { return paletteFor(dark(), accent()).text; }
QColor SystemTheme::window() const { return paletteFor(dark(), accent()).window; }

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
    const Palette p = paletteFor(dark(), accent());
    const auto h = [](const QColor &c) { return c.name(QColor::HexRgb); };
    // When translucent, the app root goes transparent so the (alpha) window reveals the desktop
    // behind the terminal + gaps; the header/status chrome stay solid (tilix-style glass). At full
    // opacity the root is the solid window colour, so nothing shows through.
    const QString rootBg = m_uiOpacity < 1.0 ? QStringLiteral("transparent") : h(p.window);

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
.search-in { background: %2; color: %6; border: 1px solid %8; }
.search-in:focus { border: 1px solid %9; }
.search-count { color: %7; }
.search-btn { color: %6; }
.search-btn:hover { background: %4; }
.term-status { background: %3; border-top: 1px solid %8; }
.term-status-t { color: %9; }
.term-hint { color: %7; }
.popup { background: %5; border: 1px solid %8; }
.option:hover { background: %9; }
.option-label { color: %6; }
.option-label:hover { color: %10; }
.sep { background: %8; }
.cfg { background: %1; color: %6; }
.cfg-group { color: %7; }
.cfg-path { color: %7; }
.cfg-card { background: %4; border: 1px solid %8; }
.cfg-l { color: %6; }
.cfg-div { background: %8; }
.cfg-in, .cfg-num, .cfg-sel, .cfg-in-img { background: %2; color: %6; border: 1px solid %8; }
.cfg-browse { background: %4; color: %6; }
.cfg-browse:hover { background: %5; }
.cfg-in:focus, .cfg-num:focus, .cfg-sel:focus { border: 1px solid %9; }
.cfg-close { background: %9; color: %10; }
.cfg-close:hover { background: %9; }
.paste-hint { color: %7; }
.paste-cancel { background: %4; color: %6; }
)")
        .arg(h(p.window), h(p.view), h(p.header), h(p.card), h(p.popover),
             h(p.text), h(p.dim), h(p.border), h(p.accent), h(p.accentText))
        .arg(rootBg);
}
