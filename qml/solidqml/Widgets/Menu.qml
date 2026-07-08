// Menu — the <Menu> popup in module solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component). A T.Menu (a QQuickPopup) with CSS support. The transpiler drives the ORCHESTRATION
// (standalone controlled `open` Binding, or the self-managed `trigger` toggle + Window-deactivation
// close) from the host Item around this instance; this component holds only the popup SHELL:
//
//   - popupType Popup.Item: an in-scene overlay popup, NOT Popup.Window — Wayland compositors don't
//     honor client toplevel positioning, so a window-type menu opens at the top of the screen once
//     the app floats (same fix as <select>/<input type=date>). It opens relative to its host.
//   - Templates popups have NO implicit-size policy (that's the style's job, and we ARE the style):
//     without explicit sizes the menu opens 0x0. The width floor lives on the POPUP, not the
//     background — a background CssFill's implicitWidth is clobbered by the engine's content pass.
//   - padding ≥ border-width keeps the popup CssFill border from clipping rows (G3).
//   - cssAncestor: popup contents reparent to the window Overlay, severing the visual chain CSS
//     scoping walks — background and contentItem (SIBLING slots covering every descendant) each
//     re-anchor the walk at the host the transpiler passes in.
//   - __closedAt: recorded on every close so the trigger form can swallow the dismissing click
//     (QToolButton+QMenu idiom). menuClosed() re-exposes the close so the author's onClose can run
//     alongside (the internal onClosed is taken by the __closedAt record).
//
// The transpiler emits (inside a host Item):
//   W.Menu { id: __menuN; cssAncestor: <host>; authorClass: […]; [title]; [x]; [y];
//            [onMenuClosed: {…}]; <W.MenuItem>/<W.MenuSeparator> children }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

T.Menu {
    id: ctl
    property Item cssAncestor: null
    // Author `class` on <Menu> — merged with "popup" for the background scope.
    property var authorClass: []
    property double __closedAt: 0
    signal menuClosed()

    popupType: T.Popup.Item
    implicitWidth: Math.max(180, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    padding: 1
    onClosed: { __closedAt = Date.now(); menuClosed() }

    background: Css.CssFill {
        property Item cssAncestor: ctl.cssAncestor
        cssPrimitive: "div"
        cssClass: ctl.authorClass.concat(["popup"])
    }
    contentItem: ListView {
        property Item cssAncestor: ctl.cssAncestor
        clip: true
        model: ctl.contentModel
        currentIndex: ctl.currentIndex
        implicitHeight: contentHeight
        // Arrow keys belong to the MENU (activateNext/PreviousItem skips separators and moves
        // `highlighted`); an interactive ListView consumes the press itself once focus sits on
        // an item, so navigation only advances every other key (QTBUG-17051 — Basic's Menu
        // disables interactivity for menus that fit; explicit keyNavigationEnabled false covers
        // the flickable-long ones too).
        keyNavigationEnabled: false
        interactive: Window.window
                     ? contentHeight + ctl.topPadding + ctl.bottomPadding > ctl.height
                     : false
    }
}
