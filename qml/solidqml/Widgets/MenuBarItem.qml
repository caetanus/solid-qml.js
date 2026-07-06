// MenuBarItem — one top-level entry in module solidqml.Widgets (owner directive 2026-07-05: one .qml
// per component). A T.MenuBarItem with CSS support, only ever instantiated as a child of W.MenuBar
// (its root lands in the bar's contentData → contentModel). The transpiler sets `menu` (an object
// binding to a W.Menu submenu) directly; `highlighted` = this item's menu is the open one (set by
// the bar). The .menubar-label reads the attached submenu's title.
//
// Qt::Popup semantics: the drop-down must not linger when the app deactivates — close it on
// Window-deactivation (the same idiom the standalone <Menu> host uses).
//
// The transpiler emits:  W.MenuBarItem { id: __mbiN; menu: W.Menu { … } }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.MenuBarItem {
    id: ctl
    hoverEnabled: true
    // A `&F` mnemonic in the menu title: setting AbstractButton.text auto-registers the Alt+letter
    // shortcut (setText → QKeySequence::mnemonic), so QQuickMenuBar's built-in Alt-nav opens this menu
    // (study §6). The `&` is stripped for DISPLAY in the contentItem below (Qt strips it only for a11y).
    text: ctl.menu ? ctl.menu.title : ""
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    leftPadding: 12
    rightPadding: 12
    topPadding: 6
    bottomPadding: 6
    Window.onActiveChanged: if (!Window.active && ctl.menu) ctl.menu.close()

    background: Css.CssFill {
        cssPrimitive: "div"
        cssClass: ["menubar-item"]
        // `highlighted` = this item's menu is the open one (set by the menu bar).
        cssState: (ctl.hovered ? ["hover"] : []).concat(ctl.highlighted ? ["open"] : [])
    }
    contentItem: Css.CssText {
        cssPrimitive: ""
        cssClass: ["menubar-label"]
        // Strip the `&N` mnemonic marker for display (a literal `&` is written `&&`), same as MenuItem.
        text: (ctl.menu ? ctl.menu.title : "").replace(/&(.)/g, "$1")
    }
}
