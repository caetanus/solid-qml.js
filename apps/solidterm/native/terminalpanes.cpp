#include "terminalpanes.h"

#include "paneheader.h"
#include "terminalview.h"

#include <QCursor>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QTimer>
#include <functional>

// A thin draggable divider: a coloured rect that drags along one axis and reports pixel deltas.
// (Global scope — the header forward-declares it for Node::dividers.)
class DividerItem : public QQuickItem {
public:
    DividerItem(bool horizontal, QQuickItem *parent)
        : QQuickItem(parent)
        , m_horizontal(horizontal)
    {
        setAcceptedMouseButtons(Qt::LeftButton);
        setFlag(ItemHasContents, true);
        setCursor(horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
    }

    QColor color;
    std::function<void(qreal)> onDrag;

    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        auto *node = static_cast<QSGSimpleRectNode *>(old);
        if (!node)
            node = new QSGSimpleRectNode;
        node->setColor(color);
        node->setRect(boundingRect());
        return node;
    }

protected:
    void mousePressEvent(QMouseEvent *e) override { m_last = e->scenePosition(); e->accept(); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPointF now = e->scenePosition();
        const qreal d = m_horizontal ? (now.x() - m_last.x()) : (now.y() - m_last.y());
        m_last = now;
        if (onDrag)
            onDrag(d);
        e->accept();
    }

private:
    bool m_horizontal;
    QPointF m_last;
};

namespace {

constexpr qreal kHeaderH = 24.0;
constexpr qreal kDivider = 6.0;
constexpr int kAnimMs = 190;

// The pane cell: a CSS-engine box wrapping the header + terminal, so opening/closing a split is a
// CSS animation (owner: "csszar e descsszar as coisas on the fly" is the whole point). It's
// QML-composed (not `new`'d) so the engine's componentComplete resolves its class → the
// @keyframes in term.css play. TerminalPanes positions the cell; the cell splits itself into
// header + view internally.
const char *kCellQml = R"(import QtQuick
import qmlcss 1.0 as Css
import SolidTerm 1.0

Css.CssRect {
    id: cell
    property alias header: __hdr
    property alias viewHost: __content
    property int headerH: 24
    cssPrimitive: "div"
    cssClass: ["pane", "pane-enter"]
    // A plain Item holds the header + a content area (anchored, so the engine's content pass never
    // moves them). The TerminalView is created SEPARATELY and parented into `content`, so a pane can
    // be MOVED (drag/detach) by reparenting its live view into another cell — the pty rides along.
    Item {
        anchors.fill: parent
        PaneHeader { id: __hdr; x: 0; y: 0; width: parent.width; height: cell.headerH }
        Item { id: __content; x: 0; y: cell.headerH; width: parent.width; height: Math.max(0, parent.height - cell.headerH) }
    }
}
)";

// The pane's terminal — created on its own so it can be reparented between cells/windows.
const char *kViewQml = "import SolidTerm 1.0\nTerminalView { anchors.fill: parent }";

// The drag drop-zone highlight — a translucent accent rectangle we move over the hovered zone while
// dragging a pane header (tilix shows this blue overlay on the half of the pane you'd split into).
const char *kDropQml =
    "import QtQuick\n"
    "Rectangle { visible: false; z: 99999; color: \"#403584e4\"; border.color: \"#3584e4\"; border.width: 2; radius: 4 }";

} // namespace

TerminalPanes::TerminalPanes(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
}

TerminalPanes::~TerminalPanes()
{
    deleteSubtree(m_root);
}

// ─── tree construction ──────────────────────────────────────────────────────────────────────────

