#include "menuwidgets.h"

#include "snippetwidget.h"

#include <QCursor>
#include <QDateTime>

namespace {

// Transparent window margin reserved on every side of the popup so the outset `box-shadow` (a
// MultiEffect blur that bleeds past the "popup" CssFill's own bounds — see cssrect.cpp's outset-
// shadow Loader) has somewhere to paint without being clipped by the popup WINDOW's own surface.
// This can't be the usual Popup recipe of NEGATIVE insets (qquickpopup.cpp doc comment, "Negative
// insets can be used to make the background larger than the popup… to place a shadow outside the
// popup's boundaries"): QQuickPopupPrivate::windowInsets() (qquickpopup.cpp:671-705) hard-codes
// `{0,0,0,0}` on Wayland regardless of sign ("assume the compositor will draw window decorations"),
// so a negative inset would never grow the window here. POSITIVE insets work instead: they shrink
// the background WITHIN space the control's own implicit size (padding, below) already reserves —
// Qt (via QQuickPopupWindow::implicit{Width,Height}Changed) sizes the Wayland popup window to
// exactly that implicit size, insets or not. Qt itself already makes the window's surface
// transparent for a `Qt::Popup`-flagged window (qquickpopupwindow.cpp:73-75), so nothing else is
// needed to let the blur show through.
constexpr qreal kMenuShadowMargin = 32.0;
// Real Adwaita popover.menu content gutter: `popover.menu contents > list { padding: 6px 0 }`
// (gtk.css:1809, extracted from /usr/lib/libadwaita-1.so.0's `/org/gnome/Adwaita/styles/gtk.css`
// gresource) — rows sit this far inside the visible (inset) card, not inside the shadow margin.
constexpr qreal kMenuContentGutter = 6.0;

// Original Menu.qml slot internals — `root` = the C++ menu.
const char *kMenuBackground = R"(import QtQuick
import qmlcss 1.0 as Css
Css.CssFill {
    property Item cssAncestor: root.cssAncestor
    cssPrimitive: "div"
    cssClass: (root.authorClass || []).concat(["popup"])
}
)";

const char *kMenuContent = R"(import QtQuick
ListView {
    id: lv
    property Item cssAncestor: root.cssAncestor
    clip: true
    model: root.contentModel
    currentIndex: root.currentIndex
    implicitHeight: contentHeight
    // A vertical ListView never auto-sizes to its delegates' width (only Row/Column propagate
    // implicit sizes) — `model` here is an OBJECT model (the actual MenuItem/MenuSeparator
    // instances, not a data+delegate pair), so nothing ever fed the Menu's own implicitWidth the
    // widest row's natural size; it silently fell back to the Menu's bare floor, and a long label
    // (e.g. "Clear scrollback") wrapped inside whatever was left after padding. A native menu never
    // wraps an item — the POPUP sizes to the content, not the other way round. `count`/`itemAtIndex`
    // re-run this whenever a row is added/removed/relabelled (itemAtIndex's reads of
    // `implicitWidth` are real property reads, so the binding re-evaluates when any row's own
    // implicitWidth changes too — e.g. once a row's text metrics settle after font load).
    implicitWidth: {
        var w = 0
        for (var i = 0; i < count; ++i) {
            var it = lv.itemAtIndex(i)
            if (it) w = Math.max(w, it.implicitWidth)
        }
        return w
    }
    // Arrow keys belong to the MENU (activateNext/PreviousItem skips separators and moves
    // `highlighted`); an interactive ListView consumes the press itself once focus sits on
    // an item, so navigation only advances every other key (QTBUG-17051 — Basic's Menu
    // disables interactivity for menus that fit; explicit keyNavigationEnabled false covers
    // the flickable-long ones too).
    keyNavigationEnabled: false
    interactive: Window.window
                 ? contentHeight + root.topPadding + root.bottomPadding > root.height
                 : false
}
)";

// Original MenuItem.qml slot internals. `menuClass` (the owning Menu's author class, e.g.
// "tmenu") is merged straight into this row's OWN cssClass — see the Q_PROPERTY doc comment in
// menuwidgets.h for why a descendant selector can't reach it instead.
const char *kItemBackground = R"(import qmlcss 1.0 as Css
Css.CssFill {
    cssPrimitive: "div"
    cssClass: (root.menuClass || []).concat(["option"])
    cssState: (root.pressed ? ["active"] : []).concat(root.highlighted ? ["hover"] : [])
}
)";

