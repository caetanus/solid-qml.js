// Tabstop — the keyboard tab-focus marker (study §6). Drop one into any Window (the app root AND
// each dialog Window) with `window:` set; it follows the window's activeFocusItem and frames whatever
// holds keyboard focus. Universal — no per-widget CSS.
//
// Styled via the `::tab-stop` PSEUDO-ELEMENT (owner call): the ring is a decorative overlay, like
// `::before`/`::after`, so a pseudo-element is the right CSS concept — and it's cleaner than a made-up
// type selector or repurposing `:focus`/`:focus-visible` (element states). The ring declares
// `cssPart: "tab-stop"`, so a rule
//   ::tab-stop { border: 2px solid #38bdf8; border-radius: 6px; background: transparent; }
// styles it; the app sheet ships a bright default and can override it. No MouseArea → clicks pass
// through. A frame timer tracks the item's screen rect (mapToItem isn't reactive to scroll on its own).
import QtQuick
import qmlcss 1.0 as Css

Css.CssRect {
    id: ring
    property var window: null

    cssPrimitive: ""
    cssPart: "tab-stop"
    z: 1000000
    visible: solidTabstop.enabled && !!window && !!window.activeFocusItem
             && window.activeFocusItem !== window.contentItem

    Timer {
        interval: 16
        repeat: true
        running: ring.visible
        triggeredOnStart: true
        onTriggered: {
            var t = ring.window ? ring.window.activeFocusItem : null;
            if (!t) return;
            var p = t.mapToItem(ring.parent, 0, 0);
            ring.x = p.x - 2;
            ring.y = p.y - 2;
            ring.width = t.width + 4;
            ring.height = t.height + 4;
        }
    }
}