TerminalPanes::Node *TerminalPanes::makeLeaf(TerminalView *adopt)
{
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return nullptr;
    if (!m_cellComponent) {
        m_cellComponent = new QQmlComponent(eng, this);
        m_cellComponent->setData(QByteArray(kCellQml), QUrl(QStringLiteral("qrc:/solidterm/PaneCell.qml")));
        m_viewComponent = new QQmlComponent(eng, this);
        m_viewComponent->setData(QByteArray(kViewQml), QUrl(QStringLiteral("qrc:/solidterm/PaneView.qml")));
    }
    auto *cell = qobject_cast<QQuickItem *>(m_cellComponent->create(qmlContext(this)));
    if (!cell) {
        qWarning("solidterm: pane cell failed: %s", qPrintable(m_cellComponent->errorString()));
        return nullptr;
    }
    cell->setParent(this);
    cell->setParentItem(this);
    cell->setProperty("headerH", kHeaderH);
    auto *content = cell->property("viewHost").value<QQuickItem *>();

    // Adopt an existing view (a moved pane — pty rides along) or create a fresh one.
    TerminalView *view = adopt;
    if (!view) {
        view = qobject_cast<TerminalView *>(m_viewComponent->create(qmlContext(this)));
        if (!view) {
            qWarning("solidterm: pane view failed: %s", qPrintable(m_viewComponent->errorString()));
            cell->deleteLater();
            return nullptr;
        }
        view->setParent(this);
    }
    view->setParentItem(content); // anchors.fill re-targets the new content area

    auto *node = new Node;
    node->cell = cell;
    node->view = view;
    node->header = qobject_cast<PaneHeader *>(cell->property("header").value<QQuickItem *>());
    if (node->view && node->header)
        wirePane(node->view, node->header);
    return node;
}

void TerminalPanes::wirePane(TerminalView *v, PaneHeader *header)
{
    applyStyle(v);
    header->setBackground(m_handleColor);
    header->setForeground(m_foreground);
    header->setAccent(QColor("#3584e4"));
    header->setTitle(v->title());

    connect(v, &TerminalView::titleChanged, this, [this, v, header] {
        header->setTitle(v->title());
        if (m_focused && m_focused->view == v)
            emit titleChanged(v->title());
    });
    connect(header, &PaneHeader::clicked, this, [this, v] {
        if (Node *leaf = leafOfView(v)) setFocused(leaf);
    });
    connect(header, &PaneHeader::closeRequested, this, [this, v] {
        if (Node *leaf = leafOfView(v)) removeLeaf(leaf);
    });
    connect(header, &PaneHeader::maximizeRequested, this, [this, v] {
        if (Node *leaf = leafOfView(v)) { setFocused(leaf); toggleZoom(); }
    });
    connect(header, &PaneHeader::menuRequested, this, [this, v](qreal x, qreal y) {
        if (Node *leaf = leafOfView(v)) setFocused(leaf);
        emit paneMenuRequested(x, y, v->readOnly());
    });
    connect(header, &PaneHeader::dragStarted, this, [this, v] { beginPaneDrag(v); });
    connect(header, &PaneHeader::dragMoved, this, [this](qreal gx, qreal gy) { updatePaneDrag(QPointF(gx, gy)); });
    connect(header, &PaneHeader::dragEnded, this, [this](qreal gx, qreal gy) { endPaneDrag(QPointF(gx, gy)); });
    connect(v, &TerminalView::readOnlyChanged, this, [v, header] { header->setReadOnly(v->readOnly()); });
    connect(v, &TerminalView::sessionFinished, this, [this, v] {
        if (Node *leaf = leafOfView(v)) removeLeaf(leaf);
    });
    // A click in the body is explicit user intent to focus this pane → authoritative m_focused update.
    connect(v, &TerminalView::focusRequested, this, [this, v] {
        if (Node *l = leafOfView(v)) setFocused(l);
    });
    // Keyboard-focus churn (reparent, window (de)activation) only repaints — it must NOT move the
    // logical focused pane, or a drag-move would snap the accent back to whatever grabbed focus.
    connect(v, &QQuickItem::activeFocusChanged, this, [this] { refreshHeaderFocus(); });
    connect(v, &TerminalView::searchChanged, this, [this, v](int idx, int count) {
        if (m_focused && m_focused->view == v)
            emit searchChanged(idx, count);
    });
    connect(v, &TerminalView::unsafePasteRequested, this, &TerminalPanes::unsafePasteRequested);
    connect(v, &TerminalView::zoomRequested, this, &TerminalPanes::zoomRequested);
    v->setReservedSequences(m_reserved);
    connect(v, &TerminalView::accelerator, this, &TerminalPanes::accelerator);
}

void TerminalPanes::componentComplete()
{
    QQuickItem::componentComplete();
    m_root = makeLeaf();
    m_focused = m_root;
    relayout();
    // Initial focus is taken on the first non-zero geometry (scene ready) — see geometryChange.
}

