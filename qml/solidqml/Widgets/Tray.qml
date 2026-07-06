// Tray — the <Tray> component in module solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component). Qt.labs.platform SystemTrayIcon is a QObject, NOT an Item — so it is wrapped in a
// zero-size Item host that lets the tag sit anywhere in the tree (and `shown` folds into the icon's
// own `visible`, since Item visibility does not cascade to non-Item resources).
//
// The `menu` is set by the transpiler ONLY when the author gave <MenuItem> children — a Platform.Menu
// carrying the item handlers (arbitrary author JS) is constructed in the emit and assigned through the
// `menu` alias, so this component never fabricates an empty menu: registration semantics are identical
// to the hand-emitted form.
//
// The transpiler emits:  W.Tray { shown; tooltip; iconSource; onActivated; menu: Platform.Menu {…} }
import QtQuick
import Qt.labs.platform 1.1 as Platform

Item {
    id: root
    width: 0
    height: 0
    property alias tooltip: tray.tooltip
    property url iconSource: ""
    property bool shown: true
    property alias menu: tray.menu
    signal activated(var reason)

    Platform.SystemTrayIcon {
        id: tray
        visible: root.shown
        icon.source: root.iconSource
        onActivated: root.activated(reason)
    }
}
