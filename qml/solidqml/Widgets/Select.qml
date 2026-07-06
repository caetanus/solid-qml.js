// Select — the <select>/<option> component in solidqml.Widgets (owner directive 2026-07-05: one .qml
// per component). A T.ComboBox with CSS support: wrapper CssFill (cssPrimitive "select") + the
// control (background null; the wrapper owns the box painting), a `.value` CssText contentItem, a
// `.chevron` glyph, `.option`/`.option-label` delegate slots, and the dropdown popup.
//
// Popup: popupType Popup.Item (an in-scene overlay popup — Wayland compositors don't honour client
// toplevel positioning, so a window popup lands wherever the compositor drops it once the app window
// floats). It flips ABOVE the control when opening below would overflow the WINDOW. The popup contents
// are reparented to the window Overlay, severing the `.wg-select .popup` visual chain — cssAncestor
// re-anchors the engine's ancestor walk at the control on BOTH sibling slots (background + contentItem).
//
// The static option labels/values are set by the emit: `model` (labels, two-way alias) + `values`
// (the parallel values array). `currentIndex` is a two-way alias so the controlled RestoreNone Binding
// writes through; user picks relay via the `activated(index)` signal.
//
// The transpiler emits:  W.Select { id: __inputN; cssClass: […]; model: […]; values: […];
//                                   [enabled:false]; onActivated; controlled Binding on currentIndex }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property alias model: ctl.model
    property alias currentIndex: ctl.currentIndex
    // Parallel values array alongside the display-label model (used by onActivated + the Binding).
    property var values: []
    signal activated(int index)

    cssPrimitive: "select"
    cssState: (ctl.activeFocus ? ["focus"] : []).concat(!ctl.enabled ? ["disabled"] : [])
    implicitWidth: ctl.implicitWidth
    implicitHeight: ctl.implicitHeight

    T.ComboBox {
        id: ctl
        anchors.fill: parent
        activeFocusOnTab: solidTabstop.enabled
        onActivated: (index) => root.activated(index)
        // Qt::Popup semantics (owner directive): the dropdown vanishes when the app window loses
        // focus — a native popup does not linger over other applications.
        Window.onActiveChanged: if (!Window.active) ctl.popup.close()
        // No visual chrome from Templates; the CssFill wrapper owns the box painting.
        background: null
        // leftPadding keeps the contentItem text clear of the border.
        leftPadding: 12
        // contentItem: CssText showing the selected item's display label.
        contentItem: Css.CssText {
            cssPrimitive: ""
            cssClass: ["value"]
            text: ctl.displayText
        }
        // Chevron: absolutely positioned at the right-centre of the ComboBox.
        Css.CssText {
            cssPrimitive: ""
            cssClass: ["chevron"]
            text: "▾"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
        }
        // Delegate: one T.ItemDelegate per model row.
        delegate: T.ItemDelegate {
            id: optDel
            // The style must bind highlighted itself (Basic does the same) — without it keyboard
            // navigation moves highlightedIndex invisibly and the active row never changes.
            highlighted: ctl.highlightedIndex === index
            // Width must be explicit: ComboBox does not size delegates automatically.
            width: ctl.popup.width
            implicitHeight: 36
            background: Css.CssFill {
                cssPrimitive: "div"
                cssClass: ["option"]
                cssState: (optDel.highlighted ? ["hover"] : []).concat(ctl.currentIndex === index ? ["selected"] : [])
            }
            contentItem: Css.CssText {
                cssPrimitive: ""
                cssClass: ["option-label"]
                text: modelData
            }
        }
        // Popup: T.Popup below the control; padding ≥ border-width prevents clip. Templates popups
        // have NO implicit-size policy of their own (that's the style's job, and we ARE the style) —
        // without the implicitHeight line the popup opens 0px tall.
        popup: T.Popup {
            popupType: T.Popup.Item
            y: (ctl.mapToItem(null, 0, ctl.height + 2).y + height > (ctl.Window.height || Screen.height)) ? -(height + 2) : ctl.height + 2
            width: ctl.width
            implicitHeight: contentHeight + topPadding + bottomPadding
            padding: 1
            // cssAncestor: re-anchor the engine's ancestor walk at the control (overlay reparenting
            // severs the `.wg-select .popup` chain). background and contentItem are SIBLING slots;
            // every popup descendant's walk passes through one of them.
            background: Css.CssFill {
                property Item cssAncestor: ctl
                cssPrimitive: "div"
                cssClass: ["popup"]
            }
            contentItem: ListView {
                property Item cssAncestor: ctl
                clip: true
                model: ctl.delegateModel
                currentIndex: ctl.highlightedIndex
                implicitHeight: Math.min(contentHeight, 240)
            }
        }
    }
}