// ─── tree walks ─────────────────────────────────────────────────────────────────────────────────

void TerminalPanes::collectLeaves(Node *node, QVector<Node *> &out) const
{
    if (!node)
        return;
    if (node->isLeaf()) {
        out.append(node);
        return;
    }
    for (Node *c : node->children)
        collectLeaves(c, out);
}

int TerminalPanes::count() const
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    return leaves.size();
}

TerminalPanes::Node *TerminalPanes::leafOfView(TerminalView *v) const
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    for (Node *l : leaves)
        if (l->view == v)
            return l;
    return nullptr;
}

void TerminalPanes::deleteSubtree(Node *node)
{
    if (!node)
        return;
    for (Node *c : node->children)
        deleteSubtree(c);
    for (DividerItem *d : node->dividers)
        if (d) d->deleteLater();
    if (node->view)               // the view is a separate object now (not owned by the cell)
        node->view->deleteLater();
    if (node->cell)
        node->cell->deleteLater();
    delete node;
}

// ─── split / close ──────────────────────────────────────────────────────────────────────────────

// Splice `fresh` next to `anchor` along `orient`. If the anchor's parent already splits that way we
// add a sibling (tilix N-way); otherwise we wrap the anchor in a new split node in its place. `before`
// puts fresh on the near side (left/top) vs far side (right/bottom). Shared by split() and moveLeaf().
void TerminalPanes::insertLeafBeside(Node *anchor, Node *fresh, int orient, bool before)
{
    Node *parent = anchor->parent;
    if (parent && parent->orientation == orient) {
        const int idx = parent->children.indexOf(anchor);
        const qreal share = parent->fractions.value(idx, 1.0) * 0.5;
        parent->fractions[idx] = share;
        const int at = before ? idx : idx + 1;
        parent->children.insert(at, fresh);
        parent->fractions.insert(at, share);
        fresh->parent = parent;
        rebuildDividers(parent);
    } else {
        Node *sp = new Node;
        sp->orientation = orient;
        sp->parent = parent;
        sp->children = before ? QVector<Node *>{ fresh, anchor } : QVector<Node *>{ anchor, fresh };
        sp->fractions = { 0.5, 0.5 };
        anchor->parent = sp;
        fresh->parent = sp;
        if (!parent) {
            m_root = sp;
        } else {
            const int idx = parent->children.indexOf(anchor);
            parent->children[idx] = sp;
        }
        rebuildDividers(sp);
    }
}

// Detach `leaf` from its parent split, renormalise the remaining shares, and collapse a split that's
// left with a single child (that child takes the split's place). The leaf NODE is left intact — the
// caller either deletes it (close) or recycles its live view (move). Assumes leaf->parent != null.
void TerminalPanes::unlinkLeaf(Node *leaf)
{
    Node *parent = leaf->parent;
    if (!parent)
        return;
    const int idx = parent->children.indexOf(leaf);
    parent->children.removeAt(idx);
    parent->fractions.removeAt(idx);
    leaf->parent = nullptr;

    qreal sum = 0;
    for (qreal f : parent->fractions) sum += f;
    if (sum > 0)
        for (qreal &f : parent->fractions) f /= sum;

    if (parent->children.size() == 1) {
        Node *only = parent->children.first();
        Node *grand = parent->parent;
        only->parent = grand;
        if (!grand) {
            m_root = only;
        } else {
            const int pidx = grand->children.indexOf(parent);
            grand->children[pidx] = only;
        }
        for (DividerItem *d : parent->dividers) if (d) d->deleteLater();
        delete parent;
    } else {
        rebuildDividers(parent);
    }
}

