// TreeView — the <TreeView> component in module solidqml.Widgets (owner directive 2026-07-05: one
// .qml per component). OUR OWN tree over plain { label, children? } objects — Qt's TreeView needs a
// QAbstractItemModel and brings delegate recycling; virtualization is a LATER phase (plan:
// 2026-07-03-native-only-widgets, Phase 4). This targets sidebar/config-panel-sized trees.
//
// The tree renders FLAT: expansion state lives on the root (`_collapsed`, keyed by row path "0/2"),
// and `_visibleRows` flattens the node tree to its visible rows in paint order. That linear order is
// what keyboard navigation walks (QtWidgets model: the whole tree is ONE tab stop; Up/Down move the
// selection, Right expands/descends, Left collapses/ascends, Enter re-commits). Every row is a
// full-width `.tree-row` (indentation is depth*16 INSIDE the row, so hover/selection paint
// edge-to-edge like every desktop tree).
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
    property int currentIndex: -1
    signal selected(var node)

    // Expansion state: collapsed row paths ("0", "0/2", …) in a plain object. Mutating a var
    // property's innards doesn't notify bindings — `_rev` is bumped on every toggle so `_visibleRows`
    // re-evaluates. Default is fully expanded (only COLLAPSED paths are recorded).
    property var _collapsed: ({})
    property int _rev: 0

    // The tree flattened to its VISIBLE rows in paint order: { __n node, __d depth, __p path,
    // __k has-children, __e expanded }. Keyboard navigation and the Repeater both walk this.
    readonly property var _visibleRows: {
        var rev = root._rev; // dependency: re-run on toggle
        var out = [];
        function walk(nodes, depth, prefix) {
            for (var i = 0; i < nodes.length; i++) {
                var n = nodes[i];
                var path = prefix + i;
                var kids = !!(n && n.children && n.children.length);
                var exp = kids && !root._collapsed[path];
                out.push({ __n: n, __d: depth, __p: path, __k: kids, __e: exp });
                if (exp) walk(n.children, depth + 1, path + "/");
            }
        }
        walk(root.__treeData || [], 0, "");
        return out;
    }

    function _toggle(path) {
        if (_collapsed[path]) delete _collapsed[path];
        else _collapsed[path] = true;
        _rev++;
    }

    // Fire selected() for the current row (arrow-key navigation, Enter, and clicks).
    function _emitCurrent() {
        if (currentIndex >= 0 && currentIndex < _visibleRows.length)
            root.selected(_visibleRows[currentIndex].__n);
    }

    function _move(step) {
        if (!_visibleRows.length) return;
        var i = currentIndex < 0 ? 0 : Math.min(Math.max(currentIndex + step, 0), _visibleRows.length - 1);
        if (i !== currentIndex) { currentIndex = i; _emitCurrent(); }
    }

    // Right: expand a collapsed branch, else step INTO it (its first child is the next visible row).
    function _expandOrDescend() {
        var r = _visibleRows[currentIndex];
        if (!r || !r.__k) return;
        if (!r.__e) _toggle(r.__p);
        else _move(1);
    }

    // Space: toggle the current branch open/closed (no-op on a leaf).
    function _toggleCurrent() {
        var r = _visibleRows[currentIndex];
        if (r && r.__k) _toggle(r.__p);
    }

    // Left: collapse an expanded branch, else jump to the parent row (path minus its last segment).
    function _collapseOrAscend() {
        var r = _visibleRows[currentIndex];
        if (!r) return;
        if (r.__k && r.__e) { _toggle(r.__p); return; }
        var slash = r.__p.lastIndexOf("/");
        if (slash < 0) return;
        var parentPath = r.__p.slice(0, slash);
        for (var i = 0; i < _visibleRows.length; i++)
            if (_visibleRows[i].__p === parentPath) { currentIndex = i; _emitCurrent(); return; }
    }

    cssPrimitive: "div"
    // Best-effort defaults for a bare (CSS-less) tree. With box rules on the wrapper, the engine's
    // content pass measures the plain anchored host as 0 and OVERWRITES these — author CSS must size
    // the pane (e.g. `.nv-tree { width; height }`), which is also the desktop-correct shape: trees
    // live in fixed panes, they don't grow the page.
    implicitWidth: 240
    implicitHeight: rootCol.height

    // Anchored plain-Item host: insulates the tree internals from the wrapper's CSS layout pass
    // (same pattern as <Calendar> / the date input — anchored plain items are skipped). It is also
    // the tree's single tab stop; the Tabstop ring frames it on focus.
    Item {
        id: kb
        anchors.fill: parent
        activeFocusOnTab: true
        Keys.onUpPressed: root._move(-1)
        Keys.onDownPressed: root._move(1)
        Keys.onRightPressed: root._expandOrDescend()
        Keys.onLeftPressed: root._collapseOrAscend()
        // Enter mirrors a click: toggles the branch AND re-commits; Space only toggles.
        Keys.onReturnPressed: { root._toggleCurrent(); root._emitCurrent(); }
        Keys.onEnterPressed: { root._toggleCurrent(); root._emitCurrent(); }
        Keys.onSpacePressed: root._toggleCurrent()

        Column {
            id: rootCol
            width: parent.width
            Repeater {
                model: root._visibleRows
                delegate: Css.CssFill {
                    id: rowItem
                    property int rowIndex: index
                    cssPrimitive: "div"
                    cssClass: ["tree-row"]
                    cssState: (rowMa.containsMouse ? ["hover"] : []).concat(index === root.currentIndex ? ["selected"] : [])
                    width: rootCol.width
                    height: 28
                    implicitHeight: 28
                    // Anchored host for the row internals — plain items inside a Css container MUST be
                    // hosted this way (see the checkbox indicator comment in qml.ts) or a CSS box rule
                    // on .tree-row triggers a flex pass that stretches them.
                    Item {
                        anchors.fill: parent
                        Text {
                            x: 8 + modelData.__d * 16
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.__k ? (modelData.__e ? "▾" : "▸") : ""
                            // CssItem injection styles the plain disclosure glyph (colour/font) without joining a layout.
                            Css.CssItem { cssPrimitive: "text"; cssClass: ["tree-disclosure"] }
                        }
                        Css.CssText {
                            cssPrimitive: ""
                            cssClass: ["tree-label"]
                            x: 8 + modelData.__d * 16 + 18
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.__n ? ("" + modelData.__n.label) : ""
                        }
                        MouseArea {
                            id: rowMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                kb.forceActiveFocus();
                                // Capture the node FIRST — _toggle rebuilds _visibleRows and this
                                // delegate's modelData context with it.
                                var node = modelData.__n;
                                root.currentIndex = rowItem.rowIndex;
                                if (modelData.__k) root._toggle(modelData.__p);
                                root.selected(node);
                            }
                        }
                    }
                }
            }
        }
    }
}
