// DateField — the <input type="date"> component in solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). A readOnly T.TextField (formatted via Qt.formatDate) with a `.chevron` glyph
// and a T.Popup hosting the shared MonthGrid. The wrapper CssFill (cssPrimitive "input") carries CSS
// identity, `:focus` while the popup is open, `:disabled` when the field is disabled.
//
// Popup: popupType Popup.Item, flip-above on WINDOW overflow, cssAncestor re-anchor on both sibling
// slots (overlay reparenting severs the `.wg-date .popup`/`.wg-date .day` chain), and the __closedAt
// toggle-guard (CloseOnPressOutside fires on the PRESS, so a naive visible-check would reopen).
//
// Keyboard: Enter/Space/Down open the popup; with it open, arrows move the day cursor (±1 left/right,
// ±7 up/down) — writing the grid's viewMonth/viewYear imperatively so nav and keyboard share state —
// and Enter/Space commit it. Both a cell click and the keyboard commit route through the `dayPicked`
// signal (→ the emit's onChange) then close.
//
// The transpiler emits:  W.DateField { cssClass: […]; [value: <expr>]; [enabled:false];
//                                      [onDayPicked: (date) => {…}] }
import QtQuick
import QtQuick.Templates as T
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    property var value: null
    // Keyboard cursor: arrows move it by ±1/±7 days; Enter commits it through the same dayPicked path
    // a cell click takes.
    property var cursor: null
    signal dayPicked(var date)

    cssPrimitive: "input"
    cssState: (pop.visible ? ["focus"] : []).concat(!field.enabled ? ["disabled"] : [])
    implicitWidth: 200
    implicitHeight: 36

    // Qt::Popup semantics: vanish when the app window loses focus.
    Window.onActiveChanged: if (!Window.active) pop.close()

    function __step(days) {
        var b = cursor instanceof Date ? cursor : (value instanceof Date ? value : new Date())
        var d = new Date(b.getFullYear(), b.getMonth(), b.getDate() + days)
        cursor = d
        grid.viewMonth = d.getMonth()
        grid.viewYear = d.getFullYear()
    }
    function __commit() {
        if (!(cursor instanceof Date)) return
        root.dayPicked(cursor)
        pop.close()
    }

    // Anchored plain-Item host: the wrapper is a Css container, and once the author's CSS gives it box
    // rules the layout engine runs a flex pass over ALL contentHolder children — plain ones included —
    // stretching the chevron Text. An anchors.fill Item is skipped by the layout; anchors hold inside.
    Item {
        anchors.fill: parent
        // ReadOnly text field — display only; typing dates is out of scope.
        T.TextField {
            id: field
            anchors.fill: parent
            background: null
            readOnly: true
            activeFocusOnTab: solidTabstop.enabled
            // Enter/Space/Down open the popup; with it open, arrows move the cursor and Enter/Space
            // commit it. The popup keeps focus on this field (focus: false default), so keys land here.
            Keys.onReturnPressed: pop.visible ? root.__commit() : pop.open()
            Keys.onSpacePressed: pop.visible ? root.__commit() : pop.open()
            Keys.onDownPressed: pop.visible ? root.__step(7) : pop.open()
            Keys.onUpPressed: { if (pop.visible) root.__step(-7) }
            Keys.onLeftPressed: { if (pop.visible) root.__step(-1) }
            Keys.onRightPressed: { if (pop.visible) root.__step(1) }
            color: cssTheme.parseColor(root.inheritedColor || "#2b2b2b")
            font.family: cssTheme.resolveFontFamily(root.inheritedFontFamily || "Sans Serif")
            font.pixelSize: cssTheme.parseFontSize(root.inheritedFontSize || "13px", 13)
            leftPadding: 12
            rightPadding: 36
            verticalAlignment: TextInput.AlignVCenter
        }
        // Binding: keep the displayed text in sync with the value, formatted as ISO date.
        // Guard: Qt.formatDate throws on null/undefined (no Date object yet).
        Binding {
            target: field
            property: "text"
            value: root.value instanceof Date ? Qt.formatDate(root.value, "yyyy-MM-dd") : ""
            restoreMode: Binding.RestoreNone
        }
        // Calendar glyph — IDENTICAL to the <select> chevron (same class, same CSS box) so the two
        // dropdowns align. Living inside the anchored host keeps it out of the wrapper's layout pass.
        Css.CssText {
            cssPrimitive: ""
            cssClass: ["chevron"]
            text: "▾"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
        }
        // MouseArea over the whole field: click toggles the popup. CloseOnPressOutside fires on the
        // PRESS, so by the time the click lands here the popup already closed — a naive visible-check
        // reopens it. The popup stamps its close time; a click right after a close is a toggle-close.
        MouseArea {
            anchors.fill: parent
            onClicked: { field.forceActiveFocus(); if (pop.visible) pop.close(); else if (Date.now() - pop.__closedAt > 150) pop.open() }
        }
    }
    // Popup: T.Popup renders on the window overlay; padding ≥ 1 prevents CssFill border clip.
    T.Popup {
        id: pop
        property double __closedAt: 0
        onOpened: root.cursor = root.value instanceof Date ? root.value : new Date()
        onClosed: { __closedAt = Date.now(); root.cursor = null }
        // In-scene overlay popup, flip on WINDOW overflow (Popup.Window mis-positions on Wayland once
        // the app window floats).
        popupType: T.Popup.Item
        y: (root.mapToItem(null, 0, root.height + 2).y + height > (root.Window.height || Screen.height)) ? -(height + 2) : root.height + 2
        // Templates popups have no implicit-size policy (style's job — ours): without these two lines
        // the calendar dropdown opens 0x0.
        implicitWidth: contentWidth + leftPadding + rightPadding
        implicitHeight: contentHeight + topPadding + bottomPadding
        padding: 1
        // cssAncestor: overlay reparenting severs the visual chain — re-anchor at the date wrapper so
        // `.wg-date .popup` / `.wg-date .day` keep matching (background and contentItem are SIBLINGS).
        background: Css.CssFill {
            property Item cssAncestor: root
            cssPrimitive: "div"
            cssClass: ["popup"]
        }
        contentItem: MonthGrid {
            id: grid
            property Item cssAncestor: root
            selectedDate: root.value
            cursorDate: root.cursor
            onDayPicked: (date) => { root.dayPicked(date); pop.close() }
        }
    }
}