void TerminalPanes::split(int orient)
{
    if (!m_focused)
        return;
    m_zoomed = false; m_zoomLeaf = nullptr; // splitting exits zoom (tmux/tilix behaviour)
    Node *leaf = m_focused;
    Node *newLeaf = makeLeaf();
    if (!newLeaf)
        return;

    insertLeafBeside(leaf, newLeaf, orient, /*before=*/false);

    relayout();
    setFocused(newLeaf);
    // The cell's initial class is ["pane","pane-enter"] → the @keyframes plays on compose; strip the
    // trigger class after so a later re-resolve (theme change) doesn't replay it. When animations are
    // off, strip it immediately (no enter animation).
    QQuickItem *cell = newLeaf->cell;
    if (m_animateSplits) {
        QTimer::singleShot(kAnimMs, cell, [cell] {
            cell->setProperty("cssClass", QVariant::fromValue(QStringList{ QStringLiteral("pane") }));
        });
    } else {
        cell->setProperty("cssClass", QVariant::fromValue(QStringList{ QStringLiteral("pane") }));
    }
    emit panesChanged();
}

void TerminalPanes::removeLeaf(Node *leaf)
{
    if (!leaf || !leaf->isLeaf())
        return;
    m_zoomed = false; m_zoomLeaf = nullptr; // closing a pane exits zoom

    // Root is the only pane → close the app.
    if (leaf == m_root && leaf->children.isEmpty() && !leaf->parent) {
        deleteSubtree(m_root);
        m_root = nullptr;
        m_focused = nullptr;
        emit allClosed();
        return;
    }
    // Play the leave animation (if enabled), then actually detach when it finishes.
    if (m_animateSplits && leaf->cell)
        leaf->cell->setProperty("cssClass", QVariant::fromValue(QStringList{ QStringLiteral("pane"), QStringLiteral("pane-leave") }));
    TerminalView *v = leaf->view;
    QTimer::singleShot(m_animateSplits ? kAnimMs : 0, this, [this, v] {
        Node *l = leafOfView(v);
        if (!l)
            return;
        unlinkLeaf(l);
        if (l->view)
            l->view->deleteLater(); // view is a separate object now (setParent(this)), not owned by the cell
        if (l->cell)
            l->cell->deleteLater();
        delete l;

        // Refocus a surviving leaf (prefer one under the same subtree, else the first).
        QVector<Node *> leaves;
        collectLeaves(m_root, leaves);
        if (leaves.isEmpty()) {
            m_focused = nullptr;
            emit allClosed();
            return;
        }
        relayout();
        setFocused(leaves.first());
        emit panesChanged();
    });
}

// ─── header-drag rearrange ────────────────────────────────────────────────────────────────────────

TerminalPanes::Node *TerminalPanes::leafAtLocal(const QPointF &p) const
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    for (Node *l : leaves) {
        if (!l->cell)
            continue;
        const QRectF r(l->cell->x(), l->cell->y(), l->cell->width(), l->cell->height());
        if (r.contains(p))
            return l;
    }
    return nullptr;
}

// Which zone of a cell (w×h) a local point falls in: a centre square (swap) surrounded by four
// triangular edge zones (split). The dominant axis decides left/right vs top/bottom.
int TerminalPanes::zoneAt(const QPointF &inCell, qreal w, qreal h) const
{
    if (w <= 0 || h <= 0)
        return ZoneNone;
    const qreal nx = inCell.x() / w - 0.5; // [-0.5, 0.5]
    const qreal ny = inCell.y() / h - 0.5;
    if (qAbs(nx) < 0.18 && qAbs(ny) < 0.18)
        return ZoneCenter;
    if (qAbs(nx) > qAbs(ny))
        return nx < 0 ? ZoneLeft : ZoneRight;
    return ny < 0 ? ZoneTop : ZoneBottom;
}

QRectF TerminalPanes::zoneRect(Node *leaf, int zone) const
{
    if (!leaf || !leaf->cell)
        return {};
    const qreal x = leaf->cell->x(), y = leaf->cell->y();
    const qreal w = leaf->cell->width(), h = leaf->cell->height();
    switch (zone) {
    case ZoneLeft:   return QRectF(x, y, w / 2, h);
    case ZoneRight:  return QRectF(x + w / 2, y, w / 2, h);
    case ZoneTop:    return QRectF(x, y, w, h / 2);
    case ZoneBottom: return QRectF(x, y + h / 2, w, h / 2);
    case ZoneCenter: return QRectF(x + 6, y + 6, w - 12, h - 12);
    default:         return {};
    }
}