// A real checkmark for `checkable`/`checked` MenuItems (e.g. solidterm's "Read only" toggle),
// instead of the item author splicing "✓" into its own label text. Adwaita reserves the check's
// column whether checked or not (gtk.css:1757 `popover.menu check { min-width: 14px; padding: 2px }`
// + margin-right 6px, gtk.css:1767) — approximated here with a fixed-width glyph column since we
// draw the mark as text, not a separate indicator node.
const char *kItemLabel = R"(import qmlcss 1.0 as Css
Css.CssText {
    cssPrimitive: ""
    cssClass: (root.menuClass || []).concat(["option-label"])
    styledText: true
    text: (root.checkable ? (root.checked ? "✓ " : "   ") : "") + root.__mnemonicMarkup(root.text)
}
)";

// Original MenuSeparator.qml contentItem: a 1px `.sep` rule; padding ≥ the .sep height keeps the
// rule off the popup's border. `menuClass` as above.
const char *kSeparatorRule = R"(import qmlcss 1.0 as Css
Css.CssRect {
    cssPrimitive: "div"
    cssClass: (root.menuClass || []).concat(["sep"])
    implicitHeight: 1
}
)";

} // namespace

namespace SolidWidgets {

// ─── Menu ───────────────────────────────────────────────────────────────────────────────────────

Menu::Menu(QObject *parent)
    : QQuickMenu(parent)
{
    // Popup.Window: a menu is its own popup surface (owner: "o popup é uma janela por si só") so it
    // overflows the app window instead of clipping to the scene. CRITICAL Wayland constraint: a
    // Popup.Window grabs input when shown, and the grab needs a FRESH input serial — so the menu MUST
    // be opened SYNCHRONOUSLY inside the triggering input handler (button onClicked / a C++ signal
    // fired during the mouse event), via popup(x, y). Opening it from a DEFERRED reactive signal
    // (`<Menu open={sig}>`) fails the grab ("Failed to create grabbing popup"). The emitter therefore
    // opens every menu imperatively: <Menu ref={r}> → r.open(x,y), <Menu trigger>, <ContextMenu>.
    setPopupType(QQuickPopup::Window);
    // Reserve the shadow's transparent gutter (see kMenuShadowMargin above) by pulling the visible
    // "popup" background IN from the control edge…
    setTopInset(kMenuShadowMargin);
    setLeftInset(kMenuShadowMargin);
    setRightInset(kMenuShadowMargin);
    setBottomInset(kMenuShadowMargin);
    // …and pushing the content (the row ListView) in far enough to clear BOTH that margin and the
    // real Adwaita row gutter on top of it (also ≥ the 1px border width, so it never clips — G3).
    setPadding(kMenuShadowMargin + kMenuContentGutter);
    connect(this, &QQuickPopup::closed, this, [this] {
        m_closedAt = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
        emit closedAtChanged();
        emit menuClosed();
    });
    // Templates popups have NO implicit-size policy (that's the style's job, and we ARE the
    // style): without explicit sizes the menu opens 0x0. The width floor lives on the POPUP, not
    // the background — a background CssFill's implicitWidth is clobbered by the engine's content pass.
    connect(this, &QQuickPopup::implicitBackgroundWidthChanged, this, &Menu::syncImplicit);
    connect(this, &QQuickPopup::implicitBackgroundHeightChanged, this, &Menu::syncImplicit);
    connect(this, &QQuickPopup::implicitContentWidthChanged, this, &Menu::syncImplicit);
    connect(this, &QQuickPopup::implicitContentHeightChanged, this, &Menu::syncImplicit);
    connect(this, &QQuickPopup::paddingChanged, this, &Menu::syncImplicit);
}

void Menu::setCssAncestor(QQuickItem *v)
{
    if (m_cssAncestor == v)
        return;
    m_cssAncestor = v;
    emit cssAncestorChanged();
}

void Menu::setAuthorClass(const QVariant &v)
{
    if (m_authorClass == v)
        return;
    m_authorClass = v;
    emit authorClassChanged();
}

void Menu::classBegin()
{
    QQuickMenu::classBegin();
    // MUST happen here, not in componentComplete() — see the declaration's comment in
    // menuwidgets.h: QQuickMenu::contentData()'s getter (called for every declared child,
    // including a <For>'s Repeater) forces a DEFAULT content item into existence on first access
    // if none is set yet; classBegin() runs before the engine processes this object's own children,
    // winning that race so a Repeater reparents into OUR content item, never a discarded default.
    setBackground(createBoundItem(this, QStringLiteral("solidwidgets-menu-bg"), kMenuBackground));
    setContentItem(createBoundItem(this, QStringLiteral("solidwidgets-menu-content"), kMenuContent));
}

void Menu::componentComplete()
{
    QQuickMenu::componentComplete();
    syncImplicit();
}

void Menu::syncImplicit()
{
    // Real Adwaita floor: `popover.menu > contents { min-width: 120px }` (gtk.css:1721) — applied
    // to the CONTENT width (before padding), not the window total, so a short menu (e.g. a 2-item
    // one) still gets a sensible card instead of shrink-wrapping to its narrowest label.
    constexpr qreal kContentFloor = 120.0;
    setImplicitWidth(qMax(qMax(implicitContentWidth(), kContentFloor) + leftPadding() + rightPadding(),
                           implicitBackgroundWidth() + leftInset() + rightInset()));
    setImplicitHeight(qMax(implicitBackgroundHeight() + topInset() + bottomInset(),
                           implicitContentHeight() + topPadding() + bottomPadding()));
}

// ─── MenuItem ───────────────────────────────────────────────────────────────────────────────────

MenuItem::MenuItem(QQuickItem *parent)
    : QQuickMenuItem(parent)
{
    // The menu highlights the hovered/keyboard-current row via `highlighted`; hoverEnabled makes
    // mouse rows current (Controls styles rely on the same).
    setHoverEnabled(true);
    setLeftPadding(12);
    setRightPadding(12);
    setImplicitHeight(32);
    // Desktop affordance: a pointing-hand cursor over the row.
    setCursor(QCursor(Qt::PointingHandCursor));
    connect(this, &QQuickControl::implicitContentWidthChanged, this, &MenuItem::syncImplicit);
    connect(this, &QQuickControl::paddingChanged, this, &MenuItem::syncImplicit);
}

void MenuItem::setMenuClass(const QVariant &v)
{
    if (m_menuClass == v)
        return;
    m_menuClass = v;
    emit menuClassChanged();
}

QString MenuItem::__mnemonicMarkup(const QString &s) const
{
    QString out;
    out.reserve(s.size() + 8);
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('&') && i + 1 < s.size()) {
            ++i;
            if (s.at(i) == QLatin1Char('&'))
                out += QLatin1String("&amp;");
            else
                out += QLatin1String("<u>") + s.at(i) + QLatin1String("</u>");
        } else if (c == QLatin1Char('<')) {
            out += QLatin1String("&lt;");
        } else if (c == QLatin1Char('>')) {
            out += QLatin1String("&gt;");
        } else {
            out += c;
        }
    }
    return out;
}

