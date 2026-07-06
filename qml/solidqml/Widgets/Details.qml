// Details — the <details>/<summary> disclosure component in module solidqml.Widgets (owner directive
// 2026-07-05: one .qml per component; the transpiler INSTANTIATES these, it no longer hand-emits the
// expander). Wrapper CssFill (cssPrimitive "details") owns `__open`, initialized from the `open`
// prop (the init binding breaks on the first user toggle — standard QML semantics, same as the
// calendar's nav month). cssState carries "open" for author CSS.
//
// The summary header row lives inline here: a "▸" marker glyph and a MouseArea that toggles __open
// (doubling as the hover tracker, like the Button component). The transpiler passes the summary's
// OWN children via `summaryContent` (appended after the marker/MouseArea) and the disclosure body
// via the default `content`. Only the CssText summary kids flex-participate; the marker (in an
// anchored Item host) and the MouseArea (anchored) are insulated from the layout pass.
//
// The transpiler emits:
//   W.Details { cssClass: […]; open: <expr>; summaryClass: […]; summaryContent: [ … ]; <body …> }
import QtQuick
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    // `open` seeds the initial disclosure state; the init binding on __open breaks on first click.
    property bool open: false
    // Author classes for the summary header row (so `summary { … }` / `summary:hover` apply).
    property var summaryClass: []
    // Summary's own children (the label). Aliased into the summary box's data, appended after the
    // inline marker + MouseArea.
    property alias summaryContent: summaryBox.data
    // Disclosure body: use-site children route here via the default property.
    default property alias content: contentBox.data

    cssPrimitive: "details"
    cssState: root.__open ? ["open"] : []
    // Disclosure state — init binding from `open` breaks on the first click (QML semantics).
    property bool __open: !!(root.open)

    // ── summary header row ────────────────────────────────────────────────
    Css.CssFill {
        id: summaryBox
        cssClass: root.summaryClass
        cssPrimitive: "summary"
        cssState: (root.__open ? ["open"] : []).concat(__ma.containsMouse ? ["hover"] : [])
        // Marker glyph: plain Text inside an anchors.fill Item host (flex-pass insulation — once the
        // author gives the summary box rules the engine's flex pass hits plain children too);
        // rotates 90° while open. `.marker` CSS lands via the nested CssItem.
        Item {
            anchors.fill: parent
            Text {
                text: "▸"
                rotation: root.__open ? 90 : 0
                anchors.left: parent.left
                anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                Css.CssItem { cssPrimitive: "text"; cssClass: ["marker"] }
            }
        }
        MouseArea {
            id: __ma
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.__open = !root.__open
        }
    }
    // ── disclosure content: only while open (invisible items leave the layout) ──
    Css.CssRect {
        id: contentBox
        cssPrimitive: "div"
        cssClass: ["content"]
        visible: root.__open
    }
}