void TerminalPanes::beginPaneDrag(TerminalView *v)
{
    m_dragView = v;
    if (!m_dropOverlay) {
        if (QQmlEngine *eng = qmlEngine(this)) {
            QQmlComponent c(eng);
            c.setData(QByteArray(kDropQml), QUrl(QStringLiteral("qrc:/solidterm/DropZone.qml")));
            m_dropOverlay = qobject_cast<QQuickItem *>(c.create(qmlContext(this)));
            if (m_dropOverlay) {
                m_dropOverlay->setParent(this);
                m_dropOverlay->setParentItem(this);
            }
        }
    }
}

void TerminalPanes::updatePaneDrag(const QPointF &globalPos)
{
    if (!m_dragView)
        return;
    const QPointF local = mapFromGlobal(globalPos);
    Node *t = leafAtLocal(local);
    if (!t) {
        m_dropTarget = nullptr;
        m_dropZone = ZoneNone;
        if (m_dropOverlay) m_dropOverlay->setVisible(false);
        return;
    }
    const QPointF inCell(local.x() - t->cell->x(), local.y() - t->cell->y());
    int zone = zoneAt(inCell, t->cell->width(), t->cell->height());
    // Dropping a pane onto its own centre is a no-op → don't highlight it.
    Node *src = leafOfView(m_dragView);
    if (t == src && zone == ZoneCenter)
        zone = ZoneNone;
    m_dropTarget = t;
    m_dropZone = zone;
    if (m_dropOverlay) {
        if (zone == ZoneNone) {
            m_dropOverlay->setVisible(false);
        } else {
            const QRectF r = zoneRect(t, zone);
            m_dropOverlay->setX(r.x());
            m_dropOverlay->setY(r.y());
            m_dropOverlay->setWidth(r.width());
            m_dropOverlay->setHeight(r.height());
            m_dropOverlay->setParentItem(this); // keep on top of freshly-added cells
            m_dropOverlay->setVisible(true);
        }
    }
}

void TerminalPanes::endPaneDrag(const QPointF &globalPos)
{
    updatePaneDrag(globalPos); // settle target/zone on the final position
    TerminalView *v = m_dragView;
    Node *t = m_dropTarget;
    const int zone = m_dropZone;
    m_dragView = nullptr;
    m_dropTarget = nullptr;
    m_dropZone = ZoneNone;
    if (m_dropOverlay)
        m_dropOverlay->setVisible(false);

    if (!v || !t || zone == ZoneNone)
        return; // dropped on nothing / on itself → keep the layout (detach handled in a later step)
    Node *src = leafOfView(v);
    if (!src || src == t)
        return;
    if (zone == ZoneCenter)
        swapLeaves(v, t);
    else
        moveLeaf(v, t, zone);
}

void TerminalPanes::moveLeaf(TerminalView *v, Node *target, int side)
{
    Node *src = leafOfView(v);
    if (!src || !target || src == target || !src->isLeaf() || !target->isLeaf())
        return;
    m_zoomed = false; m_zoomLeaf = nullptr;

    // Drop the old cell + node, but DON'T null-parent the view first: makeLeaf() reparents it straight
    // from the old cell into the new one, so it never leaves the scene — and thus keeps its keyboard
    // focus (a null-parent gap would hand focus to a sibling and the reparented view can't reclaim it).
    QQuickItem *oldCell = src->cell;
    src->cell = nullptr;
    src->view = nullptr;
    src->header = nullptr;
    unlinkLeaf(src);
    if (m_focused == src)
        m_focused = nullptr; // never leave m_focused dangling — it's re-set to `moved` below
    delete src;
    if (oldCell)
        oldCell->deleteLater();

    // A fresh cell adopting the live view, spliced beside the target.
    Node *moved = makeLeaf(v);
    if (!moved)
        return;
    const int orient = (side == ZoneLeft || side == ZoneRight) ? Qt::Horizontal : Qt::Vertical;
    const bool before = (side == ZoneLeft || side == ZoneTop);
    insertLeafBeside(target, moved, orient, before);
    if (moved->cell) // a move isn't a fresh spawn → don't play the enter keyframes
        moved->cell->setProperty("cssClass", QVariant::fromValue(QStringList{ QStringLiteral("pane") }));

    relayout();
    setFocused(moved); // accent follows m_focused synchronously; keyboard focus re-affirms on the next tick
    emit panesChanged();
}

