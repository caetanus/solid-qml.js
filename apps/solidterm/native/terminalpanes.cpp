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

struct TerminalPanes::Handle {
};

namespace {

// A thin draggable divider: a coloured rect that drags along one axis and reports pixel deltas.
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
    property alias view: __tv
    property alias header: __hdr
    property int headerH: 24
    cssPrimitive: "div"
    cssClass: ["pane", "pane-enter"]
    // A plain Item holds the header+view (anchored, so the engine's content pass never moves them);
    // the CssRect's transform (_animScale from @keyframes) scales the whole cell — content and all.
    Item {
        anchors.fill: parent
        PaneHeader { id: __hdr; x: 0; y: 0; width: parent.width; height: cell.headerH }
        TerminalView { id: __tv; x: 0; y: cell.headerH; width: parent.width; height: Math.max(0, parent.height - cell.headerH) }
    }
}
)";

} // namespace

TerminalPanes::TerminalPanes(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
}

QQuickItem *TerminalPanes::makeCell(TerminalView **outView, PaneHeader **outHeader)
{
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return nullptr;
    if (!m_cellComponent) {
        m_cellComponent = new QQmlComponent(eng, this);
        m_cellComponent->setData(QByteArray(kCellQml), QUrl(QStringLiteral("qrc:/solidterm/PaneCell.qml")));
    }
    auto *cell = qobject_cast<QQuickItem *>(m_cellComponent->create(qmlContext(this)));
    if (!cell) {
        qWarning("solidterm: pane cell failed: %s", qPrintable(m_cellComponent->errorString()));
        return nullptr;
    }
    cell->setParent(this);
    cell->setParentItem(this);
    cell->setProperty("headerH", kHeaderH);
    *outView = qobject_cast<TerminalView *>(cell->property("view").value<QQuickItem *>());
    *outHeader = qobject_cast<PaneHeader *>(cell->property("header").value<QQuickItem *>());
    return cell;
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
        if (m_panes.value(m_focused) == v)
            emit titleChanged(v->title());
    });
    connect(header, &PaneHeader::clicked, this, [this, v] {
        const int i = m_panes.indexOf(v);
        if (i >= 0) setFocusedIndex(i);
    });
    connect(header, &PaneHeader::closeRequested, this, [this, v] {
        const int i = m_panes.indexOf(v);
        if (i >= 0) removePane(i);
    });
    connect(v, &TerminalView::sessionFinished, this, [this, v] {
        const int idx = m_panes.indexOf(v);
        if (idx >= 0)
            removePane(idx);
    });
    connect(v, &QQuickItem::activeFocusChanged, this, [this] {
        for (int i = 0; i < m_panes.size(); ++i)
            m_headers[i]->setFocused(m_panes[i]->hasActiveFocus());
    });
    v->setReservedSequences(m_reserved);
    connect(v, &TerminalView::accelerator, this, &TerminalPanes::accelerator);
}

void TerminalPanes::componentComplete()
{
    QQuickItem::componentComplete();
    TerminalView *v = nullptr;
    PaneHeader *h = nullptr;
    QQuickItem *cell = makeCell(&v, &h);
    if (!cell)
        return;
    m_cells.append(cell);
    m_panes.append(v);
    m_headers.append(h);
    m_fractions.append(1.0);
    wirePane(v, h);
    rebuildDividers();
    relayout();
    setFocusedIndex(0);
}

void TerminalPanes::split(int orient)
{
    if (m_panes.size() == 1)
        setOrientation(orient);
    addPaneAfterFocused();
}

