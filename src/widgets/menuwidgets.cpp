#include "menuwidgets.h"

#include "snippetwidget.h"

#include <QCursor>
#include <QDateTime>

namespace {

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
    property Item cssAncestor: root.cssAncestor
    clip: true
    model: root.contentModel
    currentIndex: root.currentIndex
    implicitHeight: contentHeight
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

// Original MenuItem.qml slot internals.
const char *kItemBackground = R"(import qmlcss 1.0 as Css
Css.CssFill {
    cssPrimitive: "div"
    cssClass: ["option"]
    cssState: root.highlighted ? ["hover"] : []
}
)";

const char *kItemLabel = R"(import qmlcss 1.0 as Css
Css.CssText {
    cssPrimitive: ""
    cssClass: ["option-label"]
    styledText: true
    text: root.__mnemonicMarkup(root.text)
}
)";

// Original MenuSeparator.qml contentItem: a 1px `.sep` rule; padding ≥ the .sep height keeps the
// rule off the popup's border.
const char *kSeparatorRule = R"(import qmlcss 1.0 as Css
Css.CssRect {
    cssPrimitive: "div"
    cssClass: ["sep"]
    implicitHeight: 1
}
)";

} // namespace

namespace SolidWidgets {

// ─── Menu ───────────────────────────────────────────────────────────────────────────────────────

Menu::Menu(QObject *parent)
    : QQuickMenu(parent)
{
    // In-scene overlay popup, NOT Popup.Window — Wayland compositors don't honor client toplevel
    // positioning, so a window-type menu opens at the top of the screen once the app floats.
    setPopupType(QQuickPopup::Item);
    // padding ≥ border-width keeps the popup CssFill border from clipping rows (G3).
    setPadding(1);
    connect(this, &QQuickPopup::closed, this, [this] {
        m_closedAt = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
        emit closedAtChanged();
        emit menuClosed();
    });
    // Templates popups have NO implicit-size policy (that's the style's job, and we ARE the
    // style): without explicit sizes the menu opens 0x0. The width floor lives on the POPUP, not
    // the background — a background CssFill's implicitWidth is clobbered by the engine's content pass.
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

void Menu::componentComplete()
{
    QQuickMenu::componentComplete();
    setBackground(createBoundItem(this, QStringLiteral("solidwidgets-menu-bg"), kMenuBackground));
    setContentItem(createBoundItem(this, QStringLiteral("solidwidgets-menu-content"), kMenuContent));
    syncImplicit();
}

void Menu::syncImplicit()
{
    setImplicitWidth(qMax<qreal>(180.0, implicitContentWidth() + leftPadding() + rightPadding()));
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
    setImplicitWidth(qMax<qreal>(180.0, implicitContentWidth() + leftPadding() + rightPadding()));
    setImplicitHeight(32);
}

// ─── MenuSeparator ──────────────────────────────────────────────────────────────────────────────

MenuSeparator::MenuSeparator(QQuickItem *parent)
    : QQuickMenuSeparator(parent)
{
    setPadding(4);
    setImplicitWidth(180);
    setImplicitHeight(9);
}

void MenuSeparator::componentComplete()
{
    QQuickMenuSeparator::componentComplete();
    setContentItem(createBoundItem(this, QStringLiteral("solidwidgets-menuseparator-rule"), kSeparatorRule));
    setImplicitWidth(180);
    setImplicitHeight(9);
}

} // namespace SolidWidgets