void TerminalPanes::swapLeaves(TerminalView *v, Node *target)
{
    Node *src = leafOfView(v);
    if (!src || !target || src == target)
        return;
    // Swap the whole payload (cell+view+header travel together); layout repositions each cell to its
    // node's new rect, so the two panes trade places. leafOfView scans by view pointer so wiring holds.
    std::swap(src->cell, target->cell);
    std::swap(src->view, target->view);
    std::swap(src->header, target->header);
    relayout();
    setFocused(leafOfView(v));
    emit panesChanged();
}

// ─── dividers + layout ──────────────────────────────────────────────────────────────────────────

void TerminalPanes::rebuildDividers(Node *splitNode)
{
    if (!splitNode || splitNode->isLeaf())
        return;
    for (DividerItem *d : splitNode->dividers)
        if (d) d->deleteLater();
    splitNode->dividers.clear();
    const bool horiz = splitNode->orientation == Qt::Horizontal;
    for (int i = 0; i + 1 < splitNode->children.size(); ++i) {
        auto *div = new DividerItem(horiz, this);
        div->color = m_handleColor;
        Node *node = splitNode;
        const int leftIdx = i;
        div->onDrag = [this, node, leftIdx](qreal d) {
            // Convert the pixel drag to a fraction of this split's main-axis extent, then move the
            // share from one neighbour to the other (clamped so neither collapses).
            const qreal delta = d / qMax<qreal>(1, node->lastAvail);
            qreal &a = node->fractions[leftIdx];
            qreal &b = node->fractions[leftIdx + 1];
            const qreal minF = 0.05;
            const qreal na = qBound(minF, a + delta, a + b - minF);
            b = a + b - na;
            a = na;
            relayout();
        };
        splitNode->dividers.append(div);
    }
}

void TerminalPanes::layoutNode(Node *node, qreal x, qreal y, qreal w, qreal h)
{
    if (!node)
        return;
    if (node->isLeaf()) {
        if (QQuickItem *cell = node->cell) {
            cell->setX(x); cell->setY(y);
            cell->setWidth(qMax<qreal>(0, w)); cell->setHeight(qMax<qreal>(0, h));
        }
        return;
    }
    const bool horiz = node->orientation == Qt::Horizontal;
    const int n = node->children.size();
    const qreal main = horiz ? w : h;
    const qreal available = qMax<qreal>(0, main - kDivider * (n - 1));
    node->lastAvail = available;

    qreal pos = horiz ? x : y;
    for (int i = 0; i < n; ++i) {
        const qreal len = available * node->fractions.value(i, 1.0 / n);
        if (horiz)
            layoutNode(node->children[i], pos, y, len, h);
        else
            layoutNode(node->children[i], x, pos, w, len);
        pos += len;
        if (i < n - 1) {
            DividerItem *div = node->dividers.value(i);
            if (div) {
                if (horiz) { div->setX(pos); div->setY(y); div->setWidth(kDivider); div->setHeight(h); }
                else { div->setX(x); div->setY(pos); div->setWidth(w); div->setHeight(kDivider); }
            }
            pos += kDivider;
        }
    }
}

void TerminalPanes::relayout()
{
    if (!m_root)
        return;
    // Zoom: only the zoomed leaf's cell is visible (fills the session); every other cell + all
    // dividers are hidden. Otherwise everything is visible and laid out by the tree.
    std::function<void(Node *)> setVis = [&](Node *n) {
        if (!n)
            return;
        if (n->isLeaf()) {
            if (n->cell)
                n->cell->setVisible(!m_zoomed || n == m_zoomLeaf);
            return;
        }
        for (DividerItem *d : n->dividers)
            if (d) d->setVisible(!m_zoomed);
        for (Node *c : n->children)
            setVis(c);
    };
    setVis(m_root);

    if (m_zoomed && m_zoomLeaf && m_zoomLeaf->cell) {
        m_zoomLeaf->cell->setX(0); m_zoomLeaf->cell->setY(0);
        m_zoomLeaf->cell->setWidth(width()); m_zoomLeaf->cell->setHeight(height());
    } else {
        layoutNode(m_root, 0, 0, width(), height());
    }

    // Number the panes in order (tilix "1: …, 2: …"); no number when there's only one.
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    for (int i = 0; i < leaves.size(); ++i)
        if (leaves[i]->header)
            leaves[i]->header->setIndex(leaves.size() > 1 ? i + 1 : 0);
}

