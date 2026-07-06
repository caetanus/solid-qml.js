// TreeView — the <TreeView> component in module solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). OUR OWN recursive tree over plain { label, children? } objects — Qt's TreeView
// needs a QAbstractItemModel and brings delegate recycling; virtualization is a LATER phase (plan:
// 2026-07-03-native-only-widgets, Phase 4). This targets sidebar/config-panel-sized trees.
//
// Every node renders a full-width `.tree-row` (indentation is depth*16 INSIDE the row, so
// hover/selection paint edge-to-edge like every desktop tree); children stack under it via a Repeater
// when expanded. Recursion: a `Component { id: nodeComp }` whose delegate references itself BY ID —
// the only legal self-recursion in a single QML file (a self-referencing inline component is a
// compile error: "Inline components form a cycle!"). Depth travels through the model rows
// ({ __n: node, __d: depth }) because delegate contexts resolve at the Component's DECLARATION site.
//
// The transpiler emits:  W.TreeView { cssClass: […]; treeData: <expr>; onSelected: (node) => … }
import QtQuick
import qmlcss 1.0 as Css

Css.CssFill {
    id: root
    // `data` is Item's default property — the node array is a transpiler-wired `__treeData` (the `__`
    // avoids colliding with an author identifier of the same name in `__treeData: <expr>`, which would
    // self-reference). A click fires `selected(node)`.
    property var __treeData: []
    signal selected(var node)

    cssPrimitive: "div"
    // Best-effort defaults for a bare (CSS-less) tree. With box rules on the wrapper, the engine's
    // content pass measures the plain anchored host as 0 and OVERWRITES these — author CSS must size
    // the pane (e.g. `.nv-tree { width; height }`), which is also the desktop-correct shape: trees
    // live in fixed panes, they don't grow the page.
    implicitWidth: 240
    implicitHeight: rootCol.height

    // Anchored plain-Item host: insulates the tree internals from the wrapper's CSS layout pass
    // (same pattern as <Calendar> / the date input — anchored plain items are skipped).
    Item {
        anchors.fill: parent
        Component {
            id: nodeComp
            Column {
                id: nodeItem
                width: parent ? parent.width : 0
                property var node: modelData.__n
                property int depth: modelData.__d
                property bool expanded: true
                Css.CssFill {
                    cssPrimitive: "div"
                    cssClass: ["tree-row"]
                    cssState: rowMa.containsMouse ? ["hover"] : []
                    width: nodeItem.width
                    height: 28
                    implicitHeight: 28
                    // Anchored host for the row internals — plain items inside a Css container MUST be
                    // hosted this way (see the checkbox indicator comment in qml.ts) or a CSS box rule
                    // on .tree-row triggers a flex pass that stretches them.
                    Item {
                        anchors.fill: parent
                        Text {
                            x: 8 + nodeItem.depth * 16
                            anchors.verticalCenter: parent.verticalCenter
                            text: (nodeItem.node && nodeItem.node.children && nodeItem.node.children.length) ? (nodeItem.expanded ? "▾" : "▸") : ""
                            // CssItem injection styles the plain disclosure glyph (colour/font) without joining a layout.
                            Css.CssItem { cssPrimitive: "text"; cssClass: ["tree-disclosure"] }
                        }
                        Css.CssText {
                            cssPrimitive: ""
                            cssClass: ["tree-label"]
                            x: 8 + nodeItem.depth * 16 + 18
                            anchors.verticalCenter: parent.verticalCenter
                            text: nodeItem.node ? ("" + nodeItem.node.label) : ""
                        }
                        MouseArea {
                            id: rowMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { nodeItem.expanded = !nodeItem.expanded; root.selected(nodeItem.node) }
                        }
                    }
                }
                Repeater {
                    model: (nodeItem.expanded && nodeItem.node && nodeItem.node.children) ? nodeItem.node.children.map(function(c) { return ({ __n: c, __d: nodeItem.depth + 1 }) }) : []
                    delegate: nodeComp
                }
            }
        }
        Column {
            id: rootCol
            width: parent.width
            Repeater {
                model: ((root.__treeData) || []).map(function(c) { return ({ __n: c, __d: 0 }) })
                delegate: nodeComp
            }
        }
    }
}
