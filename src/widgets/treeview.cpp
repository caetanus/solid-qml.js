#include "treeview.h"

#include "snippetwidget.h"

namespace {

// Original TreeView.qml internals — `root` = the C++ wrapper; the snippet host (kb) is the
// tree's single tab stop and owns the expansion state + flattening.
const char *kTreeViewBody = R"(import QtQuick
import qmlcss 1.0 as Css

Item {
    id: kb
    anchors.fill: parent
    activeFocusOnTab: true
    // The CSS box is the wrapper; the tab stop is this inner control — mirror its focus
    // into the root so `:focus` (and the focus ring) applies to the visible box.
    onActiveFocusChanged: root.cssState = activeFocus ? ["focus"] : []

    // Expansion state: collapsed row paths ("0", "0/2", …) in a plain object. Mutating a var
    // property's innards doesn't notify bindings — `_rev` is bumped on every toggle so
    // `_visibleRows` re-evaluates. Default is fully expanded (only COLLAPSED paths are recorded).
    property var _collapsed: ({})
    property int _rev: 0

    // The tree flattened to its VISIBLE rows in paint order: { __n node, __d depth, __p path,
    // __k has-children, __e expanded }. Keyboard navigation and the Repeater both walk this.
    readonly property var _visibleRows: {
        var rev = kb._rev; // dependency: re-run on toggle
        var out = [];
        function walk(nodes, depth, prefix) {
            for (var i = 0; i < nodes.length; i++) {
                var n = nodes[i];
                var path = prefix + i;
                var kids = !!(n && n.children && n.children.length);
                var exp = kids && !kb._collapsed[path];
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
        if (root.currentIndex >= 0 && root.currentIndex < _visibleRows.length)
            root.selected(_visibleRows[root.currentIndex].__n);
    }

    function _move(step) {
        if (!_visibleRows.length) return;
        var i = root.currentIndex < 0 ? 0 : Math.min(Math.max(root.currentIndex + step, 0), _visibleRows.length - 1);
        if (i !== root.currentIndex) { root.currentIndex = i; _emitCurrent(); }
    }

    // Right: expand a collapsed branch, else step INTO it (its first child is the next visible row).
    function _expandOrDescend() {
        var r = _visibleRows[root.currentIndex];
        if (!r || !r.__k) return;
        if (!r.__e) _toggle(r.__p);
        else _move(1);
    }

    // Space: toggle the current branch open/closed (no-op on a leaf).
    function _toggleCurrent() {
        var r = _visibleRows[root.currentIndex];
        if (r && r.__k) _toggle(r.__p);
    }

    // Left: collapse an expanded branch, else jump to the parent row (path minus its last segment).
    function _collapseOrAscend() {
        var r = _visibleRows[root.currentIndex];
        if (!r) return;
        if (r.__k && r.__e) { _toggle(r.__p); return; }
        var slash = r.__p.lastIndexOf("/");
        if (slash < 0) return;
        var parentPath = r.__p.slice(0, slash);
        for (var i = 0; i < _visibleRows.length; i++)
            if (_visibleRows[i].__p === parentPath) { root.currentIndex = i; _emitCurrent(); return; }
    }

    Keys.onUpPressed: kb._move(-1)
    Keys.onDownPressed: kb._move(1)
    Keys.onRightPressed: kb._expandOrDescend()
    Keys.onLeftPressed: kb._collapseOrAscend()
    // Enter mirrors a click: toggles the branch AND re-commits; Space only toggles.
    Keys.onReturnPressed: { kb._toggleCurrent(); kb._emitCurrent(); }
    Keys.onEnterPressed: { kb._toggleCurrent(); kb._emitCurrent(); }
    Keys.onSpacePressed: kb._toggleCurrent()

    Column {
        id: rootCol
        width: parent.width
        // Best-effort implicit for a bare (CSS-less) tree — the engine's content pass measures the
        // plain anchored host as 0 and overwrites it; author CSS must size the pane (the QML
        // original's implicitHeight binding met the same fate).
        onHeightChanged: root.implicitHeight = height
        Repeater {
            model: kb._visibleRows
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
                            if (modelData.__k) kb._toggle(modelData.__p);
                            root.selected(node);
                        }
                    }
                }
            }
        }
    }
}
)";

} // namespace

namespace SolidWidgets {

TreeView::TreeView(QQuickItem *parent)
    : QmlCss::CssFill(parent)
{
    setCssPrimitive(QStringLiteral("div"));
    setImplicitWidth(240);
}

void TreeView::setTreeData(const QVariant &v)
{
    if (m_treeData == v)
        return;
    m_treeData = v;
    emit treeDataChanged();
}

void TreeView::setCurrentIndex(int v)
{
    if (m_currentIndex == v)
        return;
    m_currentIndex = v;
    emit currentIndexChanged();
}

void TreeView::componentComplete()
{
    QmlCss::CssFill::componentComplete();
    composeInternal(this, content(), QStringLiteral("solidwidgets-treeview"), kTreeViewBody);
}

} // namespace SolidWidgets