void MenuItem::componentComplete()
{
    QQuickMenuItem::componentComplete();
    setBackground(createBoundItem(this, QStringLiteral("solidwidgets-menuitem-bg"), kItemBackground));
    setContentItem(createBoundItem(this, QStringLiteral("solidwidgets-menuitem-label"), kItemLabel));
    syncImplicit();
}

void MenuItem::syncImplicit()
{
    // Real Adwaita: `popover.menu modelbutton { min-height: 32px; min-width: 40px; padding: 0
    // 12px }` (gtk.css:1771) — a ROW's own floor is just enough for an icon-only/very short label;
    // the CARD's overall minimum (Menu::syncImplicit's 120px content floor, gtk.css:1721) is a
    // separate, larger number. A per-row floor of 180 (the old value) forced every row this wide
    // regardless of its own text, which is backwards from how the widest row should actually drive
    // the card's width.
    setImplicitWidth(qMax<qreal>(40.0, implicitContentWidth() + leftPadding() + rightPadding()));
    setImplicitHeight(32);
}

// ─── MenuSeparator ──────────────────────────────────────────────────────────────────────────────

MenuSeparator::MenuSeparator(QQuickItem *parent)
    : QQuickMenuSeparator(parent)
{
    setPadding(4);
    // A separator never DEMANDS width (it's a line, not content) — it just stretches to whatever
    // the menu ends up being, the same way every row does. A fixed 180 here used to feed straight
    // into "widest row" width math and could force the card that wide even when every real item
    // was shorter; 1 (its own line) lets the actual item labels drive the card's width instead.
    setImplicitWidth(1);
    setImplicitHeight(9);
}

void MenuSeparator::setMenuClass(const QVariant &v)
{
    if (m_menuClass == v)
        return;
    m_menuClass = v;
    emit menuClassChanged();
}

void MenuSeparator::componentComplete()
{
    QQuickMenuSeparator::componentComplete();
    setContentItem(createBoundItem(this, QStringLiteral("solidwidgets-menuseparator-rule"), kSeparatorRule));
    setImplicitWidth(1);
    setImplicitHeight(9);
}

} // namespace SolidWidgets
