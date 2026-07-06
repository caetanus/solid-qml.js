// Dialog — the <dialog> component in solidqml.Widgets (owner directive 2026-07-05: one .qml per
// component). A REAL modal Window (flags Qt.Dialog, modality Qt.WindowModal), NOT a Templates dialog
// or overlay popup: a zero-size page-anchor Item provides the transientParent (its Window.window), and
// the author's content is rooted for CSS scoping. The window sizes itself to the Css root's implicit
// (content-driven) size.
//
// A separate window severs the CSS ancestor chain (scoped rules like `.native .dialog …` and
// inheritance would stop flowing), so the Css root carries `cssAncestor: wrap` to re-anchor the
// engine's ancestor walk back at the page anchor (which still sits under `.native`).
//
// `open` drives the window's visible via a plain binding AND a RestoreNone Binding (the latter
// survives a self-close — the window chrome's close button / Esc, which imperatively sets
// visible=false); `onClosing` fires `dialogClosed` so the author's onClose keeps the signal honest.
// The author's class forwards to the Css root via `cssClass`; children mount into it via `content`.
//
// The transpiler emits:  W.Dialog { open: <bool>; [title]; cssClass: […]; [onDialogClosed: {…}];
//                                   <children> }
import QtQuick
import QtQuick.Window
import qmlcss 1.0 as Css

Item {
    id: wrap
    property bool open: false
    property string title: ""
    // Author class forwards to the Css root; children mount into the root's data (the layout box).
    property alias cssClass: root.cssClass
    default property alias content: root.data
    signal dialogClosed()

    // Find the default button (<button type="submit"> → isDefault) anywhere in the dialog content,
    // so Enter fires it (study §6). Recursive over visual children; non-buttons lack `isDefault`.
    function __findDefault(item) {
        if (!item || !item.children)
            return null;
        for (var k = 0; k < item.children.length; k++) {
            var c = item.children[k];
            if (c && c.isDefault === true)
                return c;
            var f = __findDefault(c);
            if (f)
                return f;
        }
        return null;
    }

    width: 0
    height: 0

    Window {
        id: dlg
        flags: Qt.Dialog
        modality: Qt.WindowModal
        transientParent: wrap.Window.window
        title: wrap.title
        visible: wrap.open
        // Size the window to its content (the Css root's implicit size). Not circular: the root's
        // implicit is content-driven, its actual size comes back via anchors.fill.
        width: Math.max(1, root.implicitWidth)
        height: Math.max(1, root.implicitHeight)
        onClosing: wrap.dialogClosed()

        // Tab-stop is born by default in the dialog too (study §6, owner call): a modal opens with
        // keyboard focus already placed, so Tab works and the ring shows without a click. Prefer the
        // default button (<button type="submit">) so Enter confirms it (desktop autoDefault); else the
        // first focusable. A separate window has its own focus chain, independent of the app root.
        onVisibleChanged: if (visible && solidTabstop.enabled) Qt.callLater(function() {
            var def = wrap.__findDefault(root);
            if (def && def.takeFocus) { def.takeFocus(); return; }
            var f = dlg.contentItem.nextItemInFocusChain(true);
            if (f) f.forceActiveFocus(Qt.TabFocusReason);
        })

        // Esc cancels the dialog (desktop-essential for keyboard-only use, study §6). A QtQuick
        // Window does not close on Escape by default, so drive it explicitly: fire dialogClosed →
        // the author's onClose sets `open` false → the Binding hides the window.
        Shortcut {
            sequences: ["Escape"]
            enabled: dlg.visible
            onActivated: wrap.dialogClosed()
        }

        Css.CssRect {
            id: root
            anchors.fill: parent
            // Re-anchor the CSS ancestor walk at the page wrapper: the dialog lives in a separate
            // window, so without this scoped rules (`.native .dialog …`) and inheritance don't reach it.
            property Item cssAncestor: wrap
            cssPrimitive: "dialog"

            // Enter fires the default button — but only when it BUBBLES up to here, i.e. the focused
            // item didn't consume it. A focused button handles Enter itself (activating THAT button)
            // and accepts the event, so it never reaches this handler — no double-fire, and Enter on
            // a non-default button still activates just that button, matching the desktop model (§6).
            Keys.onReturnPressed: (event) => { var b = wrap.__findDefault(root); if (b) { b.clicked(); event.accepted = true; } }
            Keys.onEnterPressed: (event) => { var b = wrap.__findDefault(root); if (b) { b.clicked(); event.accepted = true; } }
        }

        // The keyboard tab-focus ring, scoped to THIS window's focus chain (the dialog is a
        // separate Window, so the app-root Tabstop can't see items in here). Same `::tab-stop`
        // styling; a sibling of root so it overlays on top.
        Tabstop { window: dlg }
    }
    // Controlled open state: survives the imperative visible=false a window self-close performs.
    Binding {
        target: dlg
        property: "visible"
        value: wrap.open
        restoreMode: Binding.RestoreNone
    }
}