void TerminalPanes::toggleZoom()
{
    if (m_zoomed) {
        m_zoomed = false;
        m_zoomLeaf = nullptr;
    } else {
        if (!m_focused)
            return;
        m_zoomed = true;
        m_zoomLeaf = m_focused;
    }
    relayout();
    if (Node *f = m_zoomLeaf ? m_zoomLeaf : m_focused)
        setFocused(f);
}

void TerminalPanes::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickItem::geometryChange(n, o);
    if (!isComponentComplete())
        return;
    relayout();
    // Grab keyboard focus once, when the panes are first laid out (the scene is ready now) — so
    // typing AND reserved accelerators work without a click to focus first.
    if (!m_didInitialFocus && width() > 0 && height() > 0 && m_focused) {
        m_didInitialFocus = true;
        setFocused(m_focused);
    }
}

// ─── focus ──────────────────────────────────────────────────────────────────────────────────────

void TerminalPanes::focusInEvent(QFocusEvent *)
{
    // Focus reaching the container (Tab, click on chrome) forwards to the focused pane so keys —
    // and reserved accelerators — always land on a terminal.
    if (m_focused && m_focused->view)
        m_focused->view->takeFocus();
}

void TerminalPanes::setFocused(Node *leaf)
{
    if (!leaf || !leaf->isLeaf() || !leaf->view)
        return;
    m_focused = leaf;
    leaf->view->takeFocus();
    refreshHeaderFocus();
    emit titleChanged(leaf->view->title());
}

void TerminalPanes::refreshHeaderFocus()
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    const bool multi = leaves.size() > 1;
    for (Node *l : leaves)
        if (l->header && l->view) {
            // Accent/dim track the LOGICAL focused pane (m_focused), not the async activeFocus — the
            // latter is stale right after a reparent (drag move) and drops when the window is unfocused.
            const bool focused = (l == m_focused);
            l->header->setFocused(focused);
            l->view->setDimmed(multi && !focused); // dim inactive panes when split
        }
}

QStringList TerminalPanes::paneTitles() const
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    QStringList out;
    out.reserve(leaves.size());
    for (Node *l : leaves)
        out.append(l->view ? l->view->title() : QString());
    return out;
}

int TerminalPanes::focusedPaneIndex() const
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    return leaves.indexOf(m_focused);
}

void TerminalPanes::focusLeafByIndex(int i)
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    if (i >= 0 && i < leaves.size())
        setFocused(leaves[i]);
}

void TerminalPanes::debugDragLeaf(int srcIdx, int dstIdx, int zone)
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    if (srcIdx < 0 || srcIdx >= leaves.size() || dstIdx < 0 || dstIdx >= leaves.size())
        return;
    Node *src = leaves[srcIdx];
    Node *dst = leaves[dstIdx];
    if (!src->view || !dst->cell)
        return;
    // Aim at the centre of the requested zone so zoneAt() classifies it the same way a real drop would.
    const QRectF r = zoneRect(dst, zone); // zone ints line up with DropZone (1=L 2=R 3=T 4=B 5=C)
    const QPointF center = r.isEmpty()
        ? QPointF(dst->cell->x() + dst->cell->width() / 2, dst->cell->y() + dst->cell->height() / 2)
        : r.center();
    const QPointF g = mapToGlobal(center);
    beginPaneDrag(src->view);
    updatePaneDrag(g);
    endPaneDrag(g);
}

void TerminalPanes::refocus()
{
    if (m_focused)
        setFocused(m_focused);
    else {
        QVector<Node *> leaves;
        collectLeaves(m_root, leaves);
        if (!leaves.isEmpty())
            setFocused(leaves.first());
    }
}

void TerminalPanes::closeFocused() { if (m_focused) removeLeaf(m_focused); }

void TerminalPanes::focusNext()
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    if (leaves.isEmpty())
        return;
    int i = qMax(0, leaves.indexOf(m_focused));
    setFocused(leaves[(i + 1) % leaves.size()]);
}