void TerminalPanes::addPaneAfterFocused()
{
    TerminalView *v = nullptr;
    PaneHeader *h = nullptr;
    QQuickItem *cell = makeCell(&v, &h);
    if (!cell)
        return;
    const int at = qBound(0, m_focused + 1, m_panes.size());
    m_cells.insert(at, cell);
    m_panes.insert(at, v);
    m_headers.insert(at, h);
    wirePane(v, h);

    const int n = m_panes.size();
    m_fractions.clear();
    for (int i = 0; i < n; ++i)
        m_fractions.append(1.0 / n);
    rebuildDividers();
    relayout();
    setFocusedIndex(at);
    // The cell's initial class is ["pane","pane-enter"] → the @keyframes plays on compose; strip
    // the trigger class after so a later re-resolve (theme change) doesn't replay it.
    QTimer::singleShot(kAnimMs, cell, [cell] {
        cell->setProperty("cssClass", QVariant::fromValue(QStringList{ QStringLiteral("pane") }));
    });
    emit panesChanged();
}

void TerminalPanes::removePane(int index)
{
    if (index < 0 || index >= m_panes.size())
        return;
    if (m_panes.size() == 1) {
        reallyRemove(index);
        return;
    }
    // Add the leave class → the CSS @keyframes shrinks it out, then delete when it finishes.
    QQuickItem *cell = m_cells.value(index);
    if (cell)
        cell->setProperty("cssClass", QVariant::fromValue(QStringList{ QStringLiteral("pane"), QStringLiteral("pane-leave") }));
    TerminalView *v = m_panes.value(index);
    QTimer::singleShot(kAnimMs, this, [this, v] {
        const int i = m_panes.indexOf(v);
        if (i >= 0)
            reallyRemove(i);
    });
}

void TerminalPanes::reallyRemove(int index)
{
    if (index < 0 || index >= m_panes.size())
        return;
    QQuickItem *cell = m_cells.takeAt(index);
    m_panes.removeAt(index);
    m_headers.removeAt(index);
    cell->deleteLater(); // takes the view + header with it (they're its children)
    if (m_panes.isEmpty()) {
        emit allClosed();
        return;
    }
    m_fractions.clear();
    const qreal share = 1.0 / m_panes.size();
    for (int i = 0; i < m_panes.size(); ++i)
        m_fractions.append(share);
    rebuildDividers();
    relayout();
    setFocusedIndex(qBound(0, index, m_panes.size() - 1));
    emit panesChanged();
}

// Dividers are PERSISTENT — recreated only when the pane COUNT changes, never mid-drag (that was
// the resize bug: relayout() deleted the divider being dragged and lost the mouse grab).
void TerminalPanes::rebuildDividers()
{
    for (QQuickItem *h : std::as_const(m_handles))
        h->deleteLater();
    m_handles.clear();
    const bool horiz = m_orientation == Qt::Horizontal;
    for (int i = 0; i + 1 < m_panes.size(); ++i) {
        auto *div = new DividerItem(horiz, this);
        div->color = m_handleColor;
        const int leftIdx = i;
        div->onDrag = [this, leftIdx](qreal d) {
            const bool h = m_orientation == Qt::Horizontal;
            const qreal mainLen = qMax<qreal>(1, (h ? width() : height()) - kDivider * (m_panes.size() - 1));
            const qreal delta = d / mainLen;
            qreal &a = m_fractions[leftIdx];
            qreal &b = m_fractions[leftIdx + 1];
            const qreal minF = 0.05;
            const qreal na = qBound(minF, a + delta, a + b - minF);
            b = a + b - na;
            a = na;
            relayout();
        };
        m_handles.append(div);
    }
}

