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

        Css.CssRect {
            id: root
            anchors.fill: parent
            // Re-anchor the CSS ancestor walk at the page wrapper: the dialog lives in a separate
            // window, so without this scoped rules (`.native .dialog …`) and inheritance don't reach it.
            property Item cssAncestor: wrap
            cssPrimitive: "dialog"
        }
    }
    // Controlled open state: survives the imperative visible=false a window self-close performs.
    Binding {
        target: dlg
        property: "visible"
        value: wrap.open
        restoreMode: Binding.RestoreNone
    }
}