void TerminalPanes::focusPrev()
{
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    if (leaves.isEmpty())
        return;
    int i = qMax(0, leaves.indexOf(m_focused));
    setFocused(leaves[(i - 1 + leaves.size()) % leaves.size()]);
}

void TerminalPanes::copyFocused() { if (m_focused && m_focused->view) m_focused->view->copySelection(); }
void TerminalPanes::pasteFocused() { if (m_focused && m_focused->view) m_focused->view->pasteClipboard(); }
void TerminalPanes::pasteTextFocused(const QString &t) { if (m_focused && m_focused->view) m_focused->view->pasteText(t); }
void TerminalPanes::zoomFocused(int delta)
{
    if (!m_focused || !m_focused->view)
        return;
    // Per-pane: change ONLY the focused view's font size (delta 0 resets to the shared/pref size).
    // We deliberately don't touch m_fontSize, so the other panes and the saved pref are unaffected.
    TerminalView *v = m_focused->view;
    v->setFontSize(delta == 0 ? m_fontSize : qBound(6, v->fontSize() + delta, 40));
}
void TerminalPanes::clearFocused() { if (m_focused && m_focused->view) m_focused->view->clearScrollback(); }
void TerminalPanes::resetFocused() { if (m_focused && m_focused->view) m_focused->view->resetTerminal(); }
void TerminalPanes::setReadOnlyFocused(bool v) { if (m_focused && m_focused->view) m_focused->view->setReadOnly(v); }
void TerminalPanes::searchFocused(const QString &q) { if (m_focused && m_focused->view) m_focused->view->search(q); }
void TerminalPanes::searchNext() { if (m_focused && m_focused->view) m_focused->view->searchNext(); }
void TerminalPanes::searchPrev() { if (m_focused && m_focused->view) m_focused->view->searchPrev(); }
void TerminalPanes::clearSearch() { if (m_focused && m_focused->view) m_focused->view->clearSearch(); }

// ─── style ──────────────────────────────────────────────────────────────────────────────────────

void TerminalPanes::applyStyle(TerminalView *v)
{
    v->setFontFamily(m_fontFamily);
    v->setFontSize(m_fontSize);
    v->setBackground(m_background);
    v->setForeground(m_foreground);
    v->setScrollbackLimit(m_scrollbackLimit);
    v->setBackgroundImage(m_bgImage);
    v->setBackgroundOpacity(m_bgOpacity);
    v->setEmboss(m_emboss);
}

#define STYLE_SETTER(Setter, Member, Type) \
    void TerminalPanes::Setter(Type v) { \
        if (Member == v) return; \
        Member = v; emit styleChanged(); \
        QVector<Node *> leaves; collectLeaves(m_root, leaves); \
        for (Node *l : std::as_const(leaves)) if (l->view) applyStyle(l->view); \
    }
STYLE_SETTER(setFontFamily, m_fontFamily, const QString &)
STYLE_SETTER(setFontSize, m_fontSize, int)
STYLE_SETTER(setBackground, m_background, const QColor &)
STYLE_SETTER(setForeground, m_foreground, const QColor &)
STYLE_SETTER(setScrollbackLimit, m_scrollbackLimit, int)
STYLE_SETTER(setBackgroundImage, m_bgImage, const QString &)
STYLE_SETTER(setBackgroundOpacity, m_bgOpacity, qreal)
STYLE_SETTER(setEmboss, m_emboss, bool)
#undef STYLE_SETTER

void TerminalPanes::setReservedSequences(const QStringList &v)
{
    if (m_reserved == v)
        return;
    m_reserved = v;
    emit reservedChanged();
    QVector<Node *> leaves;
    collectLeaves(m_root, leaves);
    for (Node *l : std::as_const(leaves))
        if (l->view)
            l->view->setReservedSequences(v);
}

void TerminalPanes::setHandleColor(const QColor &v)
{
    if (m_handleColor == v)
        return;
    m_handleColor = v;
    emit styleChanged();
    // Recolour every divider and header across the tree.
    std::function<void(Node *)> walk = [&](Node *node) {
        if (!node) return;
        for (DividerItem *d : node->dividers)
            if (d) { d->color = v; d->update(); }
        if (node->header)
            node->header->setBackground(v);
        for (Node *c : node->children)
            walk(c);
    };
    walk(m_root);
}