void TerminalPanes::relayout()
{
    const bool horiz = m_orientation == Qt::Horizontal;
    const int n = m_panes.size();
    if (n == 0)
        return;
    const qreal main = horiz ? width() : height();
    const qreal cross = horiz ? height() : width();
    const qreal available = qMax<qreal>(0, main - kDivider * (n - 1));

    qreal pos = 0;
    for (int i = 0; i < n; ++i) {
        const qreal len = available * m_fractions.value(i, 1.0 / n);
        QQuickItem *cell = m_cells[i];
        // Position the CELL; its snippet splits itself into header + view internally.
        if (horiz) { cell->setX(pos); cell->setY(0); cell->setWidth(len); cell->setHeight(cross); }
        else { cell->setX(0); cell->setY(pos); cell->setWidth(cross); cell->setHeight(len); }
        pos += len;
        if (i < n - 1) {
            QQuickItem *div = m_handles.value(i);
            if (div) {
                if (horiz) { div->setX(pos); div->setY(0); div->setWidth(kDivider); div->setHeight(cross); }
                else { div->setX(0); div->setY(pos); div->setWidth(cross); div->setHeight(kDivider); }
            }
            pos += kDivider;
        }
    }
}

void TerminalPanes::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickItem::geometryChange(n, o);
    if (isComponentComplete())
        relayout();
}

void TerminalPanes::setFocusedIndex(int i)
{
    if (i < 0 || i >= m_panes.size())
        return;
    m_focused = i;
    m_panes[i]->takeFocus();
    for (int k = 0; k < m_headers.size(); ++k)
        m_headers[k]->setFocused(k == i);
    emit titleChanged(m_panes[i]->title());
}

int TerminalPanes::focusedIndex() const
{
    for (int i = 0; i < m_panes.size(); ++i)
        if (m_panes[i]->hasActiveFocus())
            return i;
    return m_focused;
}

void TerminalPanes::closeFocused() { removePane(focusedIndex()); }
void TerminalPanes::focusNext() { setFocusedIndex((focusedIndex() + 1) % qMax(1, m_panes.size())); }
void TerminalPanes::focusPrev() { setFocusedIndex((focusedIndex() - 1 + m_panes.size()) % qMax(1, m_panes.size())); }

void TerminalPanes::copyFocused() { if (auto *v = m_panes.value(focusedIndex())) v->copySelection(); }
void TerminalPanes::pasteFocused() { if (auto *v = m_panes.value(focusedIndex())) v->pasteClipboard(); }
void TerminalPanes::clearFocused() { if (auto *v = m_panes.value(focusedIndex())) v->clearScrollback(); }

void TerminalPanes::applyStyle(TerminalView *v)
{
    v->setFontFamily(m_fontFamily);
    v->setFontSize(m_fontSize);
    v->setBackground(m_background);
    v->setForeground(m_foreground);
    v->setScrollbackLimit(m_scrollbackLimit);
}

void TerminalPanes::setOrientation(int v)
{
    if (m_orientation == v)
        return;
    m_orientation = v;
    emit orientationChanged();
    if (isComponentComplete()) {
        rebuildDividers();
        relayout();
    }
}

#define STYLE_SETTER(Setter, Member, Type) \
    void TerminalPanes::Setter(Type v) { \
        if (Member == v) return; \
        Member = v; emit styleChanged(); \
        for (TerminalView *p : std::as_const(m_panes)) applyStyle(p); \
    }
STYLE_SETTER(setFontFamily, m_fontFamily, const QString &)
STYLE_SETTER(setFontSize, m_fontSize, int)
STYLE_SETTER(setBackground, m_background, const QColor &)
STYLE_SETTER(setForeground, m_foreground, const QColor &)
STYLE_SETTER(setScrollbackLimit, m_scrollbackLimit, int)
#undef STYLE_SETTER

void TerminalPanes::setReservedSequences(const QStringList &v)
{
    if (m_reserved == v)
        return;
    m_reserved = v;
    emit reservedChanged();
    for (TerminalView *p : std::as_const(m_panes))
        p->setReservedSequences(v);
}

void TerminalPanes::setHandleColor(const QColor &v)
{
    if (m_handleColor == v)
        return;
    m_handleColor = v;
    emit styleChanged();
    for (QQuickItem *h : std::as_const(m_handles))
        if (auto *d = static_cast<DividerItem *>(h)) { d->color = v; d->update(); }
    for (PaneHeader *h : std::as_const(m_headers))
        h->setBackground(v);
}
